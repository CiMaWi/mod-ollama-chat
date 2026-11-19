// The bridge's world adapter (LCB4): the bot's own state, facts and traits for one request, the ability index,
// the watch reads, and the bindings of the conversations the bridge holds. World thread only.
#include "mod-ollama-chat_bridge_adapter.h"

#include <algorithm>
#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "BridgeAbilities.h"
#include "BridgeBotFacts.h"
#include "BridgeBotState.h"
#include "BridgeBotTraits.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RecordWords.h"
#include "SharedDefines.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_world.h"

// Helper function to format class name for any player
static std::string FormatPlayerClass(uint8_t classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR:
            return "Warrior";
        case CLASS_PALADIN:
            return "Paladin";
        case CLASS_HUNTER:
            return "Hunter";
        case CLASS_ROGUE:
            return "Rogue";
        case CLASS_PRIEST:
            return "Priest";
        case CLASS_DEATH_KNIGHT:
            return "Death Knight";
        case CLASS_SHAMAN:
            return "Shaman";
        case CLASS_MAGE:
            return "Mage";
        case CLASS_WARLOCK:
            return "Warlock";
        case CLASS_DRUID:
            return "Druid";
        default:
            return "Unknown";
    }
}

// Helper function to format race name for any player
static std::string FormatPlayerRace(uint8_t raceId)
{
    switch (raceId)
    {
        case RACE_HUMAN:
            return "Human";
        case RACE_ORC:
            return "Orc";
        case RACE_DWARF:
            return "Dwarf";
        case RACE_NIGHTELF:
            return "Night Elf";
        case RACE_UNDEAD_PLAYER:
            return "Undead";
        case RACE_TAUREN:
            return "Tauren";
        case RACE_GNOME:
            return "Gnome";
        case RACE_TROLL:
            return "Troll";
        case RACE_BLOODELF:
            return "Blood Elf";
        case RACE_DRAENEI:
            return "Draenei";
        default:
            return "Unknown";
    }
}

// DS.14/J5f: the bot's own state for one request, read by playerbots on world (the chat hook, map threads idle); the
// envelope filters and bounds it.
void BridgeBotStateFacts(Player* bot, Player* partner, OllamaBridge::Context& input)
{
    if (!PlayerbotBridge::ReadBotState(bot->GetGUID(), partner->GetGUID(), input.state.emplace()))
        input.state.reset();
}

static_assert(OllamaBridge::MaxStateDeedBytes == RecordWords::MAX_SENTENCE,
              "a deed on the wire is one whole record sentence (LC7's cap)");
static_assert(OllamaBridge::MaxStateAround == PlayerbotBridge::MaxAround, "the wire carries every listed name");

// LC10: the bot's lasting facts (playerbots ReadBotFacts, world thread, in memory) with who it is from the bot's own
// getters, and its trait words (cache read). Per request like the state, never in a capture;
// Build bounds every value and leaves out what is empty. No database access.
void BridgeFactsAndTraits(Player* bot, OllamaBridge::Context& input)
{
    if (PlayerbotBridge::ReadBotFacts(bot->GetGUID(), input.facts.emplace()))
        input.who = {bot->GetName(),
                     FormatPlayerRace(bot->getRace()),
                     FormatPlayerClass(bot->getClass()),
                     bot->getGender() == GENDER_FEMALE ? "female" : "male",
                     bot->GetLevel(),
                     static_cast<uint8_t>(bot->GetTeamId())};
    else
        input.facts.reset();
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
        row.effect = PlayerbotBridge::EffectWord;
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
    PlayerbotBridge::BotStateFacts state;
    if (!PlayerbotBridge::ReadBotState(bot->GetGUID(), partner->GetGUID(), state))
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
}  // namespace

bool BridgeHuman(Player* player)
{
    return player && player->IsInWorld() && OllamaIsRealPlayer(player) &&
           ObjectAccessor::FindConnectedPlayer(player->GetGUID()) == player;
}

namespace
{
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
}  // namespace

bool BridgeSubmit(Player* bot, Player* speaker, ChatChannelSourceLocal source, std::string const& text, bool addressed,
                  std::string const& scopeKey, OllamaBridge::Watch const* wake)
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
    BridgeBotStateFacts(bot, speaker, input);
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
