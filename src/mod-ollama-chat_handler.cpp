#include "Log.h"
#include "Language.h"
#include "Player.h"
#include "Chat.h"
#include "Channel.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
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
#include "mod-ollama-chat_bridge_adapter.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat-utilities.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_response.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat_random.h"
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

void PlayerBotChatHandler::ProcessChat(Player* player, uint32_t /*type*/, uint32_t lang, std::string& msg,
                                       ChatChannelSourceLocal sourceLocal, Channel* channel, Player* receiver)
{
    if (player == nullptr) {
        LOG_ERROR("module.ollamachat", "[Ollama Chat] ProcessChat: player is null");
        return;
    }
    if (msg.empty()) {
        return;
    }
    if (lang == LANG_ADDON) return;
    // Only a human's line opens a bridge request. A bot's own replies (BridgeHuman false) and bot chains never do,
    // so generated speech cannot feed back into the bridge or start a bot-to-bot loop.
    if (!g_Enable || !BridgeHuman(player) || channel ||
        (sourceLocal != SRC_WHISPER_LOCAL && sourceLocal != SRC_PARTY_LOCAL && sourceLocal != SRC_SAY_LOCAL &&
         sourceLocal != SRC_YELL_LOCAL))
        return;
    // A whisper or party line with the command indicator is for playerbots' own chat hooks (the same rule,
    // ChatCommandLine.h): no request and no budget. Say and yell have no playerbots hook, so such a line stays chat.
    std::string commandLine;
    if ((sourceLocal == SRC_WHISPER_LOCAL || sourceLocal == SRC_PARTY_LOCAL) &&
        PlayerbotBridge::ExtractChatCommand(msg, commandLine))
    {
        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] Command line (indicator): no model reply.");
        return;
    }
    std::string chanName = (channel != nullptr) ? channel->GetName() : "Unknown";
    uint32_t channelId = (channel != nullptr) ? channel->GetChannelId() : 0;
    std::string receiverName = (receiver != nullptr) ? receiver->GetName() : "None";
    if (g_DebugEnabled)
    {
        LOG_INFO(
            "server.loading",
            "[Ollama Chat] Player {} sent msg: '{}' | Source: {} | Channel Name: {} | Channel ID: {} | Receiver: {}",
            player->GetName(), msg, (int)sourceLocal, chanName, channelId, receiverName);
    }

    // Only the command indicator above decides; no word list silences a line.
    std::string trimmedMsg = rtrim(msg);
    
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
    // J5g: a human's line (the gate above lets only those through). A bridge reply to a human is exempt from the
    // combat filter below (DS.14: a fighting bot may just act).
    bool const bridgeHumanLine = !senderIsBot;

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
        // Bot-to-bot: bots do not hold conversations with nobody listening.
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
    
    if(g_DebugEnabled)
    {
        LOG_INFO("module.ollamachat",
                 "[Ollama Chat] Sender: {} ({}), Channel: {}, Reply Chance: {}%, Candidate Bots: {}", player->GetName(),
                 senderIsBot ? "BOT" : "PLAYER", ChatChannelSourceLocalStr[sourceLocal], chance, candidateBots.size());
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
            if (!OllamaBridge::CombatSkipsReply(g_DisableRepliesInCombat, bridgeHumanLine, whisperBot->IsInCombat()))
            {
                finalCandidates.push_back(whisperBot);
                if(g_DebugEnabled)
                {
                    LOG_INFO("module.ollamachat", "[Ollama Chat] Whisper: Bot {} selected to respond", whisperBot->GetName());
                }
            }
            else if (g_DebugEnabled)
            {
                LOG_INFO("module.ollamachat", "[Ollama Chat] Whisper: Bot {} skipped - in combat",
                         whisperBot->GetName());
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
            if (OllamaBridge::CombatSkipsReply(g_DisableRepliesInCombat, bridgeHumanLine, bot->IsInCombat()))
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
            if (!OllamaBridge::CombatSkipsReply(g_DisableRepliesInCombat, bridgeHumanLine, chosen->IsInCombat()))
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
                if (OllamaBridge::CombatSkipsReply(g_DisableRepliesInCombat, bridgeHumanLine, bot->IsInCombat()))
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

        BridgeSubmit(bot, player, sourceLocal, msg, directAddress, scopeKey);
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
    OllamaDispatch_BridgeWatchLogout(guid.GetRawValue());  // J6e-c: its watches (as bot or partner) end now.
}
