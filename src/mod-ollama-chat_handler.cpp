#include "Log.h"
#include "Language.h"
#include "Player.h"
#include "Chat.h"
#include "Channel.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "BridgeBotState.h"
#include "ChatCommandLine.h"
#include "AcquaintanceMgr.h"
#include "PlayerbotAIConfig.h"
#include "Config.h"
#include "Common.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "ObjectAccessor.h"
#include "World.h"
#include "AiFactory.h"
#include "ChannelMgr.h"
#include <sstream>
#include <vector>
#include <list>
#include <string_view>
#include <unordered_map>
#include "Containers.h"
#include <fmt/core.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <algorithm>
#include <random>
#include <cctype>
#include <chrono>
#include <ctime>
#include "DatabaseEnv.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat-utilities.h"
#include "mod-ollama-chat_sentiment.h"
#include "mod-ollama-chat_rag.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_response.h"
#include "mod-ollama-chat_capability.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat_memory.h"
#include "mod-ollama-chat_topics.h"
#include "mod-ollama-chat_random.h"
#include "mod-ollama-chat_persona_bridge.h"
#include "mod-ollama-chat_persona_db.h"
#include <iomanip>
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "SharedDefines.h"
#include "Group.h"
#include "Creature.h"
#include "GameObject.h"
#include "ObjectMgr.h"
#include "QuestDef.h"

// For AzerothCore range checks
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Map.h"
#include "GridNotifiers.h"

// Forward declarations for internal helper functions.
static bool IsBotEligibleForChatChannelLocal(Player* bot, Player* player,
                                             ChatChannelSourceLocal source, Channel* channel = nullptr, Player* receiver = nullptr);

// Position of botName inside msg as a whole word, or npos. Case-insensitive.
// Hoisted out of ProcessChat because being named is also what makes a message
// direct address, which the governor needs to know.
static size_t OllamaFindBotNameMention(const std::string& msg, const std::string& botName)
{
    if (botName.empty() || msg.size() < botName.size())
        return std::string::npos;

    auto lower = [](const std::string& in)
    {
        std::string out = in;
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };

    const std::string lowerMsg     = lower(msg);
    const std::string lowerBotName = lower(botName);

    size_t pos = 0;
    while ((pos = lowerMsg.find(lowerBotName, pos)) != std::string::npos)
    {
        const bool validStart = (pos == 0 || !std::isalnum(static_cast<unsigned char>(lowerMsg[pos - 1])));
        const size_t endPos   = pos + lowerBotName.length();
        const bool validEnd   = (endPos >= lowerMsg.length() || !std::isalnum(static_cast<unsigned char>(lowerMsg[endPos])));

        if (validStart && validEnd)
            return pos;
        ++pos;
    }
    return std::string::npos;
}

// Is this line aimed at this bot in particular, rather than said to the room?
//
// Direct address skips the pacing cooldowns in the governor. Those exist to
// stop ambient chatter running hot; applied to a conversation they make a bot
// answer once and then ignore the next several things said to it, which reads
// as broken rather than as rate limiting.
//
// Four things count:
//   * a whisper -- the bot is the only recipient
//   * being named -- "Uynya, what do you think?"
//   * an open conversation -- this bot already answered this person here, so
//     the next thing they say is a turn in that exchange. Without this a bot
//     answers the opening line and then goes quiet unless it is named again
//     every single message, which is the same bug one step later.
//   * party/raid chat from a person, in a small group the bot belongs to --
//     a five-person party is a conversation, not a crowd. Above
//     DirectAddressGroupSize members it is a crowd again and pacing returns.
static bool OllamaIsDirectAddress(Player* bot, Player* speaker, ChatChannelSourceLocal source,
                                  const std::string& msg, bool senderIsBot,
                                  const std::string& scopeKey)
{
    if (source == SRC_WHISPER_LOCAL)
        return true;

    if (!bot)
        return false;

    if (OllamaFindBotNameMention(msg, bot->GetName()) != std::string::npos)
        return true;

    if (senderIsBot)
        return false;   // only a person's turn obliges an answer

    // Already talking to this person here.
    if (speaker && Governor_InConversation(bot->GetGUID(), speaker->GetGUID(), scopeKey))
        return true;

    if (source != SRC_PARTY_LOCAL && source != SRC_RAID_LOCAL)
        return false;

    if (g_DirectAddressGroupSize == 0)
        return false;

    Group* group = bot->GetGroup();
    if (!group || !speaker || speaker->GetGroup() != group)
        return false;

    return group->GetMembersCount() <= g_DirectAddressGroupSize;
}

namespace
{
    // Unlike Acore::AnyUnitInObjectRangeCheck this keeps dead creatures --
    // a fresh corpse is worth commenting on.
    struct OllamaNearbyCreatureCheck
    {
        OllamaNearbyCreatureCheck(WorldObject const* obj, float range)
            : _obj(obj), _range(range) { }

        bool operator()(Creature* c) const
        {
            return c && _obj->IsWithinDistInMap(c, _range);
        }

        WorldObject const* _obj;
        float              _range;
    };
}

// Helper function to format class name for any player
static std::string FormatPlayerClass(uint8_t classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR:      return "Warrior";
        case CLASS_PALADIN:      return "Paladin";
        case CLASS_HUNTER:       return "Hunter";
        case CLASS_ROGUE:        return "Rogue";
        case CLASS_PRIEST:       return "Priest";
        case CLASS_DEATH_KNIGHT: return "Death Knight";
        case CLASS_SHAMAN:       return "Shaman";
        case CLASS_MAGE:         return "Mage";
        case CLASS_WARLOCK:      return "Warlock";
        case CLASS_DRUID:        return "Druid";
        default:                 return "Unknown";
    }
}

// Helper function to format race name for any player
static std::string FormatPlayerRace(uint8_t raceId)
{
    switch (raceId)
    {
        case RACE_HUMAN:         return "Human";
        case RACE_ORC:           return "Orc";
        case RACE_DWARF:         return "Dwarf";
        case RACE_NIGHTELF:      return "Night Elf";
        case RACE_UNDEAD_PLAYER: return "Undead";
        case RACE_TAUREN:        return "Tauren";
        case RACE_GNOME:         return "Gnome";
        case RACE_TROLL:         return "Troll";
        case RACE_BLOODELF:      return "Blood Elf";
        case RACE_DRAENEI:       return "Draenei";
        default:                 return "Unknown";
    }
}

const char* ChatChannelSourceLocalStr[] =
{
    "Undefined",  // 0
    "Say",        // 1
    "Party",      // 2
    "Raid",       // 3
    "Guild",      // 4
    "Officer",    // 5
    "Yell",       // 6
    "Whisper",    // 7
    "Unknown8",   // 8
    "Unknown9",   // 9
    "Unknown10",  // 10
    "Unknown11",  // 11
    "Unknown12",  // 12
    "Unknown13",  // 13
    "Unknown14",  // 14
    "Unknown15",  // 15
    "Unknown16",  // 16
    "General"     // 17
};

std::string GetConversationEntryKey(uint64_t botGuid, uint64_t playerGuid, const std::string& playerMessage, const std::string& botReply)
{
    // Use a combination that guarantees uniqueness
    return SafeFormat("{}:{}:{}:{}", botGuid, playerGuid, playerMessage, botReply);
}

std::string rtrim(const std::string& s)
{
    const std::string whitespace = " \t\n\r,.!?;:";
    size_t end = s.find_last_not_of(whitespace);
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

ChatChannelSourceLocal GetChannelSourceLocal(uint32_t type)
{
    switch (type)
    {
        case CHAT_MSG_SAY:
            return SRC_SAY_LOCAL;
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
            return SRC_PARTY_LOCAL;
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
        case CHAT_MSG_RAID_WARNING:
            return SRC_RAID_LOCAL;
        case CHAT_MSG_GUILD:
            return SRC_GUILD_LOCAL;
        case CHAT_MSG_OFFICER:
            return SRC_OFFICER_LOCAL;
        case CHAT_MSG_YELL:
            return SRC_YELL_LOCAL;
        case CHAT_MSG_WHISPER:
        case CHAT_MSG_WHISPER_FOREIGN:
        case CHAT_MSG_WHISPER_INFORM:
            return SRC_WHISPER_LOCAL;
        case CHAT_MSG_CHANNEL:
            return SRC_GENERAL_LOCAL;
        default:
            return SRC_UNDEFINED_LOCAL;
    }
}

Channel* GetValidChannel(uint32_t teamId, const std::string& channelName, Player* player)
{
    ChannelMgr* cMgr = ChannelMgr::forTeam(static_cast<TeamId>(teamId));
    Channel* channel = cMgr->GetChannel(channelName, player);
    if (!channel)
    {
        if(g_DebugEnabled)
        {
            LOG_ERROR("module.ollamachat", "[Ollama Chat] Channel '{}' not found for team {}", channelName, teamId);
        }
    }
    return channel;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg)
{
    if (!g_Enable)
        return true;

    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, nullptr, nullptr);
    return true;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Group* /*group*/)
{
    if (!g_Enable)
        return true;

    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, nullptr, nullptr);
    return true;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Guild* /*guild*/)
{
    if (!g_Enable)
        return true;

    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, nullptr, nullptr);
    return true;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel)
{
    if (!g_Enable)
        return true;

    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, channel, nullptr);
    return true;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Player* receiver)
{
    // Only process if our module is enabled
    if (!g_Enable)
        return true;

    if (type == CHAT_MSG_WHISPER)
    {
        // Check if this is a valid whisper to a bot
        if (!receiver || !player || player == receiver)
            return true;

        // Check if sender is a bot - if so, don't trigger Ollama responses for bot-to-bot whispers
        PlayerbotAI* senderAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);
        if (senderAI && senderAI->IsBotAI())
        {
            return true;
        }

        PlayerbotAI* receiverAI = PlayerbotsMgr::instance().GetPlayerbotAI(receiver);
        if (!receiverAI || !receiverAI->IsBotAI())
            return true;
    }

    if (g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] OnPlayerCanUseChat called: player={}, type={}, receiver={}",
            player->GetName(), type, receiver ? receiver->GetName() : "null");
    }

    // Process the chat immediately in OnPlayerCanUseChat to prevent double processing
    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, nullptr, receiver);

    // Return false to prevent the message from being processed again in OnPlayerChat
    return true;
}

void AppendBotConversation(uint64_t botGuid, uint64_t playerGuid, const std::string& playerMessage, const std::string& botReply)
{
    std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);
    auto& playerHistory = g_BotConversationHistory[botGuid][playerGuid];
    playerHistory.push_back({ playerMessage, botReply, /*persisted*/ false });
    while (playerHistory.size() > g_MaxConversationHistory)
    {
        playerHistory.pop_front();
    }

}

namespace
{
    // A multi-row INSERT is one statement for the database worker instead of
    // one per turn, but it still has to fit inside max_allowed_packet. Flush
    // well short of the 4MB older servers default to.
    constexpr size_t kHistoryInsertMaxBytes = 512 * 1024;
}

void SaveBotConversationHistoryToDB()
{
    // Gathered under the lock, written outside it. Escaping and statement
    // building have no business holding a mutex that reply delivery takes on
    // the world thread.
    struct PendingPair
    {
        uint64_t                                         botGuid;
        uint64_t                                         playerGuid;
        std::vector<std::pair<std::string, std::string>> turns;
    };
    std::vector<PendingPair> pending;

    {
        std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);

        for (auto& [botGuid, playerMap] : g_BotConversationHistory)
        {
            for (auto& [playerGuid, history] : playerMap)
            {
                PendingPair entry{ botGuid, playerGuid, {} };

                for (BotConversationEntry& turn : history)
                {
                    // The whole point of the flag: this used to re-INSERT
                    // IGNORE every cached turn of every pair on every save,
                    // and let the unique key throw the duplicates away after
                    // MySQL had already done the index probe for each one.
                    if (turn.persisted)
                        continue;

                    entry.turns.emplace_back(turn.playerMessage, turn.botReply);

                    // Marked before the write lands. A dropped row costs one
                    // line of remembered chatter; retrying every turn forever
                    // is the behaviour being removed here.
                    turn.persisted = true;
                }

                if (!entry.turns.empty())
                    pending.push_back(std::move(entry));
            }
        }
    }

    if (pending.empty())
        return;

    // A configured 0 would run the trim below with OFFSET -1.
    const uint32_t keep   = std::max<uint32_t>(g_MaxConversationHistory, 1);
    const uint32_t offset = keep - 1;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint32_t savedTurns = 0;

    for (const PendingPair& entry : pending)
    {
        std::string values;

        auto flushValues = [&]()
        {
            if (values.empty())
                return;

            trans->Append("INSERT IGNORE INTO mod_ollama_chat_history "
                          "(bot_guid, player_guid, timestamp, player_message, bot_reply) VALUES " + values);
            values.clear();
        };

        for (const auto& [playerMessage, botReply] : entry.turns)
        {
            std::string escPlayerMsg = playerMessage;
            CharacterDatabase.EscapeString(escPlayerMsg);

            std::string escBotReply = botReply;
            CharacterDatabase.EscapeString(escBotReply);

            if (!values.empty())
                values += ',';

            values += SafeFormat("({}, {}, NOW(), '{}', '{}')",
                                 entry.botGuid, entry.playerGuid, escPlayerMsg, escBotReply);
            ++savedTurns;

            if (values.size() >= kHistoryInsertMaxBytes)
                flushValues();
        }

        flushValues();

        // Trim this pair to its newest `keep` rows.
        //
        // This replaces a ROW_NUMBER() window function evaluated over the
        // whole table on every save, whose DELETE then matched rows by
        // (bot_guid, player_guid, timestamp). That tuple has no index, and
        // every row written in one save batch shares the same
        // second-granularity NOW() -- so the delete matched the entire batch,
        // not just the surplus, and quietly wiped whole conversations.
        //
        // Ordering by the auto-increment id is exact and is served end to end
        // by idx_pair_recent (bot_guid, player_guid, id). Only pairs that
        // gained a row are trimmed; the rest of the table is never touched.
        trans->Append(SafeFormat(
            "DELETE FROM mod_ollama_chat_history "
            "WHERE bot_guid = {0} AND player_guid = {1} AND id < ("
                "SELECT keep_id FROM ("
                    "SELECT id AS keep_id FROM mod_ollama_chat_history "
                    "WHERE bot_guid = {0} AND player_guid = {1} "
                    "ORDER BY id DESC LIMIT 1 OFFSET {2}"
                ") AS oldest_kept"
            ")",
            entry.botGuid, entry.playerGuid, offset));
    }

    CharacterDatabase.CommitTransaction(trans);

    if (g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat",
                 "[Ollama Chat] Saved {} new conversation turn(s) across {} bot/player pair(s).",
                 savedTurns, static_cast<uint32_t>(pending.size()));
    }
}

// Drop a bot's persisted history. Called from a worker thread after its
// conversation has been condensed into long-term memories -- without this the
// rows survive, get reloaded on the next startup, and are condensed again.
//
// Worker-safe: Execute() queues, and the statement is a literal plus an
// integer, so no config string is read off the world thread.
void DeleteBotConversationHistoryFromDB(uint64_t botGuid)
{
    CharacterDatabase.Execute(SafeFormat(
        "DELETE FROM mod_ollama_chat_history WHERE bot_guid = {}", botGuid));
}

// Called when a bot sends a message (random chatter or other bot-initiated messages)
// This triggers other bots to potentially reply
void ProcessBotChatMessage(Player* bot, const std::string& msg, ChatChannelSourceLocal sourceLocal, Channel* channel, uint8_t chainDepth)
{
    if (!bot || msg.empty())
        return;

    // Bail before the (not cheap) eligibility validation below when the chain
    // is already spent.
    if (!Governor_ChainDepthAllowed(chainDepth))
        return;

        
    // If channel is nullptr but this is a channel-type message, try to find the channel
    if (!channel && sourceLocal == SRC_GENERAL_LOCAL)
    {
        // Look up the General channel for this bot's faction
        std::string channelName = "General";
        ChannelMgr* cMgr = ChannelMgr::forTeam(bot->GetTeamId());
        if (cMgr)
        {
            channel = cMgr->GetChannel(channelName, bot);
            if (g_DebugEnabled)
            {
                if (channel)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] ProcessBotChatMessage: Found General channel for bot {}", bot->GetName());
                else
                    LOG_ERROR("module.ollamachat", "[Ollama Chat] ProcessBotChatMessage: Could not find General channel for bot {}", bot->GetName());
            }
        }
    }
    
    // Whether other bots may chain a reply to this line. The line itself has
    // already been delivered by this point -- this only gates propagation.
    bool canSendMessage = false;
    switch (sourceLocal)
    {
        case SRC_SAY_LOCAL:
        case SRC_YELL_LOCAL:
            // Distance checks will be applied during eligibility filtering
            canSendMessage = true;
            break;
            
        case SRC_GENERAL_LOCAL:
            // Must have a channel object
            canSendMessage = (channel != nullptr);
            if (!canSendMessage && g_DebugEnabled)
                LOG_ERROR("module.ollamachat", "[Ollama Chat] No bot replies to {} in General - no channel found", bot->GetName());
            break;
            
        case SRC_GUILD_LOCAL:
        case SRC_OFFICER_LOCAL:
            // Must be in a guild with at least one real player online
            if (bot->GetGuildId() != 0)
            {
                Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId());
                if (guild)
                {
                    OllamaWorldSnapshot world;
                    world.Build();
                    const bool hasRealPlayer = world.GuildHasRealPlayer(bot->GetGuildId());
                    canSendMessage = hasRealPlayer;
                    if (!canSendMessage && g_DebugEnabled)
                        LOG_INFO("module.ollamachat", "[Ollama Chat] No bot replies to {} in Guild - no real players online in guild", bot->GetName());
                }
                else
                {
                    canSendMessage = false;
                    if (g_DebugEnabled)
                        LOG_ERROR("module.ollamachat", "[Ollama Chat] No bot replies to {} in Guild - guild not found", bot->GetName());
                }
            }
            else
            {
                canSendMessage = false;
                if (g_DebugEnabled)
                    LOG_ERROR("module.ollamachat", "[Ollama Chat] No bot replies to {} in Guild - not in a guild", bot->GetName());
            }
            break;
            
        case SRC_PARTY_LOCAL:
        case SRC_RAID_LOCAL:
            // Must be in a group with at least one real player
            if (bot->GetGroup())
            {
                Group* group = bot->GetGroup();
                bool hasRealPlayer = false;
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    if (OllamaIsRealPlayer(member))
                    {
                        hasRealPlayer = true;
                        break;
                    }
                }
                canSendMessage = hasRealPlayer;
                if (!canSendMessage && g_DebugEnabled)
                    LOG_INFO("module.ollamachat", "[Ollama Chat] No bot replies to {} in Party - no real players in group", bot->GetName());
            }
            else
            {
                canSendMessage = false;
                if (g_DebugEnabled)
                    LOG_ERROR("module.ollamachat", "[Ollama Chat] No bot replies to {} in Party - not in a group", bot->GetName());
            }
            break;
            
        case SRC_WHISPER_LOCAL:
            // Whispers are handled separately
            canSendMessage = true;
            break;
            
        default:
            canSendMessage = true;
            break;
    }
    
    if (!canSendMessage)
    {
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat",
                     "[Ollama Chat] Not propagating {}'s {} line to other bots - no audience.",
                     bot->GetName(), ChatChannelSourceLocalStr[sourceLocal]);
        return;
    }
        
    // Convert ChatChannelSourceLocal back to chat type for ProcessChat
    uint32_t type = 0;
    switch (sourceLocal)
    {
        case SRC_SAY_LOCAL: type = CHAT_MSG_SAY; break;
        case SRC_YELL_LOCAL: type = CHAT_MSG_YELL; break;
        case SRC_PARTY_LOCAL: type = CHAT_MSG_PARTY; break;
        case SRC_RAID_LOCAL: type = CHAT_MSG_RAID; break;
        case SRC_GUILD_LOCAL: type = CHAT_MSG_GUILD; break;
        case SRC_OFFICER_LOCAL: type = CHAT_MSG_OFFICER; break;
        case SRC_WHISPER_LOCAL: type = CHAT_MSG_WHISPER; break;
        case SRC_GENERAL_LOCAL: type = CHAT_MSG_CHANNEL; break;
        default: type = CHAT_MSG_SAY; break;
    }
    
    std::string mutableMsg = msg; // ProcessChat takes non-const reference
    uint32_t lang = bot->GetTeamId() == TEAM_ALLIANCE ? LANG_COMMON : LANG_ORCISH;
    
    // Call the main ProcessChat function with bot as sender
    PlayerBotChatHandler::ProcessChat(bot, type, lang, mutableMsg, sourceLocal, channel, nullptr, chainDepth);
}

std::string GetBotHistoryPrompt(uint64_t botGuid, uint64_t playerGuid, std::string playerMessage)
{
    if(!g_EnableChatHistory)
    {
        return "";
    }
    
    std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);

    std::string result;
    const auto botIt = g_BotConversationHistory.find(botGuid);
    if (botIt == g_BotConversationHistory.end())
        return result;
    const auto playerIt = botIt->second.find(playerGuid);
    if (playerIt == botIt->second.end())
        return result;

    Player* player = ObjectAccessor::FindPlayer(ObjectGuid(playerGuid));
    std::string playerName = player ? player->GetName() : "The player";

    result += SafeFormat(g_ChatHistoryHeaderTemplate, fmt::arg("player_name", playerName));

    for (const auto& entry : playerIt->second) {
        result += SafeFormat(g_ChatHistoryLineTemplate,
            fmt::arg("player_name", playerName),
            fmt::arg("player_message", entry.playerMessage),
            fmt::arg("bot_reply", entry.botReply)
        );
    }

    result += SafeFormat(g_ChatHistoryFooterTemplate,
        fmt::arg("player_name", playerName),
        fmt::arg("player_message", playerMessage)
    );

    return result;
}

// --- Helper: Spells ---
std::string ChatHandler_GetBotSpellInfo(Player* bot)
{
    // Map to store highest rank of each spell: spell name -> (spellId, rank, costText)
    std::map<std::string, std::tuple<uint32, uint32, std::string>> uniqueSpells;
    
    for (const auto& spellPair : bot->GetSpellMap())
    {
        uint32 spellId = spellPair.first;
        const SpellInfo* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->Attributes & SPELL_ATTR0_PASSIVE)
            continue;
        if (spellInfo->SpellFamilyName == SPELLFAMILY_GENERIC)
            continue;
        if (bot->HasSpellCooldown(spellId))
            continue;
        
        const char* name = spellInfo->SpellName[0];
        if (!name || !*name)
            continue;
        
        std::string costText;
        if (spellInfo->ManaCost || spellInfo->ManaCostPercentage)
        {
            switch (spellInfo->PowerType)
            {
                case POWER_MANA: costText = std::to_string(spellInfo->ManaCost) + " mana"; break;
                case POWER_RAGE: costText = std::to_string(spellInfo->ManaCost) + " rage"; break;
                case POWER_FOCUS: costText = std::to_string(spellInfo->ManaCost) + " focus"; break;
                case POWER_ENERGY: costText = std::to_string(spellInfo->ManaCost) + " energy"; break;
                case POWER_RUNIC_POWER: costText = std::to_string(spellInfo->ManaCost) + " runic power"; break;
                default: costText = std::to_string(spellInfo->ManaCost) + " unknown resource"; break;
            }
        }
        else
        {
            costText = "no cost";
        }
        
        // Get base spell name (without rank)
        std::string spellName = name;
        uint32 rank = spellInfo->GetRank();
        
        // Check if we already have this spell, and if so, only keep the highest rank
        auto it = uniqueSpells.find(spellName);
        if (it == uniqueSpells.end())
        {
            // First time seeing this spell
            uniqueSpells[spellName] = std::make_tuple(spellId, rank, costText);
        }
        else
        {
            // We've seen this spell before, check if this is a higher rank
            uint32 existingRank = std::get<1>(it->second);
            if (rank > existingRank)
            {
                // Replace with higher rank
                uniqueSpells[spellName] = std::make_tuple(spellId, rank, costText);
            }
        }
    }
    
    // Cap the list. Dumping every off-cooldown spell a level 80 bot knows put
    // dozens of lines of the most concrete, most quotable text into the prompt
    // -- which is precisely why bots ended up reciting their spellbook instead
    // of talking about the world around them.
    if (g_SnapshotMaxSpells == 0)
        return "";

    std::vector<std::string> picked;
    picked.reserve(uniqueSpells.size());

    for (const auto& [spellName, spellData] : uniqueSpells)
    {
        const uint32 rank = std::get<1>(spellData);
        const std::string& costText = std::get<2>(spellData);

        std::string line = spellName;
        if (rank > 0)
            line += " (Rank " + std::to_string(rank) + ")";
        line += " - " + costText;

        picked.push_back(std::move(line));
    }

    // Shuffle so a bot that does mention a spell is not always mentioning the
    // alphabetically-first one it knows.
    if (picked.size() > g_SnapshotMaxSpells)
    {
        Acore::Containers::RandomShuffle(picked);
        picked.resize(g_SnapshotMaxSpells);
    }

    std::ostringstream spellSummary;
    for (const std::string& line : picked)
        spellSummary << line << "\n";

    return spellSummary.str();
}

// --- Helper: Group info ---
std::vector<std::string> ChatHandler_GetGroupStatus(Player* bot)
{
    std::vector<std::string> info;
    if (!bot || !bot->GetGroup()) return info;
    Group* group = bot->GetGroup();
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->GetMap()) continue;
        if(bot == member) continue;
        float dist = bot->GetDistance(member);
        std::string beingAttacked = "";
        if (Unit* attacker = member->GetVictim())
        {
            beingAttacked = " [Under Attack by " + attacker->GetName() +
                            ", Level: " + std::to_string(attacker->GetLevel()) + ", HP: " + std::to_string(attacker->GetHealth()) +
                            "/" + std::to_string(attacker->GetMaxHealth()) + ")]";
        }
        std::string className = FormatPlayerClass(member->getClass());
        std::string raceName = FormatPlayerRace(member->getRace());
        info.push_back(
            member->GetName() +
            " (Level: " + std::to_string(member->GetLevel()) +
            ", Class: " + className +
            ", Race: " + raceName +
            ", HP: " + std::to_string(member->GetHealth()) + "/" + std::to_string(member->GetMaxHealth()) +
            ", Dist: " + std::to_string(dist) + ")" + beingAttacked
        );

    }
    return info;
}

// --- Helper: Visible players ---
std::vector<std::string> ChatHandler_GetVisiblePlayers(Player* bot, float radius = 40.0f)
{
    std::vector<std::string> players;
    if (!bot || !bot->GetMap())
        return players;

    // Grid search rather than a walk of every online character on the realm.
    std::list<Player*> found;
    Acore::AnyPlayerInObjectRangeCheck check(bot, radius, false, true);
    Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, found, check);
    Cell::VisitObjects(bot, searcher, radius);

    std::vector<std::pair<float, std::string>> scored;
    for (Player* player : found)
    {
        if (!player || player == bot || !player->IsInWorld())
            continue;
        if (!bot->IsWithinLOSInMap(player))
            continue;

        const float dist = bot->GetDistance(player);
        scored.emplace_back(dist, SafeFormat(
            "Player: {} (Level {}, {} {}, {}, {:.0f} yards)",
            player->GetName(), player->GetLevel(),
            FormatPlayerRace(player->getRace()), FormatPlayerClass(player->getClass()),
            player->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde", dist));
    }

    std::sort(scored.begin(), scored.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });

    uint32_t taken = 0;
    for (auto const& entry : scored)
    {
        if (g_SnapshotMaxPlayers > 0 && taken >= g_SnapshotMaxPlayers)
            break;
        players.push_back(entry.second);
        ++taken;
    }

    return players;
}

// --- Helper: Visible locations/objects (creatures and gameobjects) ---
std::vector<std::string> ChatHandler_GetVisibleLocations(Player* bot, float radius = 40.0f)
{
    std::vector<std::string> visible;
    if (!bot || !bot->GetMap())
        return visible;

    // Grid search, not a walk of every spawn on the map. The old version
    // iterated Map::GetCreatureBySpawnIdStore() -- tens of thousands of
    // entries in Northrend -- with a LOS raycast per candidate, on the map
    // thread, for every prompt built.
    std::list<Creature*> creatures;
    OllamaNearbyCreatureCheck creatureCheck(bot, radius);
    Acore::CreatureListSearcher<OllamaNearbyCreatureCheck> creatureSearcher(bot, creatures, creatureCheck);
    Cell::VisitObjects(bot, creatureSearcher, radius);

    std::vector<std::pair<float, std::string>> scored;
    scored.reserve(creatures.size());

    for (Creature* c : creatures)
    {
        if (!c || c->IsPet() || c->IsTotem())
            continue;
        if (!bot->IsWithinLOSInMap(c))
            continue;

        std::string type;
        if (c->isDead())              type = "DEAD";
        else if (c->IsHostileTo(bot)) type = "ENEMY";
        else if (c->IsFriendlyTo(bot))type = "FRIENDLY";
        else                          type = "NEUTRAL";

        const float dist = bot->GetDistance(c);
        scored.emplace_back(dist, SafeFormat("{}: {} (Level {}, HP {}/{}, {:.0f} yards)",
                                             type, c->GetName(), c->GetLevel(),
                                             c->GetHealth(), c->GetMaxHealth(), dist));
    }

    std::sort(scored.begin(), scored.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });

    uint32_t taken = 0;
    for (auto const& entry : scored)
    {
        if (g_SnapshotMaxCreatures > 0 && taken >= g_SnapshotMaxCreatures)
            break;
        visible.push_back(entry.second);
        ++taken;
    }

    std::list<GameObject*> objects;
    Acore::GameObjectInRangeCheck goCheck(bot->GetPositionX(), bot->GetPositionY(),
                                          bot->GetPositionZ(), radius);
    Acore::GameObjectListSearcher<Acore::GameObjectInRangeCheck> goSearcher(bot, objects, goCheck);
    Cell::VisitObjects(bot, goSearcher, radius);

    std::vector<std::pair<float, std::string>> goScored;
    for (GameObject* go : objects)
    {
        if (!go || go->GetName().empty())
            continue;
        if (!bot->IsWithinLOSInMap(go))
            continue;

        const float dist = bot->GetDistance(go);
        goScored.emplace_back(dist, SafeFormat("{} ({:.0f} yards)", go->GetName(), dist));
    }

    std::sort(goScored.begin(), goScored.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });

    taken = 0;
    for (auto const& entry : goScored)
    {
        if (g_SnapshotMaxObjects > 0 && taken >= g_SnapshotMaxObjects)
            break;
        visible.push_back(entry.second);
        ++taken;
    }

    return visible;
}

// --- Helper: Combat summary ---
std::string ChatHandler_GetCombatSummary(Player* bot)
{
    std::ostringstream oss;
    bool inCombat = bot->IsInCombat();
    Unit* victim = bot->GetVictim();

    // Class-specific resource reporting
    auto classId = bot->getClass();

    auto printResource = [&](std::ostringstream& oss) {
        switch (classId)
        {
            case CLASS_WARRIOR:
                oss << ", Rage: " << bot->GetPower(POWER_RAGE) << "/" << bot->GetMaxPower(POWER_RAGE);
                break;
            case CLASS_ROGUE:
                oss << ", Energy: " << bot->GetPower(POWER_ENERGY) << "/" << bot->GetMaxPower(POWER_ENERGY);
                break;
            case CLASS_DEATH_KNIGHT:
                oss << ", Runic Power: " << bot->GetPower(POWER_RUNIC_POWER) << "/" << bot->GetMaxPower(POWER_RUNIC_POWER);
                break;
            case CLASS_HUNTER:
                oss << ", Focus: " << bot->GetPower(POWER_FOCUS) << "/" << bot->GetMaxPower(POWER_FOCUS);
                break;
            default: // Mana classes
                if (bot->GetMaxPower(POWER_MANA) > 0)
                    oss << ", Mana: " << bot->GetPower(POWER_MANA) << "/" << bot->GetMaxPower(POWER_MANA);
                break;
        }
    };

    if (inCombat)
    {
        oss << "IN COMBAT: ";
        if (victim)
        {
            oss << "Target: " << victim->GetName()
                << ", Level: " << victim->GetLevel()
                << ", HP: " << victim->GetHealth() << "/" << victim->GetMaxHealth();
        }
        else
        {
            oss << "No current target";
        }
        oss << ". ";
        printResource(oss);
    }
    else
    {
        oss << "NOT IN COMBAT. ";
        printResource(oss);
    }
    return oss.str();
}


std::string GenerateBotGameStateSnapshot(Player* bot)
{
    // Prepare each section
    std::string combat = ChatHandler_GetCombatSummary(bot);

    std::string group;
    std::vector<std::string> groupInfo = ChatHandler_GetGroupStatus(bot);
    if (!groupInfo.empty()) {
        group += "Group members:\n";
        for (const auto& entry : groupInfo) group += " - " + entry + "\n";
    }

    std::string spells = g_SnapshotIncludeSpells ? ChatHandler_GetBotSpellInfo(bot) : std::string();

    std::string quests;
    for (auto const& [questId, qsd] : bot->getQuestStatusMap())
    {
        // look up the template
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        // get the English title as a fallback
        std::string title = quest->GetTitle();

        // then, if we have a locale record, overwrite it
        if (auto const* locale = sObjectMgr->GetQuestLocale(questId))
        {
            int locIdx = bot->GetSession()->GetSessionDbLocaleIndex();
            if (locIdx >= 0)
                ObjectMgr::GetLocaleString(locale->Title, locIdx, title);
        }

        // Convert quest status to readable string
        std::string statusText;
        switch (qsd.Status)
        {
            case QUEST_STATUS_NONE:       statusText = "not started"; break;
            case QUEST_STATUS_COMPLETE:   statusText = "complete (ready to turn in)"; break;
            case QUEST_STATUS_INCOMPLETE: statusText = "in progress"; break;
            case QUEST_STATUS_FAILED:     statusText = "failed"; break;
            case QUEST_STATUS_REWARDED:   statusText = "completed and rewarded"; break;
            default:                      statusText = "unknown"; break;
        }

        quests += "Quest \"" + title + "\" is " + statusText + "\n";
    }

    std::string los;
    std::vector<std::string> losLocs = ChatHandler_GetVisibleLocations(bot);
    if (!losLocs.empty()) {
        for (const auto& entry : losLocs) los += " - " + entry + "\n";
    }

    std::string players;
    std::vector<std::string> nearbyPlayers = ChatHandler_GetVisiblePlayers(bot);
    if (!nearbyPlayers.empty()) {
        for (const auto& entry : nearbyPlayers) players += " - " + entry + "\n";
    }

    // Use template
    return SafeFormat(
        g_ChatBotSnapshotTemplate,
        fmt::arg("combat", combat),
        fmt::arg("group", group),
        fmt::arg("spells", spells),
        fmt::arg("quests", quests),
        fmt::arg("los", los),
        fmt::arg("players", players)
    );
}

// BEGIN BRIDGE CARD FACTS -- extracted verbatim by the offline card-facts check (J5e-crash).
// DS.12: the in-world facts a card generation gets about the bot, read on world. Outside the extracted adapter so
// the adapter checks supply their own; the envelope bounds and sanitizes every string. No database access.
void BridgeCardFacts(Player* bot, OllamaBridge::CardFacts& facts)
{
    facts.name = bot->GetName();
    facts.race = FormatPlayerRace(bot->getRace());
    facts.className = FormatPlayerClass(bot->getClass());
    facts.gender = bot->getGender() == GENDER_FEMALE ? "female" : "male";
    facts.level = bot->GetLevel();
    facts.faction = bot->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde";
    // J5e-crash: never Player::GetGuildName(), which dereferences the guild unchecked (a guildless bot's first card
    // offer crashed the world thread). A guildless bot keeps an empty guild.
    if (Guild const* guild = bot->GetGuild())
        facts.guild = guild->GetName();
    if (PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
        if (AreaTableEntry const* zone = botAI->GetCurrentZone())
            facts.zone = botAI->GetLocalizedAreaName(zone);
    // Only an existing assignment: GetBotPersonality may assign one with a synchronous query.
    auto const personality = g_BotPersonalityList.find(bot->GetGUID().GetRawValue());
    if (g_EnableRPPersonalities && personality != g_BotPersonalityList.end() && personality->second != "default")
        facts.traits = OllamaBridge::CardTraitWord(personality->second);
}
// END BRIDGE CARD FACTS

// DS.14/J5f: the bot's own state for one request, read by playerbots on world (the chat hook, map threads idle) and
// copied as plain values; the envelope filters and bounds it. Outside the extracted adapter, like the card facts.
void BridgeBotStateFacts(Player* bot, Player* partner, OllamaBridge::BotState& state)
{
    PlayerbotBridge::BotStateFacts facts;
    if (!PlayerbotBridge::ReadBotState(bot->GetGUID(), partner->GetGUID(), facts))
        return;
    state.present = true;
    state.healthPct = facts.healthPct;
    state.powerType = facts.powerType;
    state.powerCurrent = facts.powerCurrent;
    state.powerMax = facts.powerMax;
    state.level = facts.level;
    state.alive = facts.alive;
    state.ghost = facts.ghost;
    state.inCombat = facts.inCombat;
    state.victim = facts.victim;
    state.victimVisible = facts.victimVisible;
    state.victimName = std::move(facts.victimName);
    state.victimLevel = facts.victimLevel;
    state.victimHealthPct = facts.victimHealthPct;
    state.flying = facts.flying;
    state.casting = facts.casting;
    state.looting = facts.looting;
    state.resting = facts.resting;
    state.travelling = facts.travelling;
    state.working = facts.working;
    state.questTitle = std::move(facts.questTitle);
    state.zone = std::move(facts.zone);
    state.area = std::move(facts.area);
    state.mapId = facts.mapId;
    state.x = facts.x;
    state.y = facts.y;
    state.z = facts.z;
    state.partnerOnline = facts.partnerOnline;
    state.partnerInGroup = facts.partnerInGroup;
    state.partnerVisible = facts.partnerVisible;
    state.partnerSameMap = facts.partnerSameMap;
    state.partnerYards = facts.partnerYards;
    state.partnerZone = std::move(facts.partnerZone);
    state.partnerX = facts.partnerX;
    state.partnerY = facts.partnerY;
    state.partnerZ = facts.partnerZ;
    state.mounted = facts.mounted;
    state.swimming = facts.swimming;
    state.groupSize = facts.groupSize;
    state.leads = facts.leads;
    state.masterName = std::move(facts.masterName);
    state.follow = facts.follow;
    state.stay = facts.stay;
    state.guard = facts.guard;
    state.grind = facts.grind;
    state.passive = facts.passive;
    state.plans = OllamaChat_BridgePlanKinds(bot->GetGUID().GetRawValue());
    state.freeBagSlots = facts.freeBagSlots;
    state.money = facts.money;
}

static_assert(static_cast<uint8_t>(PlayerbotBridge::BotStateTopic::Spells) ==
                  static_cast<uint8_t>(OllamaBridge::StateTopic::Spells) &&
              OllamaBridge::StateTopicCount == 5);

// J5f-b: one topic of the bot's own state for a bot_state query, read by playerbots on world (WorldChannel::Update,
// map threads idle) and copied as raw entries; the channel filters, bounds and names them.
bool BridgeBotStateTopic(uint64_t botGuid, OllamaBridge::StateTopic topic,
                         std::vector<OllamaBridge::StateEntry>& entries)
{
    entries.clear();
    std::vector<PlayerbotBridge::BotStateItem> items;
    if (!PlayerbotBridge::ReadBotStateTopic(ObjectGuid(botGuid), static_cast<PlayerbotBridge::BotStateTopic>(topic),
                                            items))
        return false;
    for (auto& item : items)
    {
        OllamaBridge::StateEntry entry{std::move(item.name), {}, item.value, item.extra};
        if (topic == OllamaBridge::StateTopic::Inventory)
            entry.kind = item.code ? "equipped" : "bag";
        else if (topic == OllamaBridge::StateTopic::Quests)
            entry.kind = item.code == 1 ? "complete" : item.code == 2 ? "failed" : "active";
        else if (topic == OllamaBridge::StateTopic::Strategies)
            entry.kind = item.code ? "combat" : "non_combat";
        entries.push_back(std::move(entry));
    }
    return true;
}

// DS.14 (c), J6e-c: what a watch needs of its bot, read on world (admission in the channel's post-map-join update and
// the dispatcher's scan): ready only while the bot is a playerbot in the world outside battlegrounds and arenas and its
// partner is online; then combat and the activity code from the J5f reader. Two lookups and one read per watch.
bool BridgeWatchFacts(uint64_t botGuid, uint64_t partnerGuid, OllamaBridge::WatchFacts& facts)
{
    facts = {};
    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botGuid));
    Player* partner = ObjectAccessor::FindConnectedPlayer(ObjectGuid(partnerGuid));
    if (!bot || !partner || !bot->IsInWorld() || bot->InBattleground() || bot->InArena())
        return false;
    OllamaBridge::BotState state;
    BridgeBotStateFacts(bot, partner, state);
    if (!state.present)
        return false;
    facts.ready = true;
    facts.inCombat = state.inCombat;
    facts.activity = OllamaBridge::ActivityCode(state);
    return true;
}

// J6e-c: the governor scope of a wake's conversation, keyed as the chat hook keys a whisper, say, yell or party line
// from the partner (no channel, no guild): the partner's group for party, else the partner's zone.
std::string BridgeWakeScopeKey(Player* partner, ChatChannelSourceLocal source)
{
    Group* group = source == SRC_PARTY_LOCAL ? partner->GetGroup() : nullptr;
    return Governor_MakeScopeKey(ChatChannelSourceLocalStr[source], 0, std::string(), 0,
                                 group ? group->GetGUID().GetCounter() : partner->GetZoneId());
}

// BEGIN BRIDGE WORLD ADAPTER -- extracted verbatim by the offline game-adapter checks.
namespace
{
struct BridgeView
{
    OllamaBridge::DispatchGuard guard;
    OllamaBridge::Context context;
    std::string signature;
    // Length of the signature's voice part; the card fields (appended accepted body, card token) follow it (DS.8).
    std::size_t voiceSignature = 0;
    std::vector<OllamaBridge::ChannelObservedMark> marks;
    OllamaPersona::CardContext card;
};

struct BridgeBinding
{
    uint64_t generation = 0;
    BridgeView view;
    OllamaBridge::Deadline heartbeat;
    OllamaBridge::Deadline idleUntil;
    std::string memory;
    std::vector<OllamaBridge::ChannelObservedMark> marks;
};

// World-thread only. Entries contain values, never Player/Group pointers. Refresh work is at most 64 * 40 members.
std::vector<BridgeBinding> g_bridgeBindings;
uint64_t g_bridgeRevision = 0;
// The bridge generation the persona store was last activated for; OllamaChat_BridgeReset retires it.
uint64_t g_cardGeneration = 0;
// Watchlist (DS.8): bindings erased because their ready card changed under an unchanged voice.
uint64_t g_bridgeCardChurn = 0;

void BridgeField(std::string& signature, std::string_view value)
{
    signature += std::to_string(value.size());
    signature += ':';
    signature += value;
}

bool BridgeHuman(Player* player)
{
    return player && player->IsInWorld() && OllamaIsRealPlayer(player) &&
           ObjectAccessor::FindConnectedPlayer(player->GetGUID()) == player;
}

// DS.14: who may hold a conversation in which the bot acts on its own. Humans today; DS.11's later bot-to-bot talk
// widens this one predicate. Any human of the bot's team may make any bot act, by whisper at any distance: safe only
// because the user is the only player (DS.12); a server with several players needs a per-bot partner rule here.
bool BridgePartnerAllowed(Player* speaker) { return BridgeHuman(speaker); }

bool BridgeVisible(Player* bot, Player* player, float distance)
{
    // Always-visible core exceptions do not waive map/instance/phase, distance or stealth checks.
    return player && player->IsInWorld() && distance > 0.0f && bot->IsInMap(player) && bot->InSamePhase(player) &&
           bot->IsWithinDistInMap(player, distance) && bot->CanSeeOrDetect(player, false, true) &&
           player->CanSeeOrDetect(bot, false, true);
}

// Shared conservative participant-observable Unit check. No AI role or selected-target reads.
bool BridgeVisibleUnit(Player* bot, Unit* unit, float distance)
{
    return bot && bot->IsInWorld() && unit && unit->IsInWorld() && distance > 0.0f && bot->IsInMap(unit) &&
           bot->InSamePhase(unit) && bot->IsWithinDistInMap(unit, distance) && bot->CanSeeOrDetect(unit, false, true);
}

class BridgeQuerySource final : public OllamaBridge::QueryWorldSource
{
public:
    bool Observe(OllamaBridge::WorldTicket const& ticket, OllamaBridge::DispatchGuard& guard) override
    {
        return OllamaChat_BridgeObserve(ticket, guard);
    }

    bool GroupMembers(OllamaBridge::WorldTicket const& ticket, uint64_t groupGuid,
                      std::vector<uint64_t>& members) override
    {
        members.clear();
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(ticket.scope.botGuid));
        Group* group = bot && bot->IsInWorld() ? bot->GetGroup() : nullptr;
        if (!group || group->isRaidGroup() || group->GetGUID().GetRawValue() != groupGuid ||
            group->GetMemberSlots().empty() || group->GetMemberSlots().size() > OllamaBridge::ChannelMaxUnits ||
            !group->IsMember(bot->GetGUID()))
            return false;
        for (auto const& slot : group->GetMemberSlots())
            members.push_back(slot.guid.GetRawValue());
        return true;
    }

    bool ResolveVisible(OllamaBridge::WorldTicket const& ticket, uint64_t guid,
                        OllamaBridge::VisibleUnit& result) override
    {
        result = {};
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(ticket.scope.botGuid));
        Unit* unit = bot && bot->IsInWorld() ? ObjectAccessor::GetUnit(*bot, ObjectGuid(guid)) : nullptr;
        auto const it =
            std::find_if(g_bridgeBindings.begin(), g_bridgeBindings.end(), [&](BridgeBinding const& item)
                         { return item.generation == ticket.generation && item.view.guard.scope == ticket.scope; });
        if (it == g_bridgeBindings.end())
            return false;
        float const distance =
            it->view.guard.source == OllamaBridge::DispatchSource::Yell ? g_YellDistance : g_SayDistance;
        if (!BridgeVisibleUnit(bot, unit, distance))
            return false;
        Player* player = unit->ToPlayer();
        if (player && ObjectAccessor::FindConnectedPlayer(player->GetGUID()) != player)
            return false;
        Group* group = player ? player->GetGroup() : nullptr;
        result = {guid, group ? group->GetGUID().GetRawValue() : 0, unit->GetName(),
                  static_cast<int32_t>(unit->GetHealthPct())};
        return true;
    }

    bool ReadMark(OllamaBridge::WorldTicket const& ticket, uint8_t icon, uint64_t& targetGuid) override
    {
        targetGuid = 0;
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(ticket.scope.botGuid));
        Group* group = bot && bot->IsInWorld() ? bot->GetGroup() : nullptr;
        if (!group || group->isRaidGroup() || icon > 7 || !group->IsMember(bot->GetGUID()))
            return false;
        targetGuid = group->GetTargetIcon(icon).GetRawValue();
        return targetGuid != 0;
    }

    bool ReadTopic(OllamaBridge::WorldTicket const& ticket, OllamaBridge::StateTopic topic,
                   std::vector<OllamaBridge::StateEntry>& entries) override
    {
        return BridgeBotStateTopic(ticket.scope.botGuid, topic, entries);
    }

    bool ReadWatch(uint64_t botGuid, uint64_t partnerGuid, OllamaBridge::WatchFacts& facts) override
    {
        return BridgeWatchFacts(botGuid, partnerGuid, facts);
    }
};

// BEGIN BRIDGE PERSONA CARD -- extracted verbatim by the offline persona adapter check.
// Cache-only card read for one capture, no I/O. DS.12: the card is the bot's own data, read for every speaker and
// channel. A closed gate or no current observation keeps the view, its persona text and signature voice only. The
// caller signs the card token after the persona text.
void BridgeCard(BridgeView& view)
{
    auto const& store = OllamaPersona::WorldStore();
    if (!store.Enabled())
        return;
    view.card = OllamaPersona::ReadCard(store, OllamaDispatch_BridgeWorld().Generation(), view.guard.scope.botGuid,
                                        view.context.persona);
}

// The generation's card opportunity (Loading and Unready have no stamp and offer nothing). An accepted card in the
// persona text is reported as accepted. A bot without a card is offered a generation, at most MaxCardOffers times
// per bridge generation; the offer is only checked here and counted once the request is queued (BridgeSubmit). An
// accepted card left out as too long offers nothing: a provider never replaces a card.
OllamaBridge::CardOpportunity BridgeCardOpportunity(OllamaPersona::CardContext const& card, uint64_t botGuid)
{
    using OllamaPersona::CardStatus;
    OllamaBridge::CardOpportunity opportunity;
    bool const accepted = card.status == CardStatus::Accepted && card.included;
    bool const wanted = card.status == CardStatus::Missing || card.status == CardStatus::NoCard;
    if (!accepted &&
        (!wanted || !OllamaPersona::WorldStore().CanOffer(OllamaDispatch_BridgeWorld().Generation(), botGuid)))
        return opportunity;
    opportunity.offered = true;
    opportunity.revision = card.revision;
    opportunity.stamp = card.stamp;
    opportunity.accepted = accepted;
    return opportunity;
}
// END BRIDGE PERSONA CARD

// J5e: `why` names the first failed rule; every false return sets it.
bool BridgeCapture(uint64_t botGuid, uint64_t speakerGuid, OllamaBridge::DispatchSource source, BridgeView& view,
                   char const** why = nullptr)
{
    using namespace OllamaBridge;
    auto const drop = [why](char const* reason)
    {
        if (why)
            *why = reason;
        return false;
    };
    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botGuid));
    Player* speaker = ObjectAccessor::FindConnectedPlayer(ObjectGuid(speakerGuid));
    PlayerbotAI* ai = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
    if (!g_Enable || !bot || !bot->IsInWorld() || !ai || !ai->IsBotAI() || !BridgePartnerAllowed(speaker) ||
        bot == speaker || source == DispatchSource::Unsupported || bot->GetTeamId() != speaker->GetTeamId())
        return drop("disabled_or_offline");
    if ((source == DispatchSource::Whisper && !g_EnableWhisperReplies) ||
        (source == DispatchSource::Party && g_DisableForParty) ||
        ((source == DispatchSource::Say || source == DispatchSource::Yell) && g_DisableForSayYell))
        return drop("channel_off");

    Group* group = bot->GetGroup();
    if (group && (group->isRaidGroup() || group->GetMemberSlots().size() > 40))
        return drop("raid");
    float const distance = source == DispatchSource::Yell ? g_YellDistance : g_SayDistance;
    // DS.14: the speaker is the bot's conversation partner, whether or not they lead it. The bot acts as itself and
    // each action checks plausibility and safety, never who leads the bot; the bot's real group (0: none) goes with the
    // scope in every case. A whisper is not a local line, so it ignores the say radius (any map, J5e); an addressed
    // say or yell needs the speaker within hearing; party keeps the conservative local radius (no remote-party
    // situation knowledge). Observations still come from the bot's own surroundings.
    if (source != DispatchSource::Whisper && !BridgeVisible(bot, speaker, distance))
        return drop("out_of_range");
    if (source == DispatchSource::Party && (!group || speaker->GetGroup() != group ||
                                            !group->IsMember(bot->GetGUID()) || !group->IsMember(speaker->GetGUID())))
        return drop("not_in_group");
    // Descriptive only, never a gate: the speaker is the bot's master, or the leader of a masterless grouped bot.
    Player* master = ai->GetMaster();
    bool const speakerControlsBot =
        master ? master == speaker : group && group->GetLeaderGUID().GetRawValue() == speakerGuid;

    auto const selected = OllamaDispatch_BridgeWorld().Selected();
    if (selected.epoch.empty())
        return drop("no_epoch");
    view = {};
    auto& guard = view.guard;
    guard.scope.instance = selected.instance;
    guard.scope.provider = selected.provider;
    guard.scope.epoch = selected.epoch;
    guard.scope.botGuid = botGuid;
    guard.scope.controllerGuid = speakerGuid;  // DS.14: the partner; the wire keeps the name.
    guard.scope.channel = source == DispatchSource::Party ? "party" : "direct";
    guard.groupGuid = group ? group->GetGUID().GetRawValue() : 0;
    guard.scope.contextId = std::to_string(static_cast<unsigned>(source)) + "-" + std::to_string(speakerGuid);
    guard.source = source;
    guard.speakerGuid = speakerGuid;
    guard.mapId = bot->GetMapId();
    guard.instanceId = bot->GetInstanceId();
    guard.phaseMask = bot->GetPhaseMask();
    guard.humanSpeaker = guard.authority = guard.perceptible = guard.audienceOnline = true;
    guard.nonRaid = group != nullptr;
    guard.audience = {botGuid, speakerGuid};
    std::vector<uint64_t> members;
    if (group)
    {
        for (auto const& slot : group->GetMemberSlots())
            members.push_back(slot.guid.GetRawValue());
        std::sort(members.begin(), members.end());
        if (members.empty() || !members.front() || std::adjacent_find(members.begin(), members.end()) != members.end())
            return drop("bad_group");
        if (source == DispatchSource::Party)
            guard.audience = members;
    }
    std::sort(guard.audience.begin(), guard.audience.end());
    BridgeField(view.signature, std::to_string(guard.scope.controllerGuid));
    BridgeField(view.signature, std::to_string(guard.groupGuid));
    BridgeField(view.signature, group ? std::to_string(group->GetLeaderGUID().GetRawValue()) : "0");
    BridgeField(view.signature, std::to_string(distance));
    for (auto guid : members)
        BridgeField(view.signature, std::to_string(guid));
    BridgeField(view.signature, "audience");
    for (auto guid : guard.audience)
    {
        Player* participant = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
        // A whisper's audience is the bot and the speaker, wherever the speaker is; others must see each other.
        bool const present = source == DispatchSource::Whisper ? participant && participant->IsInWorld()
                                                               : BridgeVisible(bot, participant, distance);
        if (!present || participant->GetName().size() > 128 ||
            (source == DispatchSource::Party && participant->GetGroup() != group))
            return drop("audience");
        view.context.party.push_back({guid, participant->GetName(), "", -1, 0});
        BridgeField(view.signature, std::to_string(guid));
        BridgeField(view.signature, participant->GetName());
        BridgeField(view.signature, std::to_string(participant->GetPhaseMask()));
        BridgeField(view.signature, OllamaIsRealPlayer(participant) ? "human" : "bot");
    }
    BridgeField(view.signature, std::to_string(speaker->GetPhaseMask()));
    BridgeField(view.signature, std::to_string(guard.mapId));
    BridgeField(view.signature, std::to_string(guard.instanceId));
    BridgeField(view.signature, std::to_string(guard.phaseMask));
    view.context.speakerGuid = speakerGuid;
    view.context.channel = guard.scope.channel;
    view.context.groupScope = group ? std::to_string(guard.groupGuid) : "";
    view.context.speakerControlsBot = speakerControlsBot;
    view.context.systemPrompt =
        "You are a person living in Azeroth. Reply briefly to the speaker using only the "
        "provided observations and this conversation.";
    view.context.persona = Roleplay_BuildVoicePrompt(bot);
    if (view.context.persona.size() > 8192)
        return drop("voice_too_long");
    std::size_t const voiceSize = view.context.persona.size();
    BridgeCard(view);
    view.context.observations = {"Your name is " + bot->GetName() + ".", "The speaker is " + speaker->GetName() + "."};
    // Piggyback on this existing bounded observation; never add a world/group scan or tick hook.
    if (group)
        for (uint8_t icon = 0; icon < 8; ++icon)
        {
            auto const guid = group->GetTargetIcon(icon);
            Unit* unit = guid.GetRawValue() ? ObjectAccessor::GetUnit(*bot, guid) : nullptr;
            if (BridgeVisibleUnit(bot, unit, distance))
                view.marks.push_back({guid.GetRawValue(), icon});
        }
    std::string_view const persona = view.context.persona;
    BridgeField(view.signature, persona.substr(0, voiceSize));
    view.voiceSignature = view.signature.size();
    // The appended accepted body, then readiness, revision and snapshot: a reload or update fails BridgeSame even
    // when the text is unchanged.
    if (auto const card = OllamaPersona::CardToken(view.card); !card.empty())
    {
        BridgeField(view.signature, persona.substr(voiceSize));
        BridgeField(view.signature, card);
    }
    return true;
}

bool BridgeSame(BridgeBinding const& binding, BridgeView const& current)
{
    return binding.generation == OllamaDispatch_BridgeWorld().Generation() &&
           binding.view.guard.scope.epoch == current.guard.scope.epoch && binding.view.signature == current.signature;
}

bool BridgeCardReady(OllamaPersona::CardStatus status)
{
    return status == OllamaPersona::CardStatus::Missing || status == OllamaPersona::CardStatus::NoCard ||
           status == OllamaPersona::CardStatus::Accepted;
}

// The captures differ at most in their card fields.
bool BridgeSameVoice(BridgeBinding const& binding, BridgeView const& current)
{
    return binding.generation == OllamaDispatch_BridgeWorld().Generation() &&
           binding.view.guard.scope.epoch == current.guard.scope.epoch &&
           binding.view.voiceSignature == current.voiceSignature &&
           binding.view.signature.compare(0, current.voiceSignature, current.signature, 0, current.voiceSignature) == 0;
}

// DS.8, Refresh and Observe only: a binding captured without a ready card (Unready or Loading: voice only, nothing
// offered, stamp 0) survives a card change, so a card loading after the message keeps the in-flight reply. Submit
// stays strict (BridgeSame) and renews such a binding at the next message, unless J14d keeps it.
bool BridgeTolerates(BridgeBinding const& binding, BridgeView const& current)
{
    return BridgeSame(binding, current) ||
           (!BridgeCardReady(binding.view.card.status) && BridgeSameVoice(binding, current));
}

// Called before erasing a binding whose capture succeeded: counts a ready card that changed under the same voice
// (a committed proposal, a reload). Such a change erases the pair's memory and native context.
void BridgeCountChurn(BridgeBinding const& binding, BridgeView const& current)
{
    if (!BridgeCardReady(binding.view.card.status) || BridgeSame(binding, current) ||
        !BridgeSameVoice(binding, current))
        return;
    ++g_bridgeCardChurn;
    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] Bridge card churn for bot {}: {} -> {} ({} since start)",
                 binding.view.guard.scope.botGuid, OllamaPersona::CardToken(binding.view.card),
                 OllamaPersona::CardToken(current.card), g_bridgeCardChurn);
}

void BridgeErase(std::vector<BridgeBinding>::iterator it)
{
    auto const& scope = it->view.guard.scope;
    uint64_t const botGuid = scope.botGuid;
    OllamaDispatch_BridgeWorld().InvalidateScope(scope.botGuid, scope.contextId);
    g_bridgeBindings.erase(it);
    // The bot's last binding died because it left the world: drop its card snapshot so the next meeting reloads
    // from SQL. A changed capture alone keeps the cache; forgetting then would undo every load it signs.
    if (!ObjectAccessor::FindConnectedPlayer(ObjectGuid(botGuid)) &&
        std::none_of(g_bridgeBindings.begin(), g_bridgeBindings.end(),
                     [&](BridgeBinding const& item) { return item.view.guard.scope.botGuid == botGuid; }))
        OllamaPersona::ForgetPersona(botGuid);
}

// Fresh capture of the ticket's binding. A failed or changed capture erases the binding and returns end.
std::vector<BridgeBinding>::iterator BridgeObserveFresh(OllamaBridge::WorldTicket const& ticket, BridgeView& current)
{
    auto it = std::find_if(g_bridgeBindings.begin(), g_bridgeBindings.end(), [&](BridgeBinding const& item)
                           { return item.generation == ticket.generation && item.view.guard.scope == ticket.scope; });
    if (it == g_bridgeBindings.end())
        return it;
    auto const now = OllamaBridge::Now();
    auto const& captured = it->view.guard;
    bool const fresh = !OllamaBridge::Expired(it->heartbeat, now) && !OllamaBridge::Expired(it->idleUntil, now) &&
                       BridgeCapture(captured.scope.botGuid, captured.speakerGuid, captured.source, current);
    if (!fresh || !BridgeTolerates(*it, current))
    {
        if (fresh)
            BridgeCountChurn(*it, current);
        BridgeErase(it);
        return g_bridgeBindings.end();
    }
    return it;
}

bool BridgeSubmit(Player* bot, Player* speaker, ChatChannelSourceLocal source, std::string const& text, bool addressed,
                  std::string const& scopeKey, uint8_t chainDepth, OllamaBridge::Watch const* wake = nullptr)
{
    using namespace OllamaBridge;
    DispatchSource origin = DispatchSource::Unsupported;
    switch (source)
    {
        case SRC_WHISPER_LOCAL:
            origin = DispatchSource::Whisper;
            break;
        case SRC_PARTY_LOCAL:
            origin = DispatchSource::Party;
            break;
        case SRC_SAY_LOCAL:
            origin = DispatchSource::Say;
            break;
        case SRC_YELL_LOCAL:
            origin = DispatchSource::Yell;
            break;
        default:
            return false;
    }
    if (chainDepth || text.empty() || text.size() > MaxTextBytes ||
        ((origin == DispatchSource::Say || origin == DispatchSource::Yell) && !addressed && !wake))
        return false;
    auto& cards = OllamaPersona::WorldStore();
    uint64_t const generation = OllamaDispatch_BridgeWorld().Generation();
    // Lazy and idempotent for the current bridge generation; the store refuses an older one.
    if (cards.Enabled() && cards.Activate(generation))
        g_cardGeneration = generation;
    OllamaChat_BridgeRefresh();
    BridgeView view;
    char const* why = nullptr;
    if (!BridgeCapture(bot->GetGUID().GetRawValue(), speaker->GetGUID().GetRawValue(), origin, view, &why))
    {
        // J5e: a reply dropped after selection logs why, once per bot, channel and reason a minute (log only).
        using DropClock = std::chrono::steady_clock;
        static std::unordered_map<std::string, DropClock::time_point> nextDropLog;
        auto const now = DropClock::now();
        std::string_view const reason = why ? why : "unknown";
        std::string_view const channel = origin == DispatchSource::Whisper ? "whisper"
                                         : origin == DispatchSource::Party ? "party"
                                         : origin == DispatchSource::Say   ? "say"
                                                                           : "yell";
        // The channel is part of the key, so it is used even where logging compiles to nothing.
        std::string key = std::to_string(bot->GetGUID().GetRawValue());
        key.append(":").append(channel).append(":").append(reason);
        auto const it = nextDropLog.find(key);
        if (it != nextDropLog.end() && now < it->second)
            return false;
        if (nextDropLog.size() >= 256)
            nextDropLog.clear();
        nextDropLog[key] = now + std::chrono::seconds(60);
        LOG_INFO("module.ollamachat", "Bridge reply dropped: bot {} channel {} reason {}", bot->GetName(), channel,
                 reason);
        return false;
    }
    // J6e-c: a wake continues the conversation its watch was armed in, or nothing.
    if (wake && view.guard.scope.contextId != wake->contextId)
        return false;
    // The one load request: a qualified current encounter (DS.12: any speaker and channel; the card is the bot's own
    // data). Captures elsewhere only read. A closed gate does no I/O.
    cards.Prepare(generation, view.guard.scope.botGuid, std::chrono::steady_clock::now());
    auto const bridgeSettings = OllamaDispatch_BridgeWorld().Snapshot();
    if (bridgeSettings.provider == "codex" && bridgeSettings.observerEnabled)
    {
        // Native permits one exact context per bot/controller/channel peer. Replace, never alias to newest.
        for (auto prior = g_bridgeBindings.begin(); prior != g_bridgeBindings.end();)
        {
            auto const& scope = prior->view.guard.scope;
            if (scope.botGuid == view.guard.scope.botGuid && scope.controllerGuid == view.guard.scope.controllerGuid &&
                scope.channel == view.guard.scope.channel && scope.contextId != view.guard.scope.contextId)
            {
                auto const index = prior - g_bridgeBindings.begin();
                BridgeErase(prior);
                prior = g_bridgeBindings.begin() + index;
            }
            else
                ++prior;
        }
    }
    auto it = std::find_if(g_bridgeBindings.begin(), g_bridgeBindings.end(),
                           [&](BridgeBinding const& item)
                           {
                               return item.view.guard.scope.botGuid == view.guard.scope.botGuid &&
                                      item.view.guard.scope.contextId == view.guard.scope.contextId;
                           });
    // DS.14: speakerControlsBot is descriptive and stays out of the signature (a follow may make the partner the bot's
    // master without ending the conversation), so a kept binding takes the fresh capture's value.
    bool const speakerControlsBot = view.context.speakerControlsBot;
    auto const now = Now();
    std::string memory;
    // J14d: a tolerated binding is kept instead of renewed while the new capture's card is not ready either (a renewal
    // would bring no card, only a new revision and a native context reset) or while one of its replies is in flight
    // (a renewal invalidates the scope's pending tickets). The kept binding answers on its own voice-only view, as the
    // in-flight reply does; the first message after those replies renews it with the card.
    bool const renew =
        it != g_bridgeBindings.end() && !BridgeSame(*it, view) &&
        !(BridgeTolerates(*it, view) &&
          (!BridgeCardReady(view.card.status) || OllamaDispatch_BridgeWorld().PendingFor(it->view.guard.scope, now)));
    if (renew)
    {
        // DS.8: a binding the refresh tolerated is renewed with its current card and a new persona revision; its
        // one-exchange memory (same speaker, scope and voice) moves to the renewal.
        if (BridgeTolerates(*it, view))
            memory = std::move(it->memory);
        BridgeCountChurn(*it, view);
        BridgeErase(it);
        it = g_bridgeBindings.end();
    }
    if (it == g_bridgeBindings.end())
    {
        if (g_bridgeBindings.size() >= MaxWorldScopes || g_bridgeRevision == UINT64_MAX)
            return false;
        auto const revision = std::to_string(++g_bridgeRevision);
        auto& scope = view.guard.scope;
        scope.contextRevision = scope.membershipRevision = scope.personaRevision = scope.perceptionScope = revision;
        if (!OllamaDispatch_BridgeWorld().ObserveScope(scope, now))
            return false;
        g_bridgeBindings.push_back({OllamaDispatch_BridgeWorld().Generation(), std::move(view),
                                    CaptureDeadline(now, WorldLivenessMs),
                                    CaptureDeadline(now, MaxLifetimeMs),
                                    std::move(memory),
                                    {}});
        it = std::prev(g_bridgeBindings.end());
    }
    it->idleUntil = CaptureDeadline(now, MaxLifetimeMs);
    auto input = it->view.context;
    input.speakerControlsBot = speakerControlsBot;
    // BridgeSame proved the binding's card current, or a deferred renewal kept a not-ready card that offers nothing.
    // Without an opportunity the request stays as before.
    input.card = BridgeCardOpportunity(it->view.card, it->view.guard.scope.botGuid);
    if (input.card.offered && !input.card.accepted)
    {
        BridgeCardFacts(bot, input.card.facts);
        LOG_INFO("module.ollamachat", "[Ollama Chat] Bridge card offer for bot {} (revision {})",
                 it->view.guard.scope.botGuid, input.card.revision);
    }
    bool const offerCard = input.card.offered && !input.card.accepted;
    input.text = text;
    input.directAddress = addressed;
    // J6e-c: a chain starts at the player's line; a wake carries its watch's origin, depth and condition.
    input.originUnixMs = wake ? wake->originUnixMs : now.unixMs;
    if (wake)
    {
        input.wakeCondition = WatchConditionName(wake->condition);
        input.wakeWatchId = wake->id;
        input.wakeDepth = wake->depth;
    }
    input.memory = it->memory;
    // DS.14/J5f: the bot's own state, read fresh for every request and kept out of the capture, so a change (health,
    // position) never renews a binding or a cached prompt prefix.
    BridgeBotStateFacts(bot, speaker, input.state);
    OllamaChatRequest request;
    request.botGuid = bot->GetGUID().GetRawValue();
    request.targetGuid = speaker->GetGUID().GetRawValue();
    request.botName = bot->GetName();
    request.source = source;
    request.scopeKey = scopeKey;
    request.directAddress = addressed;
    auto const botGuid = it->view.guard.scope.botGuid;
    bool const queued =
        OllamaDispatch_SubmitBridge(std::move(request), it->view.guard.scope, std::move(input), it->view.guard, 30000);
    // A refused submit (queue, circuit, bound) sent no offer, so it does not use up the bot's offers.
    if (queued && offerCard)
        OllamaPersona::WorldStore().TakeOffer(OllamaDispatch_BridgeWorld().Generation(), botGuid);
    return queued;
}
}  // namespace

OllamaBridge::QueryWorldSource& OllamaChat_BridgeQuerySource()
{
    static BridgeQuerySource source;
    return source;
}

void OllamaChat_BridgeReset()
{
    g_bridgeBindings.clear();
    OllamaPersona::RetirePersonas(g_cardGeneration);
}

void OllamaChat_BridgeRefresh()
{
    using namespace OllamaBridge;
    auto const now = Now();
    OllamaDispatch_BridgeWorld().Expire(now);
    for (auto it = g_bridgeBindings.begin(); it != g_bridgeBindings.end();)
    {
        BridgeView current;
        auto const& guard = it->view.guard;
        bool const fresh = !Expired(it->heartbeat, now) && !Expired(it->idleUntil, now) &&
                           BridgeCapture(guard.scope.botGuid, guard.speakerGuid, guard.source, current);
        if (!fresh || !BridgeTolerates(*it, current) || !OllamaDispatch_BridgeWorld().ObserveScope(guard.scope, now))
        {
            if (fresh)
                BridgeCountChurn(*it, current);
            auto const index = it - g_bridgeBindings.begin();
            BridgeErase(it);
            it = g_bridgeBindings.begin() + index;
        }
        else
        {
            it->heartbeat = CaptureDeadline(now, WorldLivenessMs);
            bool const changed = current.marks.size() != it->marks.size() ||
                                 !std::equal(current.marks.begin(), current.marks.end(), it->marks.begin(),
                                             [](ChannelObservedMark const& a, ChannelObservedMark const& b)
                                             { return a.targetGuid == b.targetGuid && a.icon == b.icon; });
            auto const observedGuard = guard;
            std::vector<uint8_t> icons;
            if (changed)
                for (auto const& mark : current.marks)
                    icons.push_back(mark.icon);
            it->marks = std::move(current.marks);
            ++it;
            // The synchronous observer may invalidate a binding. Do not retain its iterator across publication.
            if (!icons.empty())
            {
                auto const index = it - g_bridgeBindings.begin();
                OllamaDispatch_ChannelEvents(observedGuard, icons);
                it = g_bridgeBindings.begin() + std::min<std::size_t>(index, g_bridgeBindings.size());
            }
        }
    }
}

void OllamaChat_BridgeNoteOwnMark(uint64_t botGuid)
{
    // DS.10: only the marking bot's bindings take the new marks as their baseline, so the next refresh opens no
    // events route for its own mark. A failed capture leaves the binding for the refresh to retire.
    for (auto& binding : g_bridgeBindings)
    {
        auto const& guard = binding.view.guard;
        BridgeView current;
        if (guard.scope.botGuid == botGuid &&
            BridgeCapture(guard.scope.botGuid, guard.speakerGuid, guard.source, current))
            binding.marks = std::move(current.marks);
    }
}

bool OllamaChat_BridgeObserve(OllamaBridge::WorldTicket const& ticket, OllamaBridge::DispatchGuard& guard)
{
    BridgeView current;
    auto const it = BridgeObserveFresh(ticket, current);
    if (it == g_bridgeBindings.end())
        return false;
    // Equality was established from fresh GUID resolution; revisions cannot be resurrected by matching values later.
    guard = std::move(current.guard);
    guard.scope = it->view.guard.scope;
    return true;
}

OllamaPersona::ProposalStatus OllamaChat_BridgeProposeCard(OllamaBridge::WorldTicket const& ticket,
                                                           uint64_t expectedRevision, uint64_t stamp, std::string body)
{
    BridgeView current;
    if (!OllamaDispatch_BridgeWorld().Current(ticket, OllamaBridge::Now()))
        return OllamaPersona::ProposalStatus::Stale;
    auto const it = BridgeObserveFresh(ticket, current);
    if (it == g_bridgeBindings.end())
        return OllamaPersona::ProposalStatus::Stale;
    // The fresh capture re-checked the gate, the binding and the signed card token.
    return OllamaPersona::ProposeCard(OllamaPersona::WorldStore(), it->generation, ticket.scope.botGuid,
                                      ticket.scope.provider, expectedRevision, stamp, std::move(body));
}

void OllamaChat_BridgeRemember(OllamaBridge::WorldTicket const& ticket, std::string const& reply)
{
    if (!ticket.request.input.wakeCondition.empty())
        return;  // J6e-c: a wake is no exchange with the speaker; the earlier exchange stays remembered.
    OllamaBridge::DispatchGuard guard;
    if (!OllamaDispatch_BridgeWorld().Current(ticket, OllamaBridge::Now()) ||
        !OllamaChat_BridgeObserve(ticket, guard) ||
        (guard.source != OllamaBridge::DispatchSource::Whisper && guard.source != OllamaBridge::DispatchSource::Party))
        return;
    for (auto& binding : g_bridgeBindings)
        if (binding.generation == ticket.generation && binding.view.guard.scope == ticket.scope)
        {
            // Only successfully delivered Bridge exchanges, never legacy/condensed/private-history imports.
            // At most one exchange; if two maximum-size messages exceed 8 KiB, retain the human statement alone.
            binding.memory = "Earlier the same speaker said: " + ticket.request.input.text;
            if (reply.empty())  // DS.14: an action-only answer, remembered as one and never as quoted speech.
                binding.memory += "\nYou answered without words.";
            else if (binding.memory.size() + reply.size() + 16 <= 8192)
                binding.memory += "\nYou replied: " + reply;
            return;
        }
}
bool OllamaChat_BridgeWake(OllamaBridge::Watch const& watch)
{
    using namespace OllamaBridge;
    ChatChannelSourceLocal source = SRC_WHISPER_LOCAL;
    switch (watch.source)
    {
        case DispatchSource::Whisper:
            break;
        case DispatchSource::Party:
            source = SRC_PARTY_LOCAL;
            break;
        case DispatchSource::Say:
            source = SRC_SAY_LOCAL;
            break;
        case DispatchSource::Yell:
            source = SRC_YELL_LOCAL;
            break;
        default:
            return false;
    }
    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(watch.botGuid));
    Player* partner = ObjectAccessor::FindConnectedPlayer(ObjectGuid(watch.partnerGuid));
    if (!bot || !partner || !bot->IsInWorld() || !partner->IsInWorld())
        return false;
    // One generation like the partner's line: fixed in-world text, not addressed (a failure stays silent), 30 s.
    return BridgeSubmit(bot, partner, source, WakeText(watch.condition), false, BridgeWakeScopeKey(partner, source), 0,
                        &watch);
}
// END BRIDGE WORLD ADAPTER

void PlayerBotChatHandler::ProcessChat(Player* player, uint32_t /*type*/, uint32_t lang, std::string& msg, ChatChannelSourceLocal sourceLocal, Channel* channel, Player* receiver, uint8_t chainDepth)
{
    if (player == nullptr) {
        LOG_ERROR("module.ollamachat", "[Ollama Chat] ProcessChat: player is null");
        return;
    }
    if (msg.empty()) {
        return;
    }
    if (lang == LANG_ADDON) return;
    bool const bridgeProvider = OllamaConfig_Snapshot().provider == OllamaProvider::Bridge;
    // Only a human's line opens a bridge request. A bot's own replies (BridgeHuman false) and bot chains never do,
    // so generated speech cannot feed back into the bridge or start a bot-to-bot loop.
    if (bridgeProvider && (!g_Enable || !BridgeHuman(player) || chainDepth || channel ||
                           (sourceLocal != SRC_WHISPER_LOCAL && sourceLocal != SRC_PARTY_LOCAL &&
                            sourceLocal != SRC_SAY_LOCAL && sourceLocal != SRC_YELL_LOCAL)))
        return;
    // A whisper or party line with the command indicator is for playerbots' own chat hooks (the same rule,
    // ChatCommandLine.h): no request and no budget. Say and yell have no playerbots hook, so such a line stays chat.
    std::string commandLine;
    if (bridgeProvider && (sourceLocal == SRC_WHISPER_LOCAL || sourceLocal == SRC_PARTY_LOCAL) &&
        PlayerbotBridge::ExtractChatCommand(msg, commandLine))
    {
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Command line (indicator): no model reply.");
        return;
    }
    std::string chanName = (channel != nullptr) ? channel->GetName() : "Unknown";
    uint32_t channelId = (channel != nullptr) ? channel->GetChannelId() : 0;
    std::string receiverName = (receiver != nullptr) ? receiver->GetName() : "None";
    if(g_DebugEnabled)
    {
        LOG_INFO("server.loading",
                "[Ollama Chat] Player {} sent msg: '{}' | Source: {} | Channel Name: {} | Channel ID: {} | Receiver: {}",
                player->GetName(), msg, (int)sourceLocal, chanName, channelId, receiverName);
    }


    auto startsWithWord = [](const std::string& text, const std::string& word) {
        if (text.size() < word.size()) return false;
        if (text.compare(0, word.size(), word) != 0) return false;
        // If exact length match or next char is whitespace/punctuation, it's a word
        return text.size() == word.size() || !std::isalnum((unsigned char)text[word.size()]);
    };

    // With the bridge provider only the command indicator above decides; no word list silences a line.
    std::string trimmedMsg = rtrim(msg);
    for (const std::string& blacklist : g_BlacklistCommands)
    {
        if (!bridgeProvider && startsWithWord(trimmedMsg, blacklist))
        {
            if (g_DebugEnabled)
                LOG_INFO("server.loading",
                         "[Ollama Chat] Message starts with '{}' (blacklisted). Skipping bot responses.",
                         blacklist);
            return;
        }
    }
    
    // Check if this channel type is disabled
    if (sourceLocal == SRC_GENERAL_LOCAL && g_DisableForCustomChannels)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Custom channels are disabled, skipping");
        }
        return;
    }
    
    if ((sourceLocal == SRC_SAY_LOCAL || sourceLocal == SRC_YELL_LOCAL) && g_DisableForSayYell)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Say/Yell channels are disabled, skipping");
        }
        return;
    }
    
    if ((sourceLocal == SRC_GUILD_LOCAL || sourceLocal == SRC_OFFICER_LOCAL) && g_DisableForGuild)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Guild channels are disabled, skipping");
        }
        return;
    }
    
    if ((sourceLocal == SRC_PARTY_LOCAL || sourceLocal == SRC_RAID_LOCAL) && g_DisableForParty)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Party/Raid channels are disabled, skipping");
        }
        return;
    }
             
    PlayerbotAI* senderAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);
    bool senderIsBot = (senderAI && senderAI->IsBotAI());

    // --- conversation governor -------------------------------------------
    // One key per conversation space so cooldowns, rate limits and repetition
    // history are tracked per channel rather than globally.
    //
    // Party and raid chat keys on the group, not the zone. Keying it on the
    // zone made two unrelated parties standing in Elwynn share one cooldown,
    // one rate limit and one repetition history -- and made a party reset all
    // three every time it walked over a zone border.
    uint32_t scopeGroupOrZone = player->GetZoneId();
    if (sourceLocal == SRC_PARTY_LOCAL || sourceLocal == SRC_RAID_LOCAL)
    {
        if (Group* senderGroup = player->GetGroup())
            scopeGroupOrZone = senderGroup->GetGUID().GetCounter();
    }

    const std::string scopeKey = Governor_MakeScopeKey(
        ChatChannelSourceLocalStr[sourceLocal],
        channel ? channel->GetChannelId() : 0,
        channel ? channel->GetName() : std::string(),
        (sourceLocal == SRC_GUILD_LOCAL || sourceLocal == SRC_OFFICER_LOCAL)
            ? player->GetGuildId() : 0,
        scopeGroupOrZone);

    if (!senderIsBot)
    {
        // A real player spoke here. This timestamp is what lets bots keep
        // talking to each other for a while afterwards.
        Governor_NoteHumanMessage(scopeKey);
    }
    else
    {
        // Bot-to-bot. Two brakes: an absolute depth ceiling, and the audience
        // rule -- bots do not hold conversations with nobody listening.
        if (!Governor_ChainDepthAllowed(chainDepth))
        {
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Chain depth {} reached the limit; not continuing.",
                         chainDepth);
            return;
        }

        if (!Governor_HasRecentHuman(scopeKey))
        {
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] No real player has spoken in {} recently; "
                         "bots will not talk among themselves here.", scopeKey);
            return;
        }
    }
    
    // One pass over the online players, reused by every eligibility test
    // below. These questions used to be answered by a fresh full walk per
    // candidate bot, inside a loop that was itself over every online player.
    OllamaWorldSnapshot world;
    world.Build();

    std::vector<Player*> eligibleBots;
    
    // Handle different chat sources differently
    if (sourceLocal == SRC_WHISPER_LOCAL && receiver != nullptr)
    {
        // Check if whisper replies are disabled
        if (!g_EnableWhisperReplies)
        {
            if(g_DebugEnabled)
            {
                LOG_INFO("module.ollamachat", "[Ollama Chat] Whisper replies are disabled, skipping");
            }
            return;
        }
        
        if(g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Processing whisper from {} to {}", 
                    player->GetName(), receiver->GetName());
        }
        
        // Skip bot-to-bot whispers to prevent Ollama responses
        if (senderIsBot)
        {
            return;
        }
        
        // For whispers, only the receiver bot can respond (if it's a bot)
        PlayerbotAI* receiverAI = PlayerbotsMgr::instance().GetPlayerbotAI(receiver);
        if (receiverAI && receiverAI->IsBotAI())
        {
            eligibleBots.push_back(receiver);
            if(g_DebugEnabled)
            {
                LOG_INFO("module.ollamachat", "[Ollama Chat] Found eligible bot {} for whisper", receiver->GetName());
            }
        }
        else if(g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Whisper target {} is not a bot or has no AI", receiver->GetName());
        }
    }
    else if (channel != nullptr)
    {
        // For channel chat, find all bots that are in the same channel instance
        if(g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Processing channel message in '{}' (ID: {})", 
                    channel->GetName(), channel->GetChannelId());
        }
        
        // Verify the original channel is valid before proceeding
        if (!channel)
        {
            if(g_DebugEnabled)
            {
                LOG_ERROR("module.ollamachat", "[Ollama Chat] Channel is null, cannot process channel message");
            }
            return;
        }
        
        auto const& allPlayers = ObjectAccessor::GetPlayers();

        // Hoisted out of the per-candidate loop below. This used to be a full
        // GetPlayers() walk nested inside a GetPlayers() walk -- quadratic in
        // online characters, on every single channel message.
        bool hasRealPlayerInChannel = false;
        for (auto const& playerItr : allPlayers)
        {
            Player* candidateReal = playerItr.second;
            if (!candidateReal || !OllamaIsOnChannel(channel, candidateReal->GetGUID()))
                continue;

            if (OllamaIsRealPlayer(candidateReal))
            {
                hasRealPlayerInChannel = true;
                break;
            }
        }

        if (!hasRealPlayerInChannel)
        {
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] No real players in channel '{}'; skipping.",
                         channel->GetName());
            return;
        }

        for (auto const& itr : allPlayers)
        {
            Player* candidate = itr.second;
            if (!candidate || candidate == player)
                continue;
                
            // Skip non-bots early
            PlayerbotAI* candidateAI = PlayerbotsMgr::instance().GetPlayerbotAI(candidate);
            if (!candidateAI || !candidateAI->IsBotAI())
                continue;
            
            // Classify by channel id, not by localized name substrings. The
            // old test looked for "General -" / "Trade -" / "LocalDefense -"
            // in the channel name, which only works on an English realm, and
            // GuildRecruitment was not handled at all -- which is why replies
            // went missing in the city channels.
            const uint32 chanId = channel->GetChannelId();

            // Zone-scoped: one instance per zone/city.
            const bool isZoneChannel = (chanId == uint32(ChatChannelId::GENERAL) ||
                                        chanId == uint32(ChatChannelId::TRADE) ||
                                        chanId == uint32(ChatChannelId::LOCAL_DEFENSE) ||
                                        chanId == uint32(ChatChannelId::GUILD_RECRUITMENT));

            // Realm-wide.
            const bool isGlobalChannel = (chanId == uint32(ChatChannelId::WORLD_DEFENSE) ||
                                          chanId == uint32(ChatChannelId::LOOKING_FOR_GROUP));

            if (isZoneChannel && candidate->GetZoneId() != player->GetZoneId())
                continue;   // wrong zone for a zone-scoped channel

            // A custom (unnumbered) channel has id 0 and no zone semantics;
            // membership alone decides, which is checked below.
            
            // CHANNEL MEMBERSHIP CHECK: Bot must actually be in the channel
            if (!OllamaIsOnChannel(channel, candidate->GetGUID()))
            {
                if(g_DebugEnabled)
                {
                    //LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} not in channel '{}', skipping", candidate->GetName(), channel->GetName());
                }
                continue;
            }
            
            // FACTION CHECK: For non-global channels, ensure same faction
            if (candidate->GetTeamId() != player->GetTeamId())
            {
                if (!isGlobalChannel)
                {
                    if(g_DebugEnabled)
                    {
                        //LOG_ERROR("module.ollamachat", "[Ollama Chat] Bot {} FAILED faction check - Bot: {}, Player: {}, Channel: '{}'", candidate->GetName(), (int)candidate->GetTeamId(), (int)player->GetTeamId(), channel->GetName());
                    }
                    continue; // SKIP this bot - wrong faction
                }
            }
            
            if (!hasRealPlayerInChannel)
            {
                if(g_DebugEnabled)
                {
                    //LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} skipped - no real players in channel '{}'", candidate->GetName(), channel->GetName());
                }
                continue;
            }
            
            // ONLY add bots that passed ALL verifications
            eligibleBots.push_back(candidate);
            if(g_DebugEnabled)
            {
                // LOG_INFO("module.ollamachat", "[Ollama Chat] VERIFIED eligible bot {} in channel '{}' - Distance: {:.2f}, Zone match: {}", candidate->GetName(), channel->GetName(), candidate->GetDistance(player), (candidate->GetZoneId() == player->GetZoneId()));
            }
        }
        
        if(g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Found {} bots in channel instance '{}'", 
                    eligibleBots.size(), channel->GetName());
        }
    }
    else
    {
        // For other chat types (say, yell, guild, party, etc.), use all players and filter by eligibility
        auto const& allPlayers = ObjectAccessor::GetPlayers();
        for (auto const& itr : allPlayers)
        {
            Player* candidate = itr.second;
            if (candidate->IsInWorld() && candidate != player)
            {
                PlayerbotAI* candidateAI = PlayerbotsMgr::instance().GetPlayerbotAI(candidate);
                if (candidateAI && candidateAI->IsBotAI())
                {
                    // For Guild/Party, verify there's a real player in that guild/party
                    if (sourceLocal == SRC_GUILD_LOCAL || sourceLocal == SRC_OFFICER_LOCAL)
                    {
                        if (candidate->GetGuildId() != 0 &&
                            !world.GuildHasRealPlayer(candidate->GetGuildId()))
                        {
                            continue;   // no real players in that guild
                        }
                    }
                    else if (sourceLocal == SRC_PARTY_LOCAL || sourceLocal == SRC_RAID_LOCAL)
                    {
                        Group* group = candidate->GetGroup();
                        if (group)
                        {
                            // Check if any real player is in this group
                            bool hasRealPlayerInGroup = false;
                            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                            {
                                if (OllamaIsRealPlayer(ref->GetSource()))
                                {
                                    hasRealPlayerInGroup = true;
                                    break;
                                }
                            }
                            if (!hasRealPlayerInGroup)
                                continue; // Skip bot - no real players in group
                        }
                    }
                    else if (sourceLocal == SRC_SAY_LOCAL || sourceLocal == SRC_YELL_LOCAL)
                    {
                        // Require a real player within hearing distance.
                        const float threshold =
                            (sourceLocal == SRC_SAY_LOCAL) ? g_SayDistance : g_YellDistance;

                        if (!world.RealPlayerWithin(candidate, threshold))
                            continue;   // nobody can hear it
                    }
                    
                    eligibleBots.push_back(candidate);
                }
            }
        }
    }
    
    std::vector<Player*> candidateBots;
    int notEligibleCount = 0;
    for (Player* bot : eligibleBots)
    {
        if (!bot)
        {
            continue;
        }
        
        // For channel messages, bots in eligibleBots have already passed STRICT channel checks
        // Only run additional eligibility checks for non-channel sources
        // EXCEPTION: If channel is nullptr but sourceLocal is a channel type (like GENERAL), 
        // treat it as a channel message (happens with bot-initiated messages)
        bool isChannelSource = (sourceLocal == SRC_GENERAL_LOCAL);
        
        if (channel != nullptr || isChannelSource)
        {
            // Channel bots have already been verified to be in EXACT same channel instance
            // OR this is a channel-type source (General) even without channel object
            candidateBots.push_back(bot);
        }
        else
        {
            // For non-channel sources (Say/Yell/Guild/Party/Whisper), run the full eligibility check
            if (IsBotEligibleForChatChannelLocal(bot, player, sourceLocal, channel, receiver))
                candidateBots.push_back(bot);
            else
                notEligibleCount++;
        }
    }
    
    if (g_DebugEnabled && notEligibleCount > 0)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] {} bots not eligible for {} (distance/guild/party checks failed)", 
                notEligibleCount, ChatChannelSourceLocalStr[sourceLocal]);
    }
    
    // Determine reply chance based on channel type
    uint32_t chance;
    if (sourceLocal == SRC_SAY_LOCAL || sourceLocal == SRC_YELL_LOCAL)
    {
        // Say/Yell channel type
        chance = senderIsBot ? g_BotReplyChance_Say : g_PlayerReplyChance_Say;
    }
    else if (sourceLocal == SRC_PARTY_LOCAL || sourceLocal == SRC_RAID_LOCAL)
    {
        // Party/Raid channel type
        chance = senderIsBot ? g_BotReplyChance_Party : g_PlayerReplyChance_Party;
    }
    else if (sourceLocal == SRC_GUILD_LOCAL || sourceLocal == SRC_OFFICER_LOCAL)
    {
        // Guild/Officer channel type
        chance = senderIsBot ? g_BotReplyChance_Guild : g_PlayerReplyChance_Guild;
    }
    else if (sourceLocal == SRC_GENERAL_LOCAL)
    {
        // General/Trade/Custom channel type
        chance = senderIsBot ? g_BotReplyChance_Channel : g_PlayerReplyChance_Channel;
    }
    else if (sourceLocal == SRC_WHISPER_LOCAL)
    {
        // A whisper is direct address -- as explicit as being named, and the
        // bot is the only recipient. It is not subject to the ambient reply
        // chance; EnableWhisperReplies is the whole gate.
        //
        // This used to fall through to the Say chance below, so lowering
        // PlayerReplyChance.Say randomly swallowed whispers, and setting it to
        // 0 meant a whispered bot never answered at all even with
        // EnableWhisperReplies = 1.
        chance = 100;
    }
    else
    {
        // Default fallback - use Say chances
        chance = senderIsBot ? g_BotReplyChance_Say : g_PlayerReplyChance_Say;
    }
    
    // Each bot->bot hop makes the next reply less likely, so a chain runs out
    // of energy on its own well before it hits the hard depth ceiling. Never
    // applied to a whisper: bot-to-bot whispers are refused long before this,
    // so a whisper here is always a person talking to a bot directly.
    if (senderIsBot && sourceLocal != SRC_WHISPER_LOCAL)
        chance = Governor_ApplyChainDecay(chance, chainDepth);

    if(g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat", "[Ollama Chat] Sender: {} ({}), Channel: {}, Depth: {}, Reply Chance: {}%, Candidate Bots: {}",
                player->GetName(), senderIsBot ? "BOT" : "PLAYER", ChatChannelSourceLocalStr[sourceLocal], chainDepth, chance, candidateBots.size());
    }

    // Not just an optimisation: this return also used to swallow the name-
    // mention path, so a channel set to 0% ignored a bot being addressed by
    // name. Direct address has its own chance, so only bail when both are off.
    if (chance == 0 && g_DirectAddressReplyChance == 0)
        return;
    
    std::vector<Player*> finalCandidates;
    
    // For whispers, handle directly - there should only be one receiver bot
    if (sourceLocal == SRC_WHISPER_LOCAL)
    {
        if (!candidateBots.empty())
        {
            Player* whisperBot = candidateBots[0]; // Should only be one bot for whispers
            if (!(g_DisableRepliesInCombat && whisperBot->IsInCombat()))
            {
                finalCandidates.push_back(whisperBot);
                if(g_DebugEnabled)
                {
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Whisper: Bot {} selected to respond", whisperBot->GetName());
                }
            }
        }
    }
    else
    {
        // Handle non-whisper chats with normal multi-bot logic
        std::vector<std::pair<size_t, Player*>> mentionedBots;

        // Whole-word, case-insensitive name match; see OllamaFindBotNameMention.
        auto isBotNameMentioned = [&trimmedMsg](const std::string& botName) -> size_t {
            return OllamaFindBotNameMention(trimmedMsg, botName);
        };

        for (Player* bot : candidateBots)
        {
            if (!bot)
            {
                continue;
            }
            if (g_DisableRepliesInCombat && bot->IsInCombat())
            {
                continue;
            }

            // Cross-faction say/yell renders as gibberish on the client, so a
            // fluent reply is the most immersion-breaking thing the module can
            // do. Skipping it also saves the round trip.
            if ((sourceLocal == SRC_SAY_LOCAL || sourceLocal == SRC_YELL_LOCAL) &&
                Roleplay_IsLanguageBarrier(player, bot))
            {
                continue;
            }

            size_t pos = isBotNameMentioned(bot->GetName());
            if (pos != std::string::npos)
            {
                mentionedBots.emplace_back(pos, bot);
                if(g_DebugEnabled)
                {
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} mentioned at position {} in message", bot->GetName(), pos);
                }
            }
        }

        if (!mentionedBots.empty())
        {
            // Sort by position to get the first mentioned bot
            std::sort(mentionedBots.begin(), mentionedBots.end(),
                      [](const std::pair<size_t, Player*> &a, const std::pair<size_t, Player*> &b) { return a.first < b.first; });
            Player* chosen = mentionedBots.front().second;
            if (!(g_DisableRepliesInCombat && chosen->IsInCombat()))
            {
                finalCandidates.push_back(chosen);
                if(g_DebugEnabled)
                {
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} selected (mentioned first at position {})", 
                            chosen->GetName(), mentionedBots.front().first);
                }
            }
        }
        else
        {
            for (Player* bot : candidateBots)
            {
                if (g_DisableRepliesInCombat && bot->IsInCombat())
                {
                    if(g_DebugEnabled)
                    {
                        LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} skipped - in combat", bot->GetName());
                    }
                    continue;
                }

                if ((sourceLocal == SRC_SAY_LOCAL || sourceLocal == SRC_YELL_LOCAL) &&
                    Roleplay_IsLanguageBarrier(player, bot))
                {
                    continue;
                }

                // A bot already mid-conversation with this person answers at
                // DirectAddressReplyChance rather than the ambient rate for the
                // channel. Someone who has been talking to you does not go 50/50
                // on whether to acknowledge your next sentence.
                const bool botIsAddressed =
                    OllamaIsDirectAddress(bot, player, sourceLocal, trimmedMsg, senderIsBot, scopeKey);
                const uint32_t botChance = botIsAddressed
                                               ? std::max(chance, g_DirectAddressReplyChance)
                                               : chance;

                uint32_t roll = urand(0, 99);
                if (roll < botChance)
                {
                    finalCandidates.push_back(bot);
                    if(g_DebugEnabled)
                    {
                        LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} PASSED chance roll ({} < {}%{})", bot->GetName(), roll, botChance, botIsAddressed ? ", addressed" : "");
                    }
                }
                else if(g_DebugEnabled)
                {
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Bot {} FAILED chance roll ({} >= {}%{})", bot->GetName(), roll, botChance, botIsAddressed ? ", addressed" : "");
                }
            }
        }
    }

    
    if (finalCandidates.empty())
    {
        if(g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] *** NO BOTS RESPONDING *** to {} from {} in {} channel. "
                    "Eligible: {}, Candidates: {}, Final: 0, Chance: {}%",
                    senderIsBot ? "BOT" : "PLAYER", player->GetName(), ChatChannelSourceLocalStr[sourceLocal],
                    eligibleBots.size(), candidateBots.size(), chance);
            LOG_INFO("module.ollamachat", "[Ollama Chat] No eligible bots found to respond to message '{}'. "
                    "Source: {}, Eligible bots: {}, Candidate bots: {}, Combat disabled: {}",
                    msg, ChatChannelSourceLocalStr[sourceLocal], eligibleBots.size(), 
                    candidateBots.size(), g_DisableRepliesInCombat);
        }
        return;
    }
    
    if (finalCandidates.size() > g_MaxBotsToPick)
    {
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(finalCandidates.begin(), finalCandidates.end(), g);
        uint32_t countToPick = urand(1, g_MaxBotsToPick);
        if(g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] Limiting {} bots to {} (MaxBotsToPick)", finalCandidates.size(), countToPick);
        }
        finalCandidates.resize(countToPick);
    }
    
    if(g_DebugEnabled && !finalCandidates.empty())
    {
        std::string botNames;
        for (Player* bot : finalCandidates)
        {
            if (!botNames.empty()) botNames += ", ";
            botNames += bot->GetName();
        }
        LOG_INFO("module.ollamachat", "[Ollama Chat] *** {} BOTS RESPONDING *** to {} from {} in {}: [{}]",
                finalCandidates.size(), senderIsBot ? "BOT" : "PLAYER", player->GetName(),
                ChatChannelSourceLocalStr[sourceLocal], botNames);
    }
    
    const uint64_t senderGuid = player->GetGUID().GetRawValue();

    for (Player* bot : finalCandidates)
    {
        if (!bot)
            continue;

        // Everything below runs on the world thread: prompt building reads
        // live world state, and the governor decides before we spend an LLM
        // call rather than after.
        // A line aimed at this bot is owed an answer, so it skips the pacing
        // cooldowns. The global messages-per-minute ceiling still applies.
        const bool directAddress =
            OllamaIsDirectAddress(bot, player, sourceLocal, trimmedMsg, senderIsBot, scopeKey);

        if (!Governor_CanSend(bot->GetGUID(), scopeKey, directAddress))
        {
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Bot {} skipped: cooldown or rate limit.", bot->GetName());
            continue;
        }

        if (bridgeProvider)
        {
            BridgeSubmit(bot, player, sourceLocal, msg, directAddress, scopeKey, chainDepth);
            continue;
        }

        std::string prompt = GenerateBotPrompt(bot, msg, player);
        if (prompt.empty())
            continue;

        OllamaChatRequest request;
        request.botGuid     = bot->GetGUID().GetRawValue();
        request.targetGuid  = senderGuid;
        request.source      = sourceLocal;
        request.channelName = channel ? channel->GetName() : std::string();
        request.channelId   = channel ? channel->GetChannelId() : 0;
        request.chainDepth  = chainDepth;
        request.directAddress = directAddress;
        request.scopeKey    = scopeKey;
        request.prompt      = std::move(prompt);
        request.botName     = bot->GetName();
        request.originMessage = msg;
        request.kind = (g_RoleplayEnable && g_RoleplayStrictness >= 1)
                           ? OllamaRequestKind::RoleplayReply
                           : OllamaRequestKind::ChatReply;
        request.triggerBotReplies = (sourceLocal != SRC_WHISPER_LOCAL);
        request.recordHistory     = !senderIsBot;
        request.updateSentiment   = !senderIsBot && g_EnableSentimentTracking;

        if (!OllamaDispatch_Submit(std::move(request)) && g_DebugEnabled)
        {
            LOG_INFO("module.ollamachat",
                     "[Ollama Chat] Bot {} reply dropped: dispatcher queue full.",
                     bot->GetName());
        }
    }
}

static bool IsBotEligibleForChatChannelLocal(Player* bot, Player* player, ChatChannelSourceLocal source, Channel* channel, Player* receiver)
{
    if (!bot || !player || bot == player)
    {
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] IsBotEligible: FAILED basic check - bot={}, player={}, same={}", 
                    (void*)bot, (void*)player, (bot == player));
        return false;
    }
    if (!PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] IsBotEligible: Bot {} FAILED - no PlayerbotAI", bot->GetName());
        return false;
    }
        
    // For whispers, only the specific receiver should respond
    if (source == SRC_WHISPER_LOCAL)
    {
        // Don't allow bot-to-bot whisper responses
        PlayerbotAI* senderAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);
        if (senderAI && senderAI->IsBotAI())
        {
            return false;
        }
        
        return (receiver && bot == receiver);
    }
    
    // Check team compatibility for non-proximity chats (except channels which can be cross-faction)
    // Say and Yell are proximity-based and don't require same faction
    bool isProximityChatSource = (source == SRC_SAY_LOCAL || source == SRC_YELL_LOCAL);
    if (!channel && !isProximityChatSource && bot->GetTeamId() != player->GetTeamId())
        return false;
    
    // For channels, check if bot is in the specific channel instance
    if (channel)
    {
        // Verify the channel is valid before proceeding
        if (!channel)
        {
            if(g_DebugEnabled)
            {
                LOG_ERROR("module.ollamachat", "[Ollama Chat] IsBotEligibleForChatChannelLocal: Channel is null");
            }
            return false;
        }
            
        // ONLY use exact channel instance check - NO Player::IsInChannel() anymore
        ChannelMgr* candidateCMgr = ChannelMgr::forTeam(bot->GetTeamId());
        if (!candidateCMgr)
            return false;
            
        Channel* candidateChannel = candidateCMgr->GetChannel(channel->GetName(), bot);
        // Verify both channels are valid and are the exact same instance
        if (!candidateChannel || candidateChannel != channel)
        {
            if(g_DebugEnabled)
            {
                LOG_INFO("module.ollamachat", "[Ollama Chat] IsBotEligibleForChatChannelLocal: Bot {} not in same channel instance '{}' - Bot team: {}, Channel ptr: {} vs {}", 
                        bot->GetName(), channel->GetName(), (int)bot->GetTeamId(),
                        (void*)candidateChannel, (void*)channel);
            }
            return false;
        }
        
        // Additional team check for cross-faction channels - only allow same faction unless it's a global channel
        if (bot->GetTeamId() != player->GetTeamId())
        {
            // Allow cross-faction only for specific global channels
            const uint32 chanId = channel->GetChannelId();
            const bool isGlobalChannel = (chanId == uint32(ChatChannelId::WORLD_DEFENSE) ||
                                          chanId == uint32(ChatChannelId::LOOKING_FOR_GROUP));
            if (!isGlobalChannel)
            {
                if(g_DebugEnabled)
                {
                    LOG_INFO("module.ollamachat", "[Ollama Chat] IsBotEligibleForChatChannelLocal: Bot {} different faction from player - Bot: {}, Player: {}, Channel: '{}'", bot->GetName(), (int)bot->GetTeamId(), (int)player->GetTeamId(), channel->GetName());
                }
                return false;
            }
        }
    }
    
    bool isInParty = (player->GetGroup() && bot->GetGroup() && (player->GetGroup() == bot->GetGroup()));
    float threshold = 0.0f;
    
    switch (source)
    {
        case SRC_SAY_LOCAL:    
            threshold = g_SayDistance;
            if (threshold > 0.0f)
            {
                if (!bot->IsInWorld() || !player->IsInWorld())
                    return false;
                    
                float distance = bot->GetDistance(player);
                return distance <= threshold;
            }
            return false;
            
        case SRC_YELL_LOCAL:   
            threshold = g_YellDistance;
            return (threshold > 0.0f && player->GetDistance(bot) <= threshold);
            
        case SRC_GUILD_LOCAL:
        case SRC_OFFICER_LOCAL:
            return (player->GetGuild() && bot->GetGuildId() == player->GetGuildId());
            
        case SRC_PARTY_LOCAL:
        case SRC_RAID_LOCAL:
            return isInParty;
            
        case SRC_WHISPER_LOCAL:
            // For whispers, the bot should only respond if it's the specific receiver
            return (receiver && bot == receiver);
            
        case SRC_GENERAL_LOCAL:
            // For channels like General, Trade, etc., no distance check - only channel membership matters
            // Channel membership was already checked above
            return true;
            
        default:
            return false;
    }
}

std::string GenerateBotPrompt(Player* bot, std::string playerMessage, Player* player)
{  
    if (!bot || !player) {
        return "";
    }
    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (botAI == nullptr) {
        return "";
    }
    ChatHelper* helper = botAI->GetChatHelper();
    if (helper == nullptr) {
        return "";
    }
    if (g_ChatPromptTemplate.empty()) {
        LOG_ERROR("module.ollamachat", "[Ollama Chat] GenerateBotPrompt: template is empty");
        return "";
    }

    AreaTableEntry const* botCurrentArea = botAI->GetCurrentArea();
    AreaTableEntry const* botCurrentZone = botAI->GetCurrentZone();

    uint64_t botGuid                = bot->GetGUID().GetRawValue();
    uint64_t playerGuid             = player->GetGUID().GetRawValue();

    std::string personality         = GetBotPersonality(bot);
    std::string personalityPrompt   = GetPersonalityPromptAddition(personality);
    std::string botName             = bot->GetName();
    uint32_t botLevel               = bot->GetLevel();
    uint8_t botGenderByte           = bot->getGender();
    std::string botAreaName         = botCurrentArea ? botAI->GetLocalizedAreaName(botCurrentArea): "UnknownArea";
    std::string botZoneName         = botCurrentZone ? botAI->GetLocalizedAreaName(botCurrentZone): "UnknownZone";
    std::string botMapName          = OllamaContinentName(bot);
    std::string botClass            = botAI->GetChatHelper()->FormatClass(bot->getClass());
    std::string botRace             = botAI->GetChatHelper()->FormatRace(bot->getRace());
    std::string botRole             = CleanRoleForPrompt(ChatHelper::FormatClass(bot, AiFactory::GetPlayerSpecTab(bot)));
    std::string botGender           = (botGenderByte == 0 ? "Male" : "Female");
    std::string botFaction          = (bot->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
    std::string botGuild            = (bot->GetGuild() ? bot->GetGuild()->GetName() : "No Guild");
    std::string botGroupStatus      = (bot->GetGroup() ? "In a group" : "Solo");
    uint32_t botGold                = bot->GetMoney() / 10000;

    std::string playerName          = player->GetName();
    uint32_t playerLevel            = player->GetLevel();
    std::string playerClass         = botAI->GetChatHelper()->FormatClass(player->getClass());
    std::string playerRace          = botAI->GetChatHelper()->FormatRace(player->getRace());
    std::string playerRole          = CleanRoleForPrompt(ChatHelper::FormatClass(player, AiFactory::GetPlayerSpecTab(player)));
    uint8_t playerGenderByte        = player->getGender();
    std::string playerGender        = (playerGenderByte == 0 ? "Male" : "Female");
    std::string playerFaction       = (player->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
    std::string playerGuild         = (player->GetGuild() ? player->GetGuild()->GetName() : "No Guild");
    std::string playerGroupStatus   = (player->GetGroup() ? "In a group" : "Solo");
    uint32_t playerGold             = player->GetMoney() / 10000;
    float playerDistance            = player->IsInWorld() && bot->IsInWorld() ? player->GetDistance(bot) : -1.0f;

    std::string chatHistory         = GetBotHistoryPrompt(botGuid, playerGuid, playerMessage);
    std::string sentimentInfo       = GetSentimentPromptAddition(bot, player);

    // Retrieve RAG information if enabled
    std::string ragInfo;
    if (g_EnableRAG && g_RAGSystem) {
        auto ragResults = g_RAGSystem->RetrieveRelevantInfo(playerMessage, g_RAGMaxRetrievedItems, g_RAGSimilarityThreshold);
        std::string ragContent = g_RAGSystem->GetFormattedRAGInfo(ragResults);
        if (!ragContent.empty()) {
            ragInfo = SafeFormat(g_RAGPromptTemplate, fmt::arg("rag_info", ragContent));
        }
        if (g_DebugEnabled) {
            LOG_INFO("module.ollamachat", "[Ollama Chat] RAG Debug - Enabled: {}, System: {}, Message: '{}', Results: {}, Content length: {}",
                g_EnableRAG, (void*)g_RAGSystem, playerMessage, ragResults.size(), ragContent.length());
        }
    } else if (g_DebugEnabled) {
        LOG_INFO("module.ollamachat", "[Ollama Chat] RAG Debug - Not enabled or no system - Enabled: {}, System: {}",
            g_EnableRAG, (void*)g_RAGSystem);
    }

    std::string extraInfo = SafeFormat(
        g_ChatExtraInfoTemplate,
        fmt::arg("bot_race", botRace),
        fmt::arg("bot_gender", botGender),
        fmt::arg("bot_role", botRole),
        fmt::arg("bot_faction", botFaction),
        fmt::arg("bot_guild", botGuild),
        fmt::arg("bot_group_status", botGroupStatus),
        fmt::arg("bot_gold", Roleplay_GoldText(botGold)),
        fmt::arg("player_race", playerRace),
        fmt::arg("player_gender", playerGender),
        fmt::arg("player_role", playerRole),
        fmt::arg("player_faction", playerFaction),
        fmt::arg("player_guild", playerGuild),
        fmt::arg("player_group_status", playerGroupStatus),
        fmt::arg("player_gold", Roleplay_GoldText(playerGold)),
        fmt::arg("player_distance", Roleplay_DistanceText(playerDistance)),
        fmt::arg("bot_area", botAreaName),
        fmt::arg("bot_zone", botZoneName),
        fmt::arg("bot_map", botMapName)
    );
    
    std::string prompt = SafeFormat(
        g_ChatPromptTemplate,
        fmt::arg("bot_name", botName),
        fmt::arg("bot_level", Roleplay_LevelText(botLevel)),
        fmt::arg("bot_class", botClass),
        fmt::arg("bot_personality", personalityPrompt),
        fmt::arg("bot_personality_name", personality),
        fmt::arg("player_level", Roleplay_LevelText(playerLevel)),
        fmt::arg("player_class", playerClass),
        fmt::arg("player_name", playerName),
        fmt::arg("player_message", playerMessage),
        fmt::arg("extra_info", extraInfo),
        fmt::arg("chat_history", chatHistory),
        fmt::arg("sentiment_info", sentimentInfo)
    );

    // Add RAG information to the prompt if available
    if (!ragInfo.empty()) {
        prompt += ragInfo + "\n";
    }

    if(g_EnableChatBotSnapshotTemplate)
    {
        prompt += GenerateBotGameStateSnapshot(bot);
    }

    // What this bot remembers, and how it feels about people. Bounded by
    // their own token budgets, so this cannot grow the prompt without limit.
    prompt += Memory_BuildPromptSection(bot, player);

    // Race and class as a voice rather than as a stat line.
    prompt += Roleplay_BuildVoicePrompt(bot);

    // Let the model gesture. The tag is parsed back out and stripped before
    // the line is spoken, so it never reaches chat as text.
    prompt += Expression_BuildGesturePrompt();

    // Debug logging for full prompt including RAG information
    if (g_DebugEnabled && g_DebugShowFullPrompt) {
        LOG_INFO("module.ollamachat", "[Ollama Chat] Full prompt sent to bot {} for player {}: {}", botName, playerName, prompt);
    }

    return prompt;
}

// --------------------------------------------------------------------------
// Emote reactions
// --------------------------------------------------------------------------

std::string BuildEmoteReactionPrompt(Player* bot, Player* player, uint32_t textEmote)
{
    if (!bot || !player)
        return "";

    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (!botAI || !botAI->GetChatHelper())
        return "";

    std::string emoteName = LookupTextEmoteName(textEmote);
    if (emoteName.empty())
        emoteName = "gestures at";

    const std::string personality       = GetBotPersonality(bot);
    const std::string personalityPrompt = GetPersonalityPromptAddition(personality);

    std::string prompt = SafeFormat(
        g_EmoteReactionPromptTemplate,
        fmt::arg("bot_name", bot->GetName()),
        fmt::arg("bot_level", Roleplay_LevelText(bot->GetLevel())),
        fmt::arg("bot_class", botAI->GetChatHelper()->FormatClass(bot->getClass())),
        fmt::arg("bot_race", botAI->GetChatHelper()->FormatRace(bot->getRace())),
        fmt::arg("bot_personality", personalityPrompt),
        fmt::arg("bot_personality_name", personality),
        fmt::arg("player_name", player->GetName()),
        fmt::arg("player_class", botAI->GetChatHelper()->FormatClass(player->getClass())),
        fmt::arg("player_race", botAI->GetChatHelper()->FormatRace(player->getRace())),
        fmt::arg("emote_name", emoteName));

    if (g_RoleplayEnable)
        prompt += Roleplay_BuildVoicePrompt(bot);

    return prompt;
}

// --------------------------------------------------------------------------
// Maintenance
// --------------------------------------------------------------------------

void OllamaChatMaintenance::OnPlayerLogout(Player* player)
{
    if (!player)
        return;

    // These maps used to grow for the lifetime of the process, and the event
    // cooldown one was keyed on a raw Player* that could be recycled by a
    // different character at the same address.
    const ObjectGuid guid = player->GetGUID();

    Governor_OnPlayerLogout(guid);
    Memory_ForgetBot(guid);
    OllamaRandomChatter_ForgetBot(guid);
    Topics_ForgetBot(guid);
    OllamaDispatch_BridgeWatchLogout(guid.GetRawValue());  // J6e-c: its watches (as bot or partner) end now.
}
