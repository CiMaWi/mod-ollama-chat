#include "Log.h"
#include "Language.h"
#include "Player.h"
#include "Chat.h"
#include "Channel.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "BridgeAbilities.h"
#include "BridgeBotFacts.h"
#include "BridgeBotState.h"
#include "BridgeBotTraits.h"
#include "RecordWords.h"
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

// DS.14/J5f: the bot's own state for one request, read by playerbots on world (the chat hook, map threads idle) and
// copied as plain values; the envelope filters and bounds it. Outside the extracted adapter.
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
    // LC10: what it perceives, its servant and mount, its recent deeds; Build bounds them (deeds whole, LC7's cap).
    state.around.clear();
    state.around.reserve(facts.around.size());
    for (PlayerbotBridge::AroundEntry& entry : facts.around)
        state.around.push_back(
            {std::move(entry.Name), PlayerbotBridge::AroundKindWord(entry.Kind), std::move(entry.Detail), entry.Count});
    state.servant = std::move(facts.servantName);
    state.mount = std::move(facts.mountName);
    state.deeds = std::move(facts.deeds);
}

static_assert(OllamaBridge::MaxStateDeedBytes == RecordWords::MAX_SENTENCE,
              "a deed on the wire is one whole record sentence (LC7's cap)");
static_assert(OllamaBridge::MaxStateAround == PlayerbotBridge::MaxAround, "the wire carries every listed name");

// LC10: the bot's lasting facts (playerbots ReadBotFacts, world thread, in memory) with who it is from the bot's own
// getters, and its trait words (cache read). Per request like the state, never in a capture;
// Build bounds every value and leaves out what is empty. No database access.
void BridgeFactsAndTraits(Player* bot, OllamaBridge::Context& input)
{
    input.facts.reset();
    PlayerbotBridge::BotFacts facts;
    if (PlayerbotBridge::ReadBotFacts(bot->GetGUID(), facts))
    {
        OllamaBridge::BotFacts& wire = input.facts.emplace();
        wire.name = bot->GetName();
        wire.race = FormatPlayerRace(bot->getRace());
        wire.className = FormatPlayerClass(bot->getClass());
        wire.gender = bot->getGender() == GENDER_FEMALE ? "female" : "male";
        wire.level = bot->GetLevel();
        wire.side = bot->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde";
        wire.phrase = std::move(facts.phrase);
        wire.crafts = std::move(facts.crafts);
        wire.useful = std::move(facts.useful);
        wire.servants = std::move(facts.servants);
        wire.standing = std::move(facts.standing);
        wire.guild = std::move(facts.guild);
        wire.relatives = std::move(facts.relatives);
    }
    PlayerbotBridge::BotPersonality traits;
    input.traits = {};
    if (PlayerbotBridge::ReadBotPersonality(bot->GetGUID(), traits))
    {
        input.traits.words = std::move(traits.words);
        input.traits.areas = std::move(traits.areas);
        input.traits.interests = std::move(traits.interests);
    }
}

static_assert(static_cast<uint8_t>(PlayerbotBridge::BotStateTopic::Spells) ==
                  static_cast<uint8_t>(OllamaBridge::StateTopic::Spells) &&
              static_cast<uint8_t>(PlayerbotBridge::BotStateTopic::Plan) ==
                  static_cast<uint8_t>(OllamaBridge::StateTopic::Plan) &&
              static_cast<uint8_t>(PlayerbotBridge::BotStateTopic::Options) ==
                  static_cast<uint8_t>(OllamaBridge::StateTopic::Options) &&
              static_cast<uint8_t>(PlayerbotBridge::BotStateTopic::Route) ==
                  static_cast<uint8_t>(OllamaBridge::StateTopic::Route) &&
              static_cast<uint8_t>(PlayerbotBridge::BotStateTopic::QuestPlan) ==
                  static_cast<uint8_t>(OllamaBridge::StateTopic::QuestPlan) &&
              static_cast<uint8_t>(PlayerbotBridge::BotStateTopic::Bags) ==
                  static_cast<uint8_t>(OllamaBridge::StateTopic::Bags) &&
              static_cast<uint8_t>(PlayerbotBridge::BotStateTopic::Places) ==
                  static_cast<uint8_t>(OllamaBridge::StateTopic::Places) &&
              static_cast<uint8_t>(PlayerbotBridge::BotStateTopic::History) ==
                  static_cast<uint8_t>(OllamaBridge::StateTopic::History) &&
              static_cast<uint8_t>(PlayerbotBridge::BotStateTopic::Lately) ==
                  static_cast<uint8_t>(OllamaBridge::StateTopic::Lately) &&
              static_cast<uint8_t>(PlayerbotBridge::BotStateTopic::Known) ==
                  static_cast<uint8_t>(OllamaBridge::StateTopic::Known) &&
              OllamaBridge::StateTopicCount <= PlayerbotBridge::TopicRows().size());
// LC16c: the fourteen topics above keep their values; a topic playerbots appends is answered in the lines form.

// LC3: the channel takes a parameter's field as playerbots' value (an act's code is its Intent value); each pair
// is one value.
template <typename Pairs>
constexpr bool BridgeValuesAgree(Pairs const& pairs)
{
    std::size_t index = 0;
    for (auto const& [bot, wire] : pairs)
    {
        if (static_cast<std::size_t>(bot) != index || static_cast<std::size_t>(wire) != index)
            return false;
        ++index;
    }
    return true;
}

constexpr std::pair<PlayerbotBridge::OrderField, OllamaBridge::AbilityField> BridgeFieldPairs[] = {
    {PlayerbotBridge::OrderField::Target, OllamaBridge::AbilityField::Target},
    {PlayerbotBridge::OrderField::Icon, OllamaBridge::AbilityField::Icon},
    {PlayerbotBridge::OrderField::PlaceName, OllamaBridge::AbilityField::PlaceName},
    {PlayerbotBridge::OrderField::Position, OllamaBridge::AbilityField::Position},
    {PlayerbotBridge::OrderField::QuestId, OllamaBridge::AbilityField::QuestId},
    {PlayerbotBridge::OrderField::Shape, OllamaBridge::AbilityField::Shape},
    {PlayerbotBridge::OrderField::Enabled, OllamaBridge::AbilityField::Enabled},
    {PlayerbotBridge::OrderField::ItemName, OllamaBridge::AbilityField::ItemName},
    {PlayerbotBridge::OrderField::SpellName, OllamaBridge::AbilityField::SpellName},
    {PlayerbotBridge::OrderField::Choice, OllamaBridge::AbilityField::Choice},
    {PlayerbotBridge::OrderField::Count, OllamaBridge::AbilityField::Count}};
static_assert(BridgeValuesAgree(BridgeFieldPairs));

// J5f-b: one topic of the bot's own state for a bot_state query, read by playerbots on world (WorldChannel::Update,
// map threads idle) and copied as raw entries; the channel filters, bounds and names them. WT18b: place is the
// topic's argument (route's place; LC16c: an open topic's name), a value from the staged query.
bool BridgeBotStateTopic(uint64_t botGuid, OllamaBridge::StateTopic topic, std::string const& place,
                         std::vector<OllamaBridge::StateEntry>& entries)
{
    entries.clear();
    std::vector<PlayerbotBridge::BotStateItem> items;
    if (!PlayerbotBridge::ReadBotStateTopic(ObjectGuid(botGuid), static_cast<PlayerbotBridge::BotStateTopic>(topic),
                                            place, items))
        return false;
    for (auto& item : items)
    {
        // LCB1e: the game's row word as it sends it (the bridge decides what it shows); inventory, quests and
        // strategies still send a code only, so their fixed words follow it here.
        OllamaBridge::StateEntry entry{std::move(item.name), std::move(item.word), item.value, item.extra,
                                       std::move(item.text)};
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

// LC3: the channel's ability index from playerbots' table: the catalogue with its hash, each row's intent or topic and
// parameter fields. Empty (refused by the channel) when the catalogue and the table disagree.
static OllamaBridge::AbilityIndex BridgeAbilityIndex()
{
    std::vector<PlayerbotBridge::AbilityData> const catalogue = PlayerbotBridge::Catalogue();
    std::span<PlayerbotBridge::AbilityRow const> const acts = PlayerbotBridge::AbilityRows();
    std::span<PlayerbotBridge::TopicRow const> const topics = PlayerbotBridge::TopicRows();
    if (catalogue.size() != acts.size() + topics.size())
        return {};
    OllamaBridge::AbilityIndex index;
    index.hash = PlayerbotBridge::CatalogueHash(catalogue);
    index.version = PlayerbotBridge::CatalogueVersion;
    index.maxSteps = PlayerbotBridge::MaxSteps;
    for (std::size_t position = 0; position < catalogue.size(); ++position)
    {
        PlayerbotBridge::AbilityData const& data = catalogue[position];
        bool const topic = position >= acts.size();
        PlayerbotBridge::TopicRow const* const topicRow = topic ? &topics[position - acts.size()] : nullptr;
        PlayerbotBridge::AbilityRow const* const actRow = topic ? nullptr : &acts[position];
        std::span<PlayerbotBridge::ParamRow const> const params = topic ? topicRow->Params : actRow->Params;
        if ((data.Kind == PlayerbotBridge::AbilityKind::Topic) != topic ||
            data.Name != (topic ? topicRow->Name : actRow->Name) || data.Params.size() != params.size())
            return {};
        OllamaBridge::AbilityRow& row = index.rows.emplace_back();
        row.topic = topic;
        row.code = topic ? static_cast<uint8_t>(topicRow->Topic) : static_cast<uint8_t>(actRow->Operation);
        row.name = data.Name;
        row.sentence = data.Sentence;
        for (std::size_t param = 0; param < params.size(); ++param)
            row.params.push_back({data.Params[param].Name, PlayerbotBridge::KindWord(data.Params[param].Kind),
                                  data.Params[param].Sentence, data.Params[param].Choices,
                                  static_cast<OllamaBridge::AbilityField>(params[param].Field)});
        for (PlayerbotBridge::RefusalData const& refusal : data.Refusals)
            row.refusals.push_back({refusal.Word, refusal.Sentence});
        row.effect = PlayerbotBridge::PartWord(data.Lasting);
        row.done = PlayerbotBridge::CompletionWord(data.Done);
        row.state = data.StatePhrase;
        row.event = data.Event;
        row.limit = data.Limit;
    }
    return index;
}

void OllamaChat_BridgeInstallAbilities()
{
    OllamaBridge::AbilityIndex index = BridgeAbilityIndex();
    std::size_t const rows = index.rows.size();
    uint64_t const hash = index.hash;
    std::size_t const bytes = OllamaBridge::SetAbilityIndex(std::move(index));
    if (OllamaBridge::Abilities().rows.empty())
        LOG_ERROR("module.ollamachat",
                  "[Ollama Chat] Bridge ability catalogue refused ({} rows, {} of {} bytes): the bridge channel takes "
                  "no command and no bot_state query.",
                  rows, bytes, OllamaBridge::ChannelMaxCatalogueBytes);
    else if (!OllamaBridge::CatalogueInstalled())
        LOG_ERROR("module.ollamachat",
                  "[Ollama Chat] Bridge ability catalogue not sent ({} rows, {} of {} bytes, or not UTF-8): commands "
                  "and bot_state queries stand, the catalogue query answers unavailable.",
                  rows, bytes, OllamaBridge::ChannelMaxCatalogueBytes);
    else
        LOG_INFO("module.ollamachat", "[Ollama Chat] Bridge ability catalogue: {} rows, hash {:016x}, {} bytes.", rows,
                 hash, bytes);
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
struct BridgeBinding
{
    uint64_t generation = 0;
    OllamaBridge::BindingCapture view;
    OllamaBridge::Deadline heartbeat;  // LCB3c: the next capture is due then.
    OllamaBridge::Deadline idleUntil;
    std::string memory;
};

// World-thread only. Entries contain values, never Player/Group pointers. A refresh captures a binding once per
// heartbeat (at most 64 * 40 members), not on every world tick.
std::vector<BridgeBinding> g_bridgeBindings;
uint64_t g_bridgeRevision = 0;

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

    bool ReadTopic(OllamaBridge::WorldTicket const& ticket, OllamaBridge::StateTopic topic, std::string const& place,
                   std::vector<OllamaBridge::StateEntry>& entries) override
    {
        return BridgeBotStateTopic(ticket.scope.botGuid, topic, place, entries);
    }

    bool ReadWatch(uint64_t botGuid, uint64_t partnerGuid, OllamaBridge::WatchFacts& facts) override
    {
        return BridgeWatchFacts(botGuid, partnerGuid, facts);
    }
};

// J5e: `why` names the first failed rule; every false return sets it.
bool BridgeCapture(uint64_t botGuid, uint64_t speakerGuid, OllamaBridge::DispatchSource source,
                   OllamaBridge::BindingCapture& view, char const** why = nullptr)
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
    }
    view.context.speakerGuid = speakerGuid;
    view.context.channel = guard.scope.channel;
    view.context.groupScope = group ? std::to_string(guard.groupGuid) : "";
    view.context.speakerControlsBot = speakerControlsBot;
    view.context.observations = {"Your name is " + bot->GetName() + ".", "The speaker is " + speaker->GetName() + "."};
    return true;
}

// LCB3c: a binding lives while the world generation and its scope and audience hold (SameScopeAndAudience).
bool BridgeSame(BridgeBinding const& binding, OllamaBridge::BindingCapture const& current)
{
    return binding.generation == OllamaDispatch_BridgeWorld().Generation() &&
           OllamaBridge::SameScopeAndAudience(binding.view, current);
}

// LCB3c: a kept binding takes the fresh capture (names, group, place, whether the partner leads the bot) under its own
// scope and revisions, and its next capture is one heartbeat away.
void BridgeKeep(BridgeBinding& binding, OllamaBridge::BindingCapture&& current, OllamaBridge::Clock now)
{
    current.guard.scope = binding.view.guard.scope;
    binding.view = std::move(current);
    binding.heartbeat = OllamaBridge::CaptureDeadline(now, OllamaBridge::BindingHeartbeatMs);
}

void BridgeErase(std::vector<BridgeBinding>::iterator it)
{
    auto const& scope = it->view.guard.scope;
    OllamaDispatch_BridgeWorld().InvalidateScope(scope.botGuid, scope.contextId);
    g_bridgeBindings.erase(it);
}

// Fresh capture of the ticket's binding. A failed capture or another scope or audience erases the binding and
// returns end; a kept binding holds the fresh capture.
std::vector<BridgeBinding>::iterator BridgeObserveFresh(OllamaBridge::WorldTicket const& ticket)
{
    auto it = std::find_if(g_bridgeBindings.begin(), g_bridgeBindings.end(), [&](BridgeBinding const& item)
                           { return item.generation == ticket.generation && item.view.guard.scope == ticket.scope; });
    if (it == g_bridgeBindings.end())
        return it;
    auto const now = OllamaBridge::Now();
    auto const& captured = it->view.guard;
    OllamaBridge::BindingCapture current;
    if (OllamaBridge::Expired(it->idleUntil, now) ||
        !BridgeCapture(captured.scope.botGuid, captured.speakerGuid, captured.source, current) ||
        !BridgeSame(*it, current))
    {
        BridgeErase(it);
        return g_bridgeBindings.end();
    }
    BridgeKeep(*it, std::move(current), now);
    return it;
}

bool BridgeSubmit(Player* bot, Player* speaker, ChatChannelSourceLocal source, std::string const& text, bool addressed,
                  std::string const& scopeKey, OllamaBridge::Watch const* wake = nullptr)
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
    if (text.empty() || text.size() > MaxTextBytes ||
        ((origin == DispatchSource::Say || origin == DispatchSource::Yell) && !addressed && !wake))
        return false;
    BindingCapture view;
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
    auto it = std::find_if(g_bridgeBindings.begin(), g_bridgeBindings.end(),
                           [&](BridgeBinding const& item)
                           {
                               return item.view.guard.scope.botGuid == view.guard.scope.botGuid &&
                                      item.view.guard.scope.contextId == view.guard.scope.contextId;
                           });
    auto const now = Now();
    // LCB3c: another scope or audience renews the binding: a new revision, a native context reset and no remembered
    // exchange. A kept binding takes this capture; the line's own capture also counts as its heartbeat.
    if (it != g_bridgeBindings.end() && (Expired(it->idleUntil, now) || !BridgeSame(*it, view)))
    {
        BridgeErase(it);
        it = g_bridgeBindings.end();
    }
    if (it != g_bridgeBindings.end())
        BridgeKeep(*it, std::move(view), now);
    else
    {
        if (g_bridgeBindings.size() >= MaxWorldScopes || g_bridgeRevision == UINT64_MAX)
            return false;
        auto const revision = std::to_string(++g_bridgeRevision);
        auto& scope = view.guard.scope;
        scope.contextRevision = scope.membershipRevision = scope.perceptionScope = revision;
        if (!OllamaDispatch_BridgeWorld().ObserveScope(scope, now))
            return false;
        g_bridgeBindings.push_back({OllamaDispatch_BridgeWorld().Generation(), std::move(view),
                                    CaptureDeadline(now, BindingHeartbeatMs), CaptureDeadline(now, MaxLifetimeMs)});
        it = std::prev(g_bridgeBindings.end());
    }
    it->idleUntil = CaptureDeadline(now, MaxLifetimeMs);
    auto input = it->view.context;
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
    BridgeFactsAndTraits(bot, input);  // LC10: the same rule for the lasting facts and the trait words.
    OllamaChatRequest request;
    request.botGuid = bot->GetGUID().GetRawValue();
    request.targetGuid = speaker->GetGUID().GetRawValue();
    request.botName = bot->GetName();
    request.source = source;
    request.scopeKey = scopeKey;
    request.directAddress = addressed;
    return OllamaDispatch_SubmitBridge(std::move(request), it->view.guard.scope, std::move(input), it->view.guard,
                                       30000);
}
}  // namespace

OllamaBridge::QueryWorldSource& OllamaChat_BridgeQuerySource()
{
    static BridgeQuerySource source;
    return source;
}

void OllamaChat_BridgeReset() { g_bridgeBindings.clear(); }

void OllamaChat_BridgeRefresh()
{
    using namespace OllamaBridge;
    auto const now = Now();
    OllamaDispatch_BridgeWorld().Expire(now);
    for (auto it = g_bridgeBindings.begin(); it != g_bridgeBindings.end();)
    {
        // Every tick: the world generation, the lifetime and the scope's liveness (cheap). LCB3c: the game capture
        // only when the binding's heartbeat is due.
        bool keep = it->generation == OllamaDispatch_BridgeWorld().Generation() && !Expired(it->idleUntil, now);
        if (keep && Expired(it->heartbeat, now))
        {
            BindingCapture current;
            auto const& guard = it->view.guard;
            keep = BridgeCapture(guard.scope.botGuid, guard.speakerGuid, guard.source, current) &&
                   BridgeSame(*it, current);
            if (keep)
                BridgeKeep(*it, std::move(current), now);
        }
        if (keep && OllamaDispatch_BridgeWorld().ObserveScope(it->view.guard.scope, now))
            ++it;
        else
        {
            auto const index = it - g_bridgeBindings.begin();
            BridgeErase(it);
            it = g_bridgeBindings.begin() + index;
        }
    }
}

bool OllamaChat_BridgeObserve(OllamaBridge::WorldTicket const& ticket, OllamaBridge::DispatchGuard& guard)
{
    auto const it = BridgeObserveFresh(ticket);
    if (it == g_bridgeBindings.end())
        return false;
    // Equality was established from fresh GUID resolution; revisions cannot be resurrected by matching values later.
    guard = it->view.guard;
    return true;
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
    return BridgeSubmit(bot, partner, source, WakeText(watch.condition), false, BridgeWakeScopeKey(partner, source),
                        &watch);
}
// END BRIDGE WORLD ADAPTER

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
