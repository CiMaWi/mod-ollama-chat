// The dispatcher and the bridge's world adapter, compiled whole: this file includes the two production .cpp files
// after its own doubles of the game, playerbots and the module's other units (tests/doubles names the game headers).
// The bridge, world-policy, channel, query and listener units are linked. No live-game proof.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <list>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include "SharedDefines.h"
#include "mod-ollama-chat_bridge_query.h"
#include "mod-ollama-chat_bridge_world.h"
#include "mod-ollama-chat_reply_split.h"

namespace
{
unsigned checks = 0;
unsigned cleanupCalls = 0;
unsigned filterCalls = 0;
unsigned routeCalls = 0;
unsigned governorCalls = 0;
unsigned recordCalls = 0;
unsigned observeCalls = 0;
unsigned revokeOnObserve = 0;
unsigned changeScopeOnObserve = 0;
unsigned scopeChange = 0;
void ChangeScope();
bool filterEmpty = false;
bool cleanupEmpty = false;
OllamaBridge::DispatchGuard gameGuard;
std::atomic<unsigned> apiCalls{0};
std::mutex apiMutex;
std::condition_variable apiCv;
bool apiBlocked = false;
bool apiEntered = false;
bool throwApi = false;
std::string routedText;
std::vector<std::string> routed;  // LC10: every routed message, in order.
unsigned routeFailAt = 0;         // LC10: the route call (1-based) that fails; 0 = none.
uint32_t cleanupMaxLen = 0;       // LC10: the clamp bound the cleanup was asked for.
bool adapterMode = false;
bool secondBot = false;  // players[3] is a second bot of the group, sharing the first bot's AI double.
unsigned findCalls = 0;  // LCB3c: game reads in adapter mode, to count captures.

void Check(bool condition, char const* message)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}

// Where every route of the production RouteMessage ends: one of the bot AI's send calls.
bool Routed(std::string const& text)
{
    if (++routeCalls == routeFailAt)
        return false;
    routedText = text;
    routed.push_back(text);
    return true;
}
}  // namespace

// --- game doubles (tests/doubles/game-doubles.h) ---------------------------
struct ObjectGuid
{
    uint64_t value;
    explicit ObjectGuid(uint64_t guid) : value(guid) {}
    uint64_t GetRawValue() const { return value; }
    uint32_t GetCounter() const { return static_cast<uint32_t>(value); }
    bool operator==(ObjectGuid const&) const = default;
};
struct Group;
struct Player;
struct Guild
{
};
struct Unit
{
    virtual ~Unit() = default;
    uint64_t guid = 11;
    bool inWorld = true;
    bool visible = true;
    uint32_t map = 1;
    uint32_t instance = 1;
    uint32_t phase = 1;
    float distance = 0;
    float health = 75;
    std::string name = "Bot";
    bool IsInWorld() const { return inWorld; }
    ObjectGuid GetGUID() const { return ObjectGuid(guid); }
    uint32_t GetMapId() const { return map; }
    uint32_t GetInstanceId() const { return instance; }
    uint32_t GetPhaseMask() const { return phase; }
    std::string GetName() const { return name; }
    float GetHealthPct() const { return health; }
    virtual Player* ToPlayer() { return nullptr; }
    bool IsInMap(Unit* p) const { return inWorld && p->inWorld && map == p->map && instance == p->instance; }
    bool InSamePhase(Unit* p) const { return (phase & p->phase) != 0; }
    bool IsWithinDistInMap(Unit* p, float d) const { return IsInMap(p) && InSamePhase(p) && p->distance <= d; }
    bool CanSeeOrDetect(Unit* p, bool ignoreStealth, bool) const { return ignoreStealth || (visible && p->visible); }
};
struct Player : Unit
{
    bool online = true;
    bool human = false;
    uint32_t team = 0;
    Group* group = nullptr;
    Group* GetGroup() const { return group; }
    uint32_t GetTeamId() const { return team; }
    Player* ToPlayer() override { return this; }
    uint8_t getRace() const { return RACE_HUMAN; }
    uint8_t getClass() const { return CLASS_WARRIOR; }
    uint8_t getGender() const { return GENDER_MALE; }
    uint8_t GetLevel() const { return 10; }
    uint32_t GetZoneId() const { return 12; }
    bool InBattleground() const { return false; }
    bool InArena() const { return false; }
    Guild* GetGuild() const { return nullptr; }
    uint32_t GetGuildId() const { return 0; }
};
struct Group
{
    struct Slot
    {
        ObjectGuid guid;
    };
    std::list<Slot> members{{ObjectGuid(11)}, {ObjectGuid(22)}, {ObjectGuid(33)}};
    uint64_t guid = 99;
    uint64_t leader = 22;
    bool raid = false;
    bool isRaidGroup() const { return raid; }
    auto const& GetMemberSlots() const { return members; }
    ObjectGuid GetGUID() const { return ObjectGuid(guid); }
    ObjectGuid GetLeaderGUID() const { return ObjectGuid(leader); }
    std::array<uint64_t, 8> marks{};
    ObjectGuid GetTargetIcon(uint8_t icon) const { return ObjectGuid(marks.at(icon)); }
    bool IsMember(ObjectGuid id) const
    {
        return std::any_of(members.begin(), members.end(), [&](Slot const& slot) { return slot.guid == id; });
    }
};
struct PlayerbotAI
{
    Player* master = nullptr;
    bool isBot = true;
    Player* GetMaster() { return master; }
    bool IsBotAI() const { return isBot; }
    bool Say(std::string const& text) { return Routed(text); }
    bool Yell(std::string const& text) { return Routed(text); }
    bool Whisper(std::string const& text, std::string const&) { return Routed(text); }
    bool SayToParty(std::string const& text) { return Routed(text); }
    bool SayToRaid(std::string const& text) { return Routed(text); }
    bool SayToGuild(std::string const& text) { return Routed(text); }
};
// No test routes to a channel: the bridge takes whisper, say, yell and party lines only.
struct Channel
{
    bool IsOn(ObjectGuid) const { return false; }
    void Say(ObjectGuid, std::string const&, uint32_t) {}
};
struct ChannelMgr
{
    static ChannelMgr* forTeam(uint32_t) { return nullptr; }
    Channel* GetChannel(std::string const&, Player*) { return nullptr; }
};
namespace
{
Player players[70];
Group party;
PlayerbotAI gameAI;
}  // namespace
struct ObjectAccessor
{
    static Player* FindConnectedPlayer(ObjectGuid guid)
    {
        if (adapterMode)
        {
            ++findCalls;
            for (auto& player : players)
                if (player.guid == guid.value && player.online)
                    return &player;
            return nullptr;
        }
        static Player player;
        player.group = &party;  // A party line's route asks for the bot's group.
        return &player;
    }
    static Unit* GetUnit(Unit const&, ObjectGuid guid) { return FindConnectedPlayer(guid); }
};
struct PlayerbotsMgr
{
    static PlayerbotsMgr& instance()
    {
        static PlayerbotsMgr manager;
        return manager;
    }
    PlayerbotAI* GetPlayerbotAI(Player* player)
    {
        if (adapterMode)
            return (player == &players[0] || (secondBot && player == &players[3])) ? &gameAI : nullptr;
        static PlayerbotAI ai;
        return &ai;
    }
};
// The grid search of a say or yell route: every online player of the fixture stands in range.
namespace Acore
{
struct AnyPlayerInObjectRangeCheck
{
    AnyPlayerInObjectRangeCheck(Player*, float, bool, bool) {}
};
template <typename Check>
struct PlayerListSearcher
{
    std::list<Player*>& found;
    PlayerListSearcher(Player*, std::list<Player*>& list, Check&) : found(list) {}
};
namespace Crypto
{
template <std::size_t Size>
std::array<uint8_t, Size> GetRandomBytes()
{
    return {};
}
}  // namespace Crypto
}  // namespace Acore
struct Cell
{
    template <typename Searcher>
    static void VisitObjects(Player*, Searcher& searcher, float)
    {
        for (auto& player : players)
            if (player.online)
                searcher.found.push_back(&player);
    }
};
#define OLLAMA_SELFTEST_GAME_DOUBLES

// --- the production units ---------------------------------------------------
#include "../src/mod-ollama-chat_bridge_adapter.cpp"

// LC20b: the store's world is game code (mod-ollama-chat_bridge_store.cpp); these checks send no store packet.
OllamaBridge::StoreWorld& OllamaChat_BridgeStoreWorld()
{
    struct None final : OllamaBridge::StoreWorld
    {
        bool Known(uint64_t) override { return false; }
        uint32_t SharedId(uint64_t, uint8_t) override { return 0; }
        bool InEvent(uint64_t, uint32_t) override { return false; }
        PastRules::Refusal Apply(OllamaBridge::StoreOp, uint32_t, OllamaBridge::StoreRow const&) override
        {
            return PastRules::Refusal::Missing;
        }
    };
    static None none;
    return none;
}
#include "../src/mod-ollama-chat_dispatch.cpp"
#include "bridge-ability-double.h"

// --- doubles of the module's other units ------------------------------------
bool g_DebugEnabled = false;
bool g_EnableTypingSimulation = false;
uint32_t g_TypingSimulationBaseDelay = 100;
uint32_t g_TypingSimulationDelayPerChar = 10;
uint32_t g_TypingSimulationMaxDelay = 1000;
uint32_t g_MaxQueueDepth = 64;
uint32_t g_DispatchWorkerThreads = 1;
uint32_t g_MaxReplyLength = 240;  // The legacy clamp; the bridge path never passes it.
bool g_Enable = true;
bool g_EnableWhisperReplies = true;
bool g_DisableForSayYell = false;
bool g_DisableForParty = false;
bool g_DisableForGuild = false;
float g_SayDistance = 25.0f;
float g_YellDistance = 100.0f;
const char* ChatChannelSourceLocalStr[] = {"Undefined", "Say", "Party", "Raid", "Guild", "Officer", "Yell", "Whisper"};

namespace
{
OllamaEndpointSettings config;
OllamaEndpointSettings observedSettings;
OllamaApiResult apiResult;
}  // namespace

OllamaEndpointSettings OllamaConfig_Snapshot() { return config; }
OllamaApiResult QueryOllamaBridge(OllamaEndpointSettings const& settings, OllamaBridge::Request const&)
{
    ++apiCalls;
    observedSettings = settings;
    std::unique_lock lock(apiMutex);
    apiEntered = true;
    apiCv.notify_all();
    if (!apiCv.wait_for(lock, std::chrono::seconds(3), [] { return !apiBlocked; }))
        throw std::runtime_error("API fixture deadline");
    if (throwApi)
        throw std::runtime_error("synthetic failure");
    return apiResult;
}
std::string ProcessLlmResponse(std::string const& text, std::string const&, uint32_t*, uint32_t maxLen)
{
    ++cleanupCalls;
    cleanupMaxLen = maxLen;
    return cleanupEmpty ? "" : text;
}
std::string Roleplay_FilterMetaTerms(std::string const& text)
{
    ++filterCalls;
    return filterEmpty ? "" : text;
}
bool OllamaIsBotPlayer(Player* player) { return !player->human; }
bool OllamaGroupHasRealPlayer(Player*) { return true; }
void OllamaWorldSnapshot::Build() {}
bool OllamaWorldSnapshot::RealPlayerInChannel(Channel*) const { return true; }
bool Governor_IsRepetitive(ObjectGuid, std::string const&, std::string const&) { return false; }
bool Governor_TryConsumeSend(ObjectGuid, std::string const&, bool)
{
    ++governorCalls;
    return true;
}
void Governor_RecordUtterance(ObjectGuid, std::string const&, std::string const&) { ++recordCalls; }
// J6e-c: the wake's governor scope is the partner's, keyed by the governor.
std::string Governor_MakeScopeKey(char const*, uint32_t, std::string const&, uint32_t, uint32_t)
{
    return "wake-scope";
}

// The typed effect adapter (bridge_effect.cpp) needs playerbots' executor. This double is the closed release gate
// (every command refused without a game read) unless a DS.14 silent-answer test opens it: then it asks the channel's
// authorization once and reports the chosen result only when granted. No lasting plan lives here.
OllamaBridge::EffectResult executorResult = OllamaBridge::EffectResult::Rejected;
OllamaBridge::CommandExecutor& OllamaChat_BridgeCommandExecutor()
{
    struct Closed final : OllamaBridge::CommandExecutor
    {
        std::vector<OllamaBridge::ChannelStepResult> ran;  // LC3b: every step applied, or the one failed step.
        std::vector<OllamaBridge::ChannelStepResult> Steps() const override { return ran; }
        OllamaBridge::EffectResult Execute(OllamaBridge::EffectPhase const&, OllamaBridge::WorldTicket const&,
                                           OllamaBridge::DispatchGuard const&,
                                           OllamaBridge::ChannelCommand const& command,
                                           std::function<bool()> const& authorize) override
        {
            ran.clear();
            if (executorResult == OllamaBridge::EffectResult::Rejected || !authorize())
                return OllamaBridge::EffectResult::Rejected;
            if (executorResult == OllamaBridge::EffectResult::Applied)
                ran.assign(command.steps.size(), {OllamaBridge::ChannelOutcome::Applied, "", ""});
            else if (executorResult == OllamaBridge::EffectResult::Failed)
                ran.assign(1, {OllamaBridge::ChannelOutcome::Failed, "", ""});
            return executorResult;
        }
    };
    static Closed executor;
    return executor;
}

// --- doubles of playerbots' readers -----------------------------------------
namespace
{
// J5f: each state read is 10 points less health, the rest stays fixed; the marker zone shows whether state leaked
// into a capture or a prompt.
unsigned stateFactsCalls = 0;
// LC10: the facts and traits carry markers that show a leak. LCB3c: a nonzero factsTick changes the facts and traits
// of every request.
unsigned factsCalls = 0;
unsigned factsTick = 0;
}  // namespace

namespace PlayerbotBridge
{
bool ReadBotState(ObjectGuid const&, ObjectGuid const&, BotStateFacts& facts)
{
    ++stateFactsCalls;
    facts.healthPct = 100.0f - 10.0f * static_cast<float>(stateFactsCalls);
    facts.alive = true;
    facts.zone = "MARKERZONE";
    return true;
}
bool ReadBotFacts(ObjectGuid const&, BotFacts& facts)
{
    ++factsCalls;
    facts.phrase = "MARKERFACT";
    if (factsTick)
        facts.phrase += std::to_string(factsTick);
    return true;
}
bool ReadBotPersonality(ObjectGuid const&, BotPersonality& out)
{
    out.words = {"MARKERTRAIT"};
    if (factsTick)
        out.words.push_back("trait" + std::to_string(factsTick));
    return true;
}
// J5f-b: the topic reader is not exercised here.
bool ReadBotStateTopic(ObjectGuid const&, BotStateTopic, std::string const&, std::vector<BotStateItem>& items)
{
    items.clear();
    return false;
}
// LC3: the tests install bridge-ability-double.h's index; the adapter's own installer is not run.
std::span<AbilityRow const> AbilityRows() { return {}; }
std::vector<AbilityData> Catalogue() { return {}; }
uint64_t CatalogueHash(std::vector<AbilityData> const&) { return 0; }
char const* KindWord(AbilityKind) { return ""; }
char const* KindWord(ParamKind) { return ""; }
char const* CompletionWord(Completion) { return ""; }
PostMapJoin::PostMapJoin() : _thread(std::this_thread::get_id()) {}
}  // namespace PlayerbotBridge

// The post-map-join token's one construction site is this class's world update; here it runs one dispatcher tick.
class OllamaBotRandomChatter
{
public:
    static void Tick()
    {
        PlayerbotBridge::PostMapJoin const join;
        OllamaDispatch_Update(0, join);
    }
};

namespace
{
bool Observe(OllamaBridge::WorldTicket const&, OllamaBridge::DispatchGuard& guard)
{
    guard = gameGuard;
    ++observeCalls;
    if (revokeOnObserve && observeCalls >= revokeOnObserve)
        guard.authority = false;
    if (changeScopeOnObserve && observeCalls == changeScopeOnObserve)
        ChangeScope();
    return true;
}
}  // namespace

namespace
{
void ChangeScope()
{
    auto now = OllamaBridge::Now();
    if (scopeChange < 2)
        g_bridgeWorld.InvalidateScope(gameGuard.scope.botGuid, gameGuard.scope.contextId);
    else if (scopeChange < 4)
    {
        // Expire only the heartbeat, without an Expire sweep or a request deadline change.
        if (scopeChange == 2)
            now.unixMs -= OllamaBridge::WorldLivenessMs + 1;
        else
            now.steadyMs -= OllamaBridge::WorldLivenessMs + 1;
        Check(g_bridgeWorld.ObserveScope(gameGuard.scope, now), "backdated heartbeat installed");
    }
    else
    {
        now.unixMs += OllamaBridge::WorldLivenessMs + 1;
        now.steadyMs += OllamaBridge::WorldLivenessMs + 1;
        g_bridgeWorld.Expire(now);
    }
    if (scopeChange == 1 || scopeChange == 4)
        Check(g_bridgeWorld.ObserveScope(gameGuard.scope, now), "same-value scope reobserved");
}

void Reset()
{
    adapterMode = false;
    OllamaChat_BridgeReset();
    g_queue.clear();
    g_done.clear();
    Check(OllamaBridge::DispatchSlot::Used() == 0, "all physical slots disposed");
    config = {};
    config.bridge = {"fixture", "codex", "http://127.0.0.1:47800", "synthetic-game-link-bearer-only"};
    // LCB6b: the channel runs whenever the bridge does, so the fixture opens it as OllamaDispatch_BridgeConfigured
    // does. ChannelPolicy never rebinds its previous epoch, so every fixture selects its own.
    static unsigned fixtures = 0;
    std::string const epoch = "epoch-r" + std::to_string(++fixtures);
    g_bridgeWorld.Configured(true, config.bridge, epoch);
    Check(g_bridgeWorld.ObserveDrain(true), "selected");
    g_bridgeChannel.Configure(true);
    Check(g_bridgeChannel.Activate(), "channel open");
    g_bridgeGeneration = g_bridgeWorld.Generation();
    gameGuard = {};
    gameGuard.scope = {1, "fixture", "codex", epoch, 11, 22, "conversation", "party", "visible", "c1", "m1"};
    gameGuard.source = OllamaBridge::DispatchSource::Party;
    gameGuard.speakerGuid = 33;
    gameGuard.groupGuid = 99;
    gameGuard.phaseMask = 1;
    gameGuard.audience = {11, 22, 33};
    gameGuard.humanSpeaker = true;
    gameGuard.authority = true;
    gameGuard.perceptible = true;
    gameGuard.audienceOnline = true;
    gameGuard.nonRaid = true;
    Check(g_bridgeWorld.ObserveScope(gameGuard.scope, OllamaBridge::Now()), "scope observed");
    g_bridgeObserver = Observe;
    g_running = true;
    g_MaxQueueDepth = 64;
    g_EnableTypingSimulation = false;
    apiResult = {};
    apiResult.ok = true;
    apiResult.text = "Ready when you are.";
    apiCalls = 0;
    apiBlocked = false;
    apiEntered = false;
    throwApi = false;
    filterEmpty = cleanupEmpty = false;
    cleanupCalls = filterCalls = routeCalls = governorCalls = recordCalls = observeCalls = revokeOnObserve = 0;
    changeScopeOnObserve = scopeChange = 0;
    routedText.clear();
    routed.clear();
    routeFailAt = 0;
    cleanupMaxLen = 999;
}

bool Submit(bool addressed = true, std::string memory = {}, int64_t lifetimeMs = 1000)
{
    OllamaChatRequest request;
    request.botGuid = gameGuard.scope.botGuid;  // 11 except in the DS.14 silent tests (one bot per command).
    request.targetGuid = 33;
    request.botName = "Bot";
    request.source = SRC_PARTY_LOCAL;
    request.directAddress = addressed;
    request.scopeKey = "party-99";
    OllamaBridge::Context input;
    input.speakerGuid = 33;
    input.channel = "party";
    input.text = "Are you ready?";
    input.directAddress = addressed;
    input.memory = std::move(memory);
    return OllamaDispatch_SubmitBridge(std::move(request), gameGuard.scope, std::move(input), gameGuard, lifetimeMs);
}

Completion Finish()
{
    Task task = std::move(g_queue.front());
    g_queue.pop_front();
    RunBridgeTask(task);
    Completion done = std::move(g_done.front());
    g_done.pop_front();
    return done;
}

void GuardTests()
{
    Reset();
    Check(Submit(), "separate speaker and controller admitted");
    auto const& ticket = g_queue.front().request.bridgeTicket;
    auto const captured = gameGuard;
    auto const now = OllamaBridge::Now();
    Check(OllamaBridge::DispatchGuardCurrent(ticket, captured, gameGuard, now), "exact current guard");
    auto reject = [&](OllamaBridge::DispatchGuard changed, char const* label)
    { Check(!OllamaBridge::DispatchGuardCurrent(ticket, captured, changed, now), label); };
    auto changed = captured;
    changed.authority = false;
    reject(changed, "authority lost");
    changed = captured;
    changed.scope.controllerGuid = 44;
    reject(changed, "controller changed");
    changed = captured;
    changed.groupGuid = 100;
    reject(changed, "different exact group");
    changed = captured;
    changed.nonRaid = false;
    reject(changed, "raid forbidden");
    changed = captured;
    changed.perceptible = false;
    reject(changed, "stealth/hearing loss");
    changed = captured;
    changed.audienceOnline = false;
    reject(changed, "audience logout");
    changed = captured;
    changed.audience.push_back(44);
    reject(changed, "audience addition");
    changed = captured;
    changed.instanceId = 2;
    reject(changed, "instance changed");
    changed = captured;
    changed.mapId = 2;
    reject(changed, "map changed");
    changed = captured;
    changed.phaseMask = 2;
    reject(changed, "phase changed");
    changed = captured;
    changed.scope.membershipRevision = "m2";
    reject(changed, "membership revision changed");
    changed = captured;
    changed.humanSpeaker = false;
    reject(changed, "bot-origin forbidden");
    Check(
        !OllamaBridge::DispatchGuardCurrent(ticket, captured, captured, {ticket.request.deadline.unixMs, now.steadyMs}),
        "UTC deadline");
    Check(
        !OllamaBridge::DispatchGuardCurrent(ticket, captured, captured, {now.unixMs, ticket.request.deadline.steadyMs}),
        "steady deadline");
}

void AdmissionTests()
{
    Reset();
    g_bridgeObserver = nullptr;
    Check(!Submit() && !apiCalls && !OllamaBridge::DispatchSlot::Used(), "unbound fails closed");
    g_bridgeObserver = Observe;
    Check(!Submit(true, std::string(8193, 'x')), "actual builder rejects oversize context");
    Check(Submit(), "bound request admitted");
    auto const original = config;
    config.bridge.token = "synthetic-game-link-bearer-rotated";
    auto done = Finish();
    Check(apiCalls == 1 && observedSettings.bridge.token == original.bridge.token,
          "worker uses immutable full snapshot");
    Check(!cleanupCalls && !filterCalls, "worker skips world cleanup/filter");
    g_bridgeWorld.Configured(true, config.bridge, "epoch-2");
    g_bridgeGeneration = 0;
    Check(!PrepareBridge(done) && !cleanupCalls && !routeCalls, "token reload rejects before cleanup");
}

void CapacityTests()
{
    // LC.38: no fixed count of reservations or slots; the generation queue's depth is the bound, and a full queue
    // stages its bounded terminal (the outage line) as before.
    Reset();
    g_MaxQueueDepth = 200;
    for (unsigned i = 0; i < 200; ++i)
        Check(Submit(), "LC.38: 200 generations admitted, no slot cap");
    Check(g_queue.size() == 200 && g_done.empty() && OllamaBridge::DispatchSlot::Used() == 200,
          "every queued task holds its physical slot");
    auto now = OllamaBridge::Now();
    now.unixMs += 1001;
    now.steadyMs += 1001;
    g_bridgeWorld.Expire(now);
    Check(!g_bridgeWorld.Pending() && OllamaBridge::DispatchSlot::Used() == 200, "expiry retains physical queue slots");
    g_queue.pop_front();
    Check(OllamaBridge::DispatchSlot::Used() == 199, "actual disposal releases exactly one slot");
    g_queue.clear();
    Check(!OllamaBridge::DispatchSlot::Used(), "queue disposal frees slots");
    Reset();
    g_MaxQueueDepth = 1;
    Check(Submit() && Submit(), "queue full stages bounded terminal");
    Check(g_queue.size() == 1 && g_done.size() == 1 && OllamaBridge::DispatchSlot::Used() == 2, "staging counted");
    auto done = std::move(g_done.front());
    g_done.pop_front();
    Check(PrepareBridge(done), "queue-full completes Reserved ticket");
    DeliverBridge(done, {});
    DeliverBridge(done, {});
    Check(routeCalls == 1 && routedText == OllamaBridge::DispatchOutageText() && apiCalls == 0,
          "queue-full outage once, no provider retry");
}

void PublicationTest()
{
    Reset();
    Check(Submit() && Submit(), "two generations queued");
    Task task = std::move(g_queue.front());
    g_queue.pop_front();
    RunBridgeTask(task);
    Check(OllamaBridge::DispatchSlot::Used() == 2 && g_done.size() == 1, "publication keeps its slot owned");
    Check(!task.bridgeSlot && !task.request.bridgeGeneration && task.request.bridgeSettings.bridge.token.empty() &&
              task.request.bridgeTicket.request.input.text.empty() && task.request.bridgeGuard.audience.empty(),
          "published completion leaves no worker request payload");
    g_bridgeWorld.Cancel(g_done.front().request.bridgeTicket);
    g_done.clear();
    Check(OllamaBridge::DispatchSlot::Used() == 1, "completion disposal releases exactly its slot");
    Check(!task.request.bridgeGeneration && task.request.bridgeTicket.request.input.text.empty(),
          "old worker remains alive but empty after replacement admission");
}

void ClaimedScopeTests()
{
    using OllamaBridge::WorldOutcome;
    for (auto outcome : {WorldOutcome::Success, WorldOutcome::Unavailable})
    {
        for (unsigned change = 0; change < 5; ++change)
        {
            for (unsigned boundary = 2; boundary <= 4; ++boundary)
            {
                Reset();
                Check(Submit(true, {}, 20000), "long-lived request for short scope heartbeat");
                auto done = Finish();
                done.bridgeOutcome = outcome;
                Check(PrepareBridge(done), "scope transition prepared");
                auto later = OllamaBridge::Now();
                later.unixMs += OllamaBridge::WorldLivenessMs + 1;
                later.steadyMs += OllamaBridge::WorldLivenessMs + 1;
                Check(OllamaBridge::DispatchGuardCurrent(done.request.bridgeTicket, gameGuard, gameGuard, later),
                      "equal game values and request deadline alone still authorize");
                observeCalls = 0;
                changeScopeOnObserve = boundary;
                scopeChange = change;
                DeliverBridge(done, {});
                Check(
                    governorCalls == (boundary > 2 ? 1u : 0u) && routeCalls == (boundary > 3 ? 1u : 0u) && !recordCalls,
                    "scope loss blocks the next effect after terminal claim");
                Check(!g_bridgeWorld.Pending(), "terminal exit always releases policy claim");
                Check(g_bridgeWorld.ObserveScope(gameGuard.scope, OllamaBridge::Now()), "refresh after terminal exit");
                DeliverBridge(done, {});
                Check(
                    governorCalls == (boundary > 2 ? 1u : 0u) && routeCalls == (boundary > 3 ? 1u : 0u) && !recordCalls,
                    "same-value scope cannot replay disposed claim");
            }
        }
    }
    Reset();
    Check(Submit(), "one-shot claim admitted");
    auto done = Finish();
    Check(PrepareBridge(done), "one-shot claim prepared");
    auto const& ticket = done.request.bridgeTicket;
    auto const now = OllamaBridge::Now();
    Check(g_bridgeWorld.Consume(ticket, now) && g_bridgeWorld.Current(ticket, now), "claim retains authorization");
    Check(!g_bridgeWorld.Consume(ticket, now) && !g_bridgeWorld.Complete(ticket, WorldOutcome::Success, now),
          "claim cannot be consumed or completed again");
    DeliverBridge(done, {});
    Check(!governorCalls && !routeCalls, "reentrant delivery cannot reuse terminal claim");
    g_bridgeWorld.Cancel(ticket);
    Check(!g_bridgeWorld.Current(ticket, now) && !g_bridgeWorld.Pending(), "claim disposal ends authorization");
}

void TerminalTests()
{
    using OllamaBridge::WorldOutcome;
    for (auto outcome : {WorldOutcome::Unavailable, WorldOutcome::Timeout, WorldOutcome::Malformed, WorldOutcome::Empty,
                         WorldOutcome::Filtered, WorldOutcome::QueueFull})
    {
        Reset();
        Check(Submit(), "fallback admission");
        auto done = Finish();
        done.bridgeOutcome = outcome;
        Check(PrepareBridge(done), "eligible fallback prepared");
        DeliverBridge(done, {});
        DeliverBridge(done, {});
        Check(routeCalls == 1 && governorCalls == 1 && recordCalls == 1 &&
                  routedText == OllamaBridge::DispatchOutageText(),
              "fixed one-shot governor-routed outage");
        Check(OllamaBridge::DispatchTerminal(outcome, false) == OllamaBridge::TerminalAction::Silence,
              "unaddressed failures silent");
    }
    Reset();
    Check(Submit(), "success admission");
    auto done = Finish();
    Check(PrepareBridge(done), "success prepared");
    Check(!g_bridgeWorld.Complete(done.request.bridgeTicket, WorldOutcome::Success, OllamaBridge::Now()),
          "duplicate completion rejected");
    DeliverBridge(done, {});
    Check(routeCalls == 1 && routedText == apiResult.text, "success text preserved");
    done = {};
    Reset();
    Check(Submit(), "filtered admission");
    done = Finish();
    filterEmpty = true;
    Check(PrepareBridge(done) && done.text == OllamaBridge::DispatchOutageText(), "world filtering yields outage");
    done = {};
    Reset();
    Check(Submit(), "delayed admission");
    done = Finish();
    g_EnableTypingSimulation = true;
    Check(PrepareBridge(done) && done.deliverAt > Clock::now(), "typing staged without sleep");
    auto later = OllamaBridge::Now();
    later.unixMs += 1001;
    later.steadyMs += 1001;
    g_bridgeWorld.Expire(later);
    Check(OllamaBridge::DispatchSlot::Used() == 1, "delayed expiry retains slot until disposal");
    DeliverBridge(done, {});
    Check(!governorCalls && !routeCalls, "delayed expiry produces no effects");
    done = {};
    Reset();
    Check(Submit(), "guard recheck admission");
    done = Finish();
    Check(PrepareBridge(done), "guard recheck prepared");
    observeCalls = 0;
    revokeOnObserve = 3;
    DeliverBridge(done, {});
    Check(governorCalls == 1 && !routeCalls && !recordCalls, "revalidate again after governor before route");
    done = {};
    Reset();
    Check(Submit(), "cancel admission");
    done = Finish();
    done.bridgeOutcome = WorldOutcome::Cancelled;
    Check(!PrepareBridge(done) && !routeCalls && !cleanupCalls, "cancelled silent without cleanup");
    done = {};
    Reset();
    Check(Submit(), "exception admission");
    throwApi = true;
    done = Finish();
    Check(PrepareBridge(done) && done.text == OllamaBridge::DispatchOutageText() && apiCalls == 1,
          "exception becomes one fixed outage");
}

void DrainTest()
{
    Reset();
    Check(Submit(), "worker drain admission");
    apiBlocked = true;
    std::thread worker(WorkerLoop);
    bool entered;
    {
        std::unique_lock lock(apiMutex);
        entered = apiCv.wait_for(lock, std::chrono::seconds(3), [] { return apiEntered; });
    }
    bool const counted = g_bridgeInFlight == 1 && OllamaBridge::DispatchSlot::Used() == 1;
    config.bridge.token = "synthetic-game-link-bearer-new";
    g_bridgeWorld.Configured(true, config.bridge, "epoch-2");
    g_bridgeGeneration = 0;
    bool const waits = !g_bridgeWorld.ObserveDrain(g_bridgeInFlight == 0);
    {
        std::lock_guard lock(g_queueMutex);
        g_running = false;
    }
    {
        std::lock_guard lock(apiMutex);
        apiBlocked = false;
    }
    apiCv.notify_all();
    g_queueCv.notify_all();
    worker.join();
    Check(entered && counted && waits, "cancellation is not actual worker drain");
    Check(!g_bridgeInFlight && !OllamaBridge::DispatchSlot::Used() && g_done.empty(),
          "stale worker disposes actual payload before drain");
    Check(g_bridgeWorld.ObserveDrain(true), "actual drain permits selection");
}

void AdapterFixture()
{
    Reset();
    g_bridgeWorld.InvalidateScope(gameGuard.scope.botGuid, gameGuard.scope.contextId);
    adapterMode = true;
    secondBot = false;
    party = {};
    for (unsigned i = 0; i < std::size(players); ++i)
    {
        players[i] = {};
        players[i].guid = (i + 1) * 11;
        players[i].human = i != 0;
        players[i].name = "Person" + std::to_string(i);
        players[i].group = &party;
    }
    gameAI = {};
    gameAI.master = &players[1];
    g_Enable = g_EnableWhisperReplies = true;
    g_DisableForParty = g_DisableForSayYell = false;
    g_SayDistance = 25.0f;
    g_YellDistance = 100.0f;
    g_bridgeObserver = OllamaChat_BridgeObserve;
}

bool AdapterSubmit(ChatChannelSourceLocal source = SRC_PARTY_LOCAL, std::string text = "A public question.")
{
    return BridgeSubmit(&players[0], &players[2], source, text, true, "party-99");
}

// DS.14 (b): one typed act on the generation's own route, as the bridge sends it before the terminal result.
void SendAct(OllamaBridge::WorldTicket const& ticket, OllamaBridge::EffectResult result)
{
    executorResult = result;
    nlohmann::json const act = {
        {"version", 1},
        {"instance", ticket.scope.instance},
        {"provider", ticket.scope.provider},
        {"epoch", ticket.scope.epoch},
        {"requestId", ticket.request.requestId},
        {"deadlineUnixMs", ticket.request.deadline.unixMs},
        {"type", "command"},
        {"commandId", "act-" + ticket.request.requestId},  // Ids are once per epoch.
        {"issuerGuid", std::to_string(ticket.scope.controllerGuid)},
        {"botGuid", std::to_string(ticket.scope.botGuid)},
        {"steps", nlohmann::json::array({{{"act", "follow"}, {"params", nlohmann::json::object()}}})}};
    auto const receipt = std::make_shared<OllamaBridge::ChannelAnswer>();
    auto const before = g_bridgeChannel.Commands();
    Check(g_bridgeChannel.Receive(ticket.request.requestId, act.dump(), receipt) == OllamaBridge::ChannelError::None,
          "DS.14: the act was received on the generation route");
    OllamaBotRandomChatter::Tick();  // One world tick takes the act.
    Check(g_bridgeChannel.Commands().proposed == before.proposed + 1, "DS.14: the act was decided");
    std::unique_ptr<OllamaBridge::ChannelPacket> packet;
    char const* const outcome = result == OllamaBridge::EffectResult::Applied  ? "applied"
                                : result == OllamaBridge::EffectResult::Failed ? "failed"
                                                                               : "rejected";
    Check(receipt->Wait(std::chrono::steady_clock::now(), packet) && packet &&
              packet->error == OllamaBridge::ChannelError::None &&
              nlohmann::json::parse(packet->body).at("outcome") == outcome,
          "DS.14: the bridge sees the act's outcome");
    executorResult = OllamaBridge::EffectResult::Rejected;
}

// The channel binds an epoch once (ChannelPolicy never reuses one); Reset keeps the same selected identity.
void OpenChannel()
{
    if (!g_bridgeChannel.Activate())
        g_bridgeChannel.Configure(true);
    Check(g_bridgeChannel.Activate(), "DS.14: game link channel open");
}

void SilentTests()
{
    using namespace OllamaBridge;
    // A silent answer stands only after an authorized attempt (Applied or Failed) on its own route: no line, no
    // outage, no governor send, completed and consumed once, counted. Otherwise it is no answer: Empty, so the outage
    // line for an addressed human and silence otherwise. Each case uses its own bot (the 2 s command interval).
    struct Case
    {
        char const* label;
        std::optional<EffectResult> act;
        bool addressed;
        uint64_t bot;
        bool stands;
    };
    for (auto const& c :
         {Case{"applied", EffectResult::Applied, true, 12, true}, Case{"failed", EffectResult::Failed, false, 13, true},
          Case{"rejected", EffectResult::Rejected, true, 14, false},
          Case{"rejected, unaddressed", EffectResult::Rejected, false, 15, false},
          Case{"no command", std::nullopt, true, 16, false},
          Case{"no command, unaddressed", std::nullopt, false, 17, false}})
    {
        Reset();
        gameGuard.scope.botGuid = c.bot;
        gameGuard.audience = {c.bot, 22, 33};
        Check(g_bridgeWorld.ObserveScope(gameGuard.scope, Now()), "DS.14: silent fixture scope observed");
        OpenChannel();
        Check(Submit(c.addressed), "DS.14: silent fixture admitted");
        auto const ticket = g_queue.front().request.bridgeTicket;
        if (c.act)
            SendAct(ticket, *c.act);
        apiResult = {};
        apiResult.ok = true;
        apiResult.silent = true;
        auto done = Finish();
        Check(done.bridgeOutcome == WorldOutcome::Silent && done.text.empty(), "DS.14: the worker maps silent");
        g_EnableTypingSimulation = true;
        uint64_t const counted = g_bridgeSilent;
        bool const prepared = PrepareBridge(done);
        if (c.stands)
        {
            Check(prepared && done.bridgeOutcome == WorldOutcome::Silent && done.text.empty() &&
                      done.deliverAt <= std::chrono::steady_clock::now(),
                  "DS.14: a silent answer after an act stands, without outage text or typing delay");
            Check(!g_bridgeWorld.Complete(ticket, WorldOutcome::Success, Now()), "DS.14: the silent ticket completed");
            DeliverBridge(done, {});
            DeliverBridge(done, {});
            Check(!routeCalls && !governorCalls && !recordCalls && g_bridgeSilent == counted + 1,
                  "DS.14: no line and no governor send; counted once");
            Check(!g_bridgeWorld.Current(ticket, Now()) && !g_bridgeWorld.Pending(), "DS.14: the claim is disposed");
        }
        else if (c.addressed)
        {
            Check(prepared && done.bridgeOutcome == WorldOutcome::Empty && done.text == DispatchOutageText(),
                  "DS.14: silence without an act is no answer: the outage line for an addressed human");
            DeliverBridge(done, {});
            Check(routeCalls == 1 && governorCalls == 1 && routedText == DispatchOutageText() &&
                      g_bridgeSilent == counted,
                  "DS.14: the outage line is routed and not counted silent");
        }
        else
        {
            Check(!prepared && !routeCalls && !governorCalls && g_bridgeSilent == counted,
                  "DS.14: silence without an act stays silent when not addressed");
            g_bridgeWorld.Cancel(ticket);
        }
        Check(!g_bridgeChannel.Close(ticket), "DS.14: the route was closed by the dispatcher");
    }
    Check(DispatchTerminal(WorldOutcome::Silent, true) == TerminalAction::Silence &&
              DispatchTerminal(WorldOutcome::Silent, false) == TerminalAction::Silence,
          "DS.14: a silent outcome never maps to the outage line");

    // The real adapter remembers the exchange as an answer without words, never as quoted speech.
    AdapterFixture();
    OpenChannel();
    Check(AdapterSubmit(SRC_WHISPER_LOCAL, "Come here."), "DS.14: adapter silent fixture");
    auto const ticket = g_queue.front().request.bridgeTicket;
    SendAct(ticket, EffectResult::Applied);
    apiResult = {};
    apiResult.ok = true;
    apiResult.silent = true;
    auto done = Finish();
    Check(PrepareBridge(done), "DS.14: adapter silent answer prepared");
    DeliverBridge(done, {});
    Check(!routeCalls && !governorCalls && g_bridgeBindings.size() == 1 &&
              g_bridgeBindings[0].memory == "Earlier the same speaker said: Come here.\nYou answered without words.",
          "DS.14: the memory records a wordless answer");
    done = {};
    g_bridgeChannel.Configure(false);
    Reset();
}

void AdapterTests()
{
    using namespace OllamaBridge;
    AdapterFixture();
    Check(AdapterSubmit(), "real adapter admits a party line from a speaker who does not lead the bot");
    auto const& input = g_queue.front().request.bridgeTicket.request.input;
    Check(g_queue.front().request.bridgeTicket.scope.controllerGuid == 33 && input.speakerGuid == 33 &&
              !input.speakerControlsBot && input.memory.empty(),
          "DS.14: the speaker is the partner and does not control the bot");
    Check(input.party.size() == 3 && input.observations.size() == 2 && input.party[0].role.empty() &&
              input.party[0].healthPercent == -1 && !input.party[0].visibleTargetGuid,
          "restricted explicit context excludes legacy prompt, role, victim and private history");
    auto done = Finish();
    Check(PrepareBridge(done), "adapter completion prepared");
    DeliverBridge(done, {});
    Check(g_bridgeBindings[0].memory.find("A public question.") != std::string::npos, "successful exchange remembered");
    done = {};
    Check(AdapterSubmit(), "next same-audience request admitted");
    Check(!g_queue.front().request.bridgeTicket.request.input.memory.empty(), "same-scope memory carried");

    // Each mutation exercises the real adapter through preparation and delivery after a typing delay.
    std::vector<std::function<void()>> losses = {[] { players[2].human = false; },
                                                 [] { players[2].online = false; },
                                                 [] { players[0].inWorld = false; },
                                                 [] { gameAI.isBot = false; },
                                                 [] { party.guid = 100; },
                                                 [] { party.raid = true; },
                                                 [] { party.members.push_back({ObjectGuid(44)}); },
                                                 [] { players[2].group = nullptr; },
                                                 [] { players[1].group = nullptr; },
                                                 [] { players[2].map = 2; },
                                                 [] { players[2].instance = 2; },
                                                 [] { players[2].phase = 2; },
                                                 [] { players[1].visible = false; },
                                                 [] { players[2].visible = false; },
                                                 [] { players[2].distance = 26; },
                                                 []
                                                 {
                                                     players[3].online = false;
                                                     party.members.push_back({ObjectGuid(44)});
                                                 },
                                                 [] { g_DisableForParty = true; },
                                                 [] { g_Enable = false; },
                                                 [] { g_bridgeBindings[0].idleUntil.unixMs = 1; },
                                                 [] { g_bridgeBindings[0].idleUntil.steadyMs = 1; }};
    for (auto const& lose : losses)
    {
        AdapterFixture();
        Check(AdapterSubmit(), "loss fixture admitted");
        done = Finish();
        g_EnableTypingSimulation = true;
        Check(PrepareBridge(done) && done.deliverAt > std::chrono::steady_clock::now(), "loss fixture typing delay");
        lose();
        DeliverBridge(done, {});
        Check(!routeCalls && !governorCalls && !recordCalls && g_bridgeBindings.empty(),
              "world loss invalidates before delivery, including scoped memory");
        done = {};
    }
    // LCB3c: a change that is neither the scope nor the audience keeps the in-flight reply. DS.14: who leads the bot
    // is no part of the capture's identity (a follow that makes the partner the bot's master, any other change of
    // master, another group leader); nor is a listener being a bot, or a phase the partner still shares.
    std::vector<std::function<void()>> const kept = {[] { gameAI.master = &players[3]; },
                                                     [] { gameAI.master = &players[2]; },
                                                     [] { gameAI.master = nullptr; },
                                                     []
                                                     {
                                                         gameAI.master = nullptr;
                                                         party.leader = 33;
                                                     },
                                                     [] { players[1].human = false; },
                                                     [] { players[2].phase = 3; }};
    for (auto const& keep : kept)
    {
        AdapterFixture();
        Check(AdapterSubmit(), "kept binding fixture admitted");
        done = Finish();
        Check(PrepareBridge(done), "kept binding fixture prepared");
        keep();
        DeliverBridge(done, {});
        Check(routeCalls == 1 && g_bridgeBindings.size() == 1,
              "LCB3c: a change outside the scope and audience keeps the in-flight reply");
        done = {};
    }
    // DS.14: the speaker is the bot's conversation partner whoever leads it; speakerControlsBot only describes the
    // master or, for a masterless bot, the group leader.
    AdapterFixture();
    Check(BridgeSubmit(&players[0], &players[1], SRC_PARTY_LOCAL, "A public question.", true, "party-99") &&
              g_queue.front().request.bridgeTicket.scope.controllerGuid == 22 &&
              g_queue.front().request.bridgeTicket.request.input.speakerControlsBot,
          "DS.14: the master speaking is the partner and controls the bot");
    AdapterFixture();
    gameAI.master = nullptr;
    Check(AdapterSubmit() && g_queue.front().request.bridgeTicket.scope.controllerGuid == 33 &&
              !g_queue.front().request.bridgeTicket.request.input.speakerControlsBot,
          "DS.14: a masterless bot's non-leader speaker is the partner without control");
    AdapterFixture();
    gameAI.master = nullptr;
    party.leader = 33;
    Check(AdapterSubmit() && g_queue.front().request.bridgeTicket.request.input.speakerControlsBot,
          "DS.14: the leader of a masterless bot controls it");
    AdapterFixture();
    gameAI.master = &players[3];
    players[3].human = false;
    Check(AdapterSubmit() && g_queue.front().request.bridgeTicket.scope.controllerGuid == 33,
          "DS.14: a bot master does not keep the speaker from talking with the bot");
    AdapterFixture();
    players[2].human = false;
    Check(!AdapterSubmit(SRC_WHISPER_LOCAL), "DS.14: a bot speaker is no conversation partner yet (DS.11 later)");
    // DS.14: a far whisper keeps the bot's real group (group queries, marks, group verbs).
    AdapterFixture();
    gameAI.master = nullptr;
    players[2].visible = false;
    players[2].distance = 1000;
    Check(AdapterSubmit(SRC_WHISPER_LOCAL, "Come here."), "DS.14: a far whisper to an unled bot is admitted");
    {
        auto const& ticket = g_queue.front().request.bridgeTicket;
        auto const& view = g_bridgeBindings.back().view;
        Check(ticket.scope.controllerGuid == 33 && ticket.scope.channel == "direct" && view.guard.groupGuid == 99 &&
                  view.guard.nonRaid && !ticket.request.input.speakerControlsBot &&
                  ticket.request.input.party.size() == 2,
              "DS.14: the far whisper keeps the bot's group");
    }
    done = Finish();
    Check(PrepareBridge(done), "DS.14: far whisper prepared");
    DeliverBridge(done, {});
    Check(routeCalls == 1, "DS.14: far whisper reply delivered");
    done = {};
    // DS.14: an ungrouped bot's whisper has no group; it may still stay on its own.
    AdapterFixture();
    gameAI.master = nullptr;
    players[0].group = nullptr;
    Check(AdapterSubmit(SRC_WHISPER_LOCAL, "Wait here."), "DS.14: a whisper to an ungrouped bot is admitted");
    {
        auto const& ticket = g_queue.front().request.bridgeTicket;
        auto const& view = g_bridgeBindings.back().view;
        Check(ticket.scope.controllerGuid == 33 && !view.guard.groupGuid && !view.guard.nonRaid &&
                  ticket.request.input.party.size() == 2,
              "DS.14: an ungrouped bot's whisper carries no group");
    }
    // DS.14: an addressed say or yell reaches an unled bot within hearing; party still needs the bot's group.
    AdapterFixture();
    gameAI.master = nullptr;
    Check(AdapterSubmit(SRC_SAY_LOCAL), "DS.14: an addressed say to an unled bot within hearing is captured");
    AdapterFixture();
    gameAI.master = nullptr;
    Check(AdapterSubmit(SRC_YELL_LOCAL), "DS.14: an addressed yell to an unled bot within hearing is captured");
    AdapterFixture();
    players[0].group = nullptr;
    Check(AdapterSubmit(SRC_SAY_LOCAL), "DS.14: an ungrouped bot hears an addressed say");
    AdapterFixture();
    players[0].group = nullptr;
    Check(!AdapterSubmit(), "DS.14: party needs the bot's group");
    // DS.14: a kept binding sends the fresh speakerControlsBot (a follow made the partner the bot's master).
    AdapterFixture();
    gameAI.master = nullptr;
    Check(AdapterSubmit(SRC_WHISPER_LOCAL, "Follow me.") &&
              !g_queue.front().request.bridgeTicket.request.input.speakerControlsBot,
          "DS.14: fresh control fixture");
    done = Finish();
    Check(PrepareBridge(done), "DS.14: fresh control prepared");
    DeliverBridge(done, {});
    done = {};
    gameAI.master = &players[2];
    Check(AdapterSubmit(SRC_WHISPER_LOCAL, "Now stay."), "DS.14: the conversation goes on after the follow");
    Check(g_bridgeBindings.size() == 1 && !g_queue.front().request.bridgeTicket.request.input.memory.empty() &&
              g_queue.front().request.bridgeTicket.request.input.speakerControlsBot,
          "DS.14: the kept binding sends the fresh speakerControlsBot");
    AdapterFixture();
    Check(AdapterSubmit(SRC_WHISPER_LOCAL, "Private whisper."), "direct admitted");
    done = Finish();
    Check(PrepareBridge(done), "private prepared");
    DeliverBridge(done, {});
    done = {};
    Check(AdapterSubmit(), "party after whisper admitted");
    Check(g_queue.back().request.bridgeTicket.request.input.memory.empty(), "private memory never crosses to party");
    g_queue.clear();
    Check(BridgeSubmit(&players[0], &players[1], SRC_WHISPER_LOCAL, "Other speaker", true, "scope"),
          "another direct speaker admitted");
    Check(g_queue.front().request.bridgeTicket.request.input.memory.empty(), "memory separated by speaker");
    AdapterFixture();
    Check(!AdapterSubmit(SRC_RAID_LOCAL), "unsupported raid rejected");
    Check(!BridgeSubmit(&players[0], &players[2], SRC_SAY_LOCAL, "ambient", false, "scope"),
          "unaddressed ambient skipped");
    Check(!AdapterSubmit(SRC_PARTY_LOCAL, std::string(4097, 'x')), "text bound fails closed");
    players[2].name.assign(129, 'x');
    Check(!AdapterSubmit(), "name bound fails closed");
    AdapterFixture();
    players[2].distance = 26;
    Check(!AdapterSubmit(SRC_SAY_LOCAL), "say hearing bound");
    Check(AdapterSubmit(SRC_YELL_LOCAL), "yell uses its own hearing radius");
    done = Finish();
    Check(PrepareBridge(done), "yell prepared");
    g_YellDistance = 20;
    DeliverBridge(done, {});
    Check(!routeCalls, "changed hearing invalidates delayed yell");
    done = {};
    // J5e: a whisper reaches the bot from any distance and out of sight; say and party keep the hearing bound and
    // name the drop.
    AdapterFixture();
    players[2].visible = false;
    players[2].distance = 1000;
    Check(AdapterSubmit(SRC_WHISPER_LOCAL, "From afar."), "J5e: an out-of-sight whisper is admitted");
    done = Finish();
    Check(PrepareBridge(done), "J5e: out-of-sight whisper prepared");
    DeliverBridge(done, {});
    Check(routeCalls == 1, "J5e: out-of-sight whisper reply delivered");
    done = {};
    OllamaChat_BridgeRefresh();
    Check(!g_bridgeBindings.empty(), "J5e: the out-of-sight whisper binding survives a refresh");
    for (auto const source : {DispatchSource::Say, DispatchSource::Party})
    {
        OllamaBridge::BindingCapture view;
        char const* why = nullptr;
        Check(!BridgeCapture(players[0].guid, players[2].guid, source, view, &why) && why &&
                  std::string(why) == "out_of_range",
              "J5e: an out-of-sight say or party line is dropped as out_of_range");
    }
    Check(!AdapterSubmit(SRC_SAY_LOCAL) && !AdapterSubmit(), "J5e: out-of-sight say and party are refused");
    AdapterFixture();
    Check(AdapterSubmit(), "scope renewal fixture");
    done = Finish();
    auto const old = done.request.bridgeTicket;
    players[2].visible = false;
    g_bridgeBindings[0].heartbeat.steadyMs = 1;  // LCB3c: the next capture is due.
    OllamaChat_BridgeRefresh();
    players[2].visible = true;
    Check(AdapterSubmit(), "fresh eligibility permits new request");
    Check(!(g_queue.front().request.bridgeTicket.scope == old.scope) && !g_bridgeWorld.Current(old, Now()),
          "same values after loss cannot revive old tickets");
    done = {};
    AdapterFixture();
    Check(AdapterSubmit(), "outage memory fixture");
    done = Finish();
    done.bridgeOutcome = WorldOutcome::Unavailable;
    Check(PrepareBridge(done), "outage prepared");
    DeliverBridge(done, {});
    Check(g_bridgeBindings[0].memory.empty(), "outage is not an exchange memory");
    done = {};
    for (unsigned loss = 0; loss < 4; ++loss)
    {
        AdapterFixture();
        Check(AdapterSubmit(SRC_WHISPER_LOCAL, "Private memory marker"), "memory invalidation fixture");
        done = Finish();
        Check(PrepareBridge(done), "memory invalidation prepared");
        DeliverBridge(done, {});
        done = {};
        Check(!g_bridgeBindings[0].memory.empty(), "memory exists before invalidation");
        if (loss == 0)
            players[2].inWorld = false;  // J5e: a whisper ignores distance and sight, not presence
        else if (loss == 1)
            g_bridgeBindings[0].idleUntil.unixMs = 1;
        else if (loss == 2)
            g_bridgeBindings[0].idleUntil.steadyMs = 1;
        else
        {
            g_bridgeWorld.Configured(true, config.bridge, "new-epoch");
            Check(g_bridgeWorld.ObserveDrain(true), "reload selected");
            g_bridgeGeneration = g_bridgeWorld.Generation();
            g_bridgeChannel.Configure(true);  // LCB6b: a reload reopens the channel on the new epoch, as production.
            Check(g_bridgeChannel.Activate(), "reload channel open");
        }
        g_bridgeBindings[0].heartbeat.steadyMs = 1;  // LCB3c: the next capture is due.
        OllamaChat_BridgeRefresh();
        Check(g_bridgeBindings.empty(), "loss or reload erases actual stored memory");
        players[2].inWorld = true;
        Check(AdapterSubmit(SRC_WHISPER_LOCAL), "fresh scope after memory invalidation");
        Check(g_queue.front().request.bridgeTicket.request.input.memory.empty(),
              "fresh scope cannot recover old memory");
    }
    AdapterFixture();
    Check(AdapterSubmit(SRC_WHISPER_LOCAL, std::string(4096, 'x')), "maximum human statement admitted");
    apiResult.text.assign(4096, 'y');
    done = Finish();
    Check(PrepareBridge(done), "maximum exchange prepared");
    DeliverBridge(done, {});
    Check(g_bridgeBindings[0].memory.size() <= 8192 && g_bridgeBindings[0].memory.find('y') == std::string::npos,
          "oversized combined exchange retains bounded human statement only");
    done = {};
    AdapterFixture();
    g_MaxQueueDepth = 0;  // Unbounded here: the bindings are under test, not the queue.
    for (unsigned i = 1; i < std::size(players); ++i)
        Check(BridgeSubmit(&players[0], &players[i], SRC_WHISPER_LOCAL, "Question", true, "scope"),
              "LC.38: every distinct conversation admitted, no fixed count");
    g_queue.clear();
    Check(!DispatchSlot::Used() && g_bridgeBindings.size() == std::size(players) - 1,
          "LC.38: 69 bindings, more than the old 64, held independently of slots");
    for (auto& binding : g_bridgeBindings)
        binding.idleUntil.steadyMs = 1;
    OllamaChat_BridgeRefresh();
    Check(g_bridgeBindings.empty(), "idle bindings and memory retire within bounded lifetime");
    Check(BridgeSubmit(&players[0], &players[65], SRC_WHISPER_LOCAL, "Question", true, "scope"),
          "expired binding capacity reusable");
    Reset();
}

// J5f (DS.14 (d)): the bot state is read on world for every request, outside the capture. Two messages whose
// state differs only in health keep the same binding (BridgeSame, no renewal) and each request carries its own
// reading; a refresh reads no state; no state value reaches the binding.
void StateTests()
{
    AdapterFixture();
    stateFactsCalls = 0;
    factsCalls = 0;
    Check(AdapterSubmit(SRC_PARTY_LOCAL, "Come here."), "state: first message admitted");
    auto const first = g_queue.back().request.bridgeTicket;
    OllamaChat_BridgeRefresh();
    Check(stateFactsCalls == 1 && g_bridgeBindings.size() == 1, "state: a refresh reads no state");
    Check(AdapterSubmit(SRC_PARTY_LOCAL, "Where are you?"), "state: second message admitted");
    auto const second = g_queue.back().request.bridgeTicket;
    Check(stateFactsCalls == 2 && first.request.input.state && second.request.input.state &&
              first.request.input.state->healthPct == 90.0f && second.request.input.state->healthPct == 80.0f,
          "state: each request carries its own fresh reading");
    Check(second.scope == first.scope && g_bridgeBindings.size() == 1,
          "state: a health-only difference stays BridgeSame (no renewal)");
    auto const leaks = [](std::string const& text) { return text.find("MARKERZONE") != std::string::npos; };
    Check(!g_bridgeBindings[0].view.context.state || !leaks(g_bridgeBindings[0].view.context.state->zone),
          "state: no state value in the binding");
    Check(factsCalls == 2 && first.request.input.facts && second.request.input.facts &&
              second.request.input.facts->phrase == "MARKERFACT" && second.request.input.traits.words.size() == 1,
          "facts: each request reads the facts and traits at submit");
    Check(!g_bridgeBindings[0].view.context.facts && g_bridgeBindings[0].view.context.traits.words.empty(),
          "facts: no fact or trait in the binding");
    g_queue.clear();
}

// LCB3c: a binding is its scope and audience. For 100 world ticks the facts and traits of every request, the bot's
// place and phase, its master, its group's leader and members and the partner's distance change while the partner
// and the audience of a whisper stay: one binding, one scope, its memory kept, and the refresh captured only on the
// ticks whose heartbeat had lapsed (every tenth). Another listener on a party line then ends its binding once.
void BindingTests()
{
    AdapterFixture();
    gameAI.master = nullptr;
    Check(AdapterSubmit(SRC_WHISPER_LOCAL, "Remember me."), "binding: whisper admitted");
    auto const firstTraits = g_queue.back().request.bridgeTicket.request.input.traits.words;
    Completion done = Finish();
    Check(PrepareBridge(done), "binding: whisper prepared");
    DeliverBridge(done, {});
    Check(g_bridgeBindings.size() == 1 && !g_bridgeBindings[0].memory.empty(), "binding: the exchange is remembered");
    auto const scope = g_bridgeBindings[0].view.guard.scope;
    std::vector<std::string> lastTraits;
    unsigned captures = 0;
    bool offBeat = false;
    for (unsigned tick = 1; tick <= 100; ++tick)
    {
        factsTick = tick;
        players[0].map = players[2].map = tick;
        players[0].phase = players[2].phase = 1u << (tick % 4);
        players[2].distance = 30.0f * static_cast<float>(tick);
        gameAI.master = tick % 3 == 0 ? &players[2] : tick % 3 == 1 ? &players[1] : nullptr;
        party.leader = tick % 2 ? 22 : 33;
        if (tick == 30)
            party.members.push_back({ObjectGuid(44)});
        if (tick == 60)
            party.members.pop_back();
        bool const due = tick % 10 == 0;
        if (due)
            g_bridgeBindings[0].heartbeat.steadyMs = 1;
        unsigned const before = findCalls;
        OllamaChat_BridgeRefresh();
        if (findCalls != before)
            ++captures;
        offBeat = offBeat || (findCalls != before && !due);
        if (tick % 25 == 0)
        {
            Check(AdapterSubmit(SRC_WHISPER_LOCAL, "Still there?"), "binding: a later line admitted");
            lastTraits = g_queue.back().request.bridgeTicket.request.input.traits.words;
            done = Finish();
            Check(PrepareBridge(done), "binding: a later line prepared");
            DeliverBridge(done, {});
        }
        Check(g_bridgeBindings.size() == 1 && g_bridgeBindings[0].view.guard.scope == scope,
              "binding: the binding survives facts, traits, place, master and group changes");
    }
    factsTick = 0;
    Check(captures == 10 && !offBeat, "binding: the refresh captured once per lapsed heartbeat, never between");
    Check(!g_bridgeBindings[0].memory.empty() && !lastTraits.empty() && lastTraits != firstTraits,
          "binding: the memory survives while each request carried its own traits");
    Check(g_bridgeBindings[0].view.guard.mapId == 100, "binding: a kept binding holds the fresh capture");
    g_queue.clear();
    done = {};

    AdapterFixture();
    Check(AdapterSubmit(), "binding: party line admitted");
    g_queue.clear();
    auto const partyScope = g_bridgeBindings[0].view.guard.scope;
    party.members.push_back({ObjectGuid(44)});  // players[3] joins the party: another listener.
    unsigned erased = 0;
    for (unsigned tick = 1; tick <= 20; ++tick)
    {
        bool const had = !g_bridgeBindings.empty();
        if (had && tick % 10 == 0)
            g_bridgeBindings[0].heartbeat.steadyMs = 1;
        OllamaChat_BridgeRefresh();
        erased += had && g_bridgeBindings.empty();
    }
    Check(erased == 1, "binding: another listener ends the party binding exactly once");
    Check(AdapterSubmit() && !(g_bridgeBindings[0].view.guard.scope == partyScope) &&
              g_bridgeBindings[0].view.guard.audience.size() == 4,
          "binding: the new audience opens a new binding");
    g_queue.clear();
}

// J6e-c (DS.14 (c)): one act on a route as the bridge sends it; the act's result.
nlohmann::json WakeAct(OllamaBridge::WorldTicket const& ticket, std::string const& id)
{
    executorResult = OllamaBridge::EffectResult::Applied;
    nlohmann::json const act = {
        {"version", 1},
        {"instance", ticket.scope.instance},
        {"provider", ticket.scope.provider},
        {"epoch", ticket.scope.epoch},
        {"requestId", ticket.request.requestId},
        {"deadlineUnixMs", ticket.request.deadline.unixMs},
        {"type", "command"},
        {"commandId", id},
        {"issuerGuid", std::to_string(ticket.scope.controllerGuid)},
        {"botGuid", std::to_string(ticket.scope.botGuid)},
        {"steps", nlohmann::json::array({{{"act", "follow"}, {"params", nlohmann::json::object()}}})}};
    auto const receipt = std::make_shared<OllamaBridge::ChannelAnswer>();
    Check(g_bridgeChannel.Receive(ticket.request.requestId, act.dump(), receipt) == OllamaBridge::ChannelError::None,
          "wake: the act was received on the wake route");
    OllamaBotRandomChatter::Tick();  // One world tick takes the act.
    std::unique_ptr<OllamaBridge::ChannelPacket> packet;
    Check(receipt->Wait(std::chrono::steady_clock::now(), packet) && packet &&
              packet->error == OllamaBridge::ChannelError::None,
          "wake: the act's result is published");
    executorResult = OllamaBridge::EffectResult::Rejected;
    return nlohmann::json::parse(packet->body);
}

// J6e-c (DS.14 (c)): a fired watch opens one unaddressed generation through BridgeSubmit on the partner's
// conversation, with fresh state and the chain's depth and origin; acts on it pass the same command path and rate.
void WakeTests()
{
    using namespace OllamaBridge;
    AdapterFixture();
    players[0].guid = 77;  // A bot no earlier test commanded: the 2 s command interval survives resets.
    party.members.front().guid = ObjectGuid(77);
    OpenChannel();
    stateFactsCalls = 0;
    int64_t const before = Now().unixMs;
    Check(AdapterSubmit(SRC_PARTY_LOCAL, "Come to me once this fight is over."), "wake: chat line admitted");
    auto const line = g_queue.back().request.bridgeTicket;
    Check(line.request.input.wakeCondition.empty() && line.request.input.wakeWatchId.empty() &&
              line.request.input.wakeDepth == 0 && line.request.input.originUnixMs >= before &&
              line.request.input.originUnixMs <= Now().unixMs,
          "wake: a chat line starts its chain now, at depth 0");
    OllamaChat_BridgeRemember(line, "I will.");
    std::string const remembered = g_bridgeBindings.size() == 1 ? g_bridgeBindings[0].memory : std::string();
    Check(remembered.find("Come to me once this fight is over.") != std::string::npos,
          "wake: the chat exchange is remembered");

    Watch watch;
    watch.id = "watch-77";
    watch.botGuid = 77;
    watch.partnerGuid = players[2].guid;
    watch.source = DispatchSource::Party;
    watch.channel = line.scope.channel;
    watch.contextId = line.scope.contextId;
    watch.condition = WatchCondition::CombatEnd;
    watch.depth = 2;
    watch.originUnixMs = line.request.input.originUnixMs - 1000;
    std::size_t queued = g_queue.size();
    Check(OllamaChat_BridgeWake(watch) && g_queue.size() == queued + 1, "wake: a fired watch opens one generation");
    auto const wake = g_queue.back().request.bridgeTicket;
    bool const wakeAddressed = g_queue.back().request.directAddress;
    std::string const wakeScopeKey = g_queue.back().request.scopeKey;
    auto const& input = wake.request.input;
    Check(input.text == "Your fight is over." && input.wakeCondition == "combat_end" && input.wakeWatchId == "watch-77",
          "wake: the fixed in-world sentence and the watch's condition and id");
    Check(!input.directAddress && !wakeAddressed, "wake: never addressed, so a failure stays silent");
    Check(input.wakeDepth == 2 && input.originUnixMs == watch.originUnixMs, "wake: the chain's depth and origin");
    Check(stateFactsCalls == 2 && input.state, "wake: the bot's state is read again for the wake");
    Check(input.facts && input.facts->phrase == "MARKERFACT", "wake: the facts are read for the wake too");
    Check(wakeScopeKey == "wake-scope" && wake.scope.botGuid == 77 && wake.scope.controllerGuid == players[2].guid &&
              wake.scope.contextId == line.scope.contextId,
          "wake: the partner's scope key and the same conversation");
    Check(wake.request.deadline.unixMs <= Now().unixMs + 30000, "wake: at most 30 s");

    OllamaChat_BridgeRemember(wake, "On my way.");
    Check(g_bridgeBindings.size() == 1 && g_bridgeBindings[0].memory == remembered,
          "wake: a wake reply is no exchange with the speaker; the earlier one stays remembered");

    Check(WakeAct(wake, "wake-act-1").at("outcome") == "applied",
          "wake: an act on the wake route passes the full command path");
    auto const second = WakeAct(wake, "wake-act-2");
    Check(second.at("outcome") == "rejected" && second.at("reason") == "too_soon",
          "wake: the bot's command interval holds on a wake route");

    auto other = watch;
    other.contextId = "another-conversation";
    queued = g_queue.size();
    Check(!OllamaChat_BridgeWake(other) && g_queue.size() == queued,
          "wake: a watch of another conversation opens nothing");
    players[0].online = false;
    Check(!OllamaChat_BridgeWake(watch) && g_queue.size() == queued, "wake: the bot gone, nothing opens");
    players[0].online = true;
    players[2].online = false;
    Check(!OllamaChat_BridgeWake(watch) && g_queue.size() == queued, "wake: the partner gone, nothing opens");
    players[2].online = true;

    Check(BridgeSubmit(&players[0], &players[2], SRC_SAY_LOCAL, "Bot, wait for me.", true, "say-key"),
          "wake: an addressed say line admitted");
    auto sayWatch = watch;
    sayWatch.id = "watch-say";
    sayWatch.source = DispatchSource::Say;
    sayWatch.contextId = g_queue.back().request.bridgeTicket.scope.contextId;
    sayWatch.condition = WatchCondition::Time;
    queued = g_queue.size();
    Check(OllamaChat_BridgeWake(sayWatch) && g_queue.size() == queued + 1 &&
              g_queue.back().request.source == SRC_SAY_LOCAL && !g_queue.back().request.directAddress &&
              g_queue.back().request.bridgeTicket.request.input.text == "The time you meant to wait has passed.",
          "wake: a say conversation wakes too, unaddressed");

    // A wake failure is silence: no outage line, no governor send.
    while (g_queue.size() > 1)
        g_queue.pop_front();
    auto const sayWake = g_queue.front().request.bridgeTicket;
    apiResult = {};
    apiResult.ok = false;
    apiResult.error = "bridge_unavailable";
    auto done = Finish();
    Check(!PrepareBridge(done) && !routeCalls && !governorCalls, "wake: a failed wake stays silent");
    g_bridgeWorld.Cancel(sayWake);
    done = {};
    g_queue.clear();
    Reset();  // The channel stays open for SilentTests (it binds an epoch once per process).
}
}  // namespace

// LC10: a reply over one chat line is two messages in order, with one governor send, one utterance and the whole
// reply remembered; a failed first message sends no second; one that would need three is refused (the outage line
// for an addressed human, silence otherwise) and counted once.
void SplitTests()
{
    using namespace OllamaBridge;
    std::string const first = std::string(199, 'a') + ".";
    std::string const second = std::string(198, 'b') + "!";
    std::string const reply = first + " " + second;
    Reset();
    apiResult.text = reply;
    Check(Submit(), "LC10: split fixture admitted");
    auto done = Finish();
    g_EnableTypingSimulation = true;
    g_TypingSimulationDelayPerChar = 1;
    g_TypingSimulationMaxDelay = 0;
    uint64_t const splits = g_bridgeSplit, refusals = g_bridgeRefusedLong;
    auto const before = std::chrono::steady_clock::now();
    Check(PrepareBridge(done) && done.text == first && done.secondText == second && !cleanupMaxLen &&
              g_MaxReplyLength == 240,
          "LC10: the bridge path is not clamped; the reply is split at its sentence end");
    Check(done.deliverAt >= before + std::chrono::milliseconds(100 + reply.size()),
          "LC10: the typing delay covers the whole reply");
    g_TypingSimulationDelayPerChar = 10;
    g_TypingSimulationMaxDelay = 1000;
    DeliverBridge(done, {});
    DeliverBridge(done, {});
    Check(routed == std::vector<std::string>{first, second} && governorCalls == 1 && recordCalls == 1 &&
              g_bridgeSplit == splits + 1 && g_bridgeRefusedLong == refusals,
          "LC10: two messages in order, once; one governor send and one utterance");

    for (unsigned failAt : {1u, 2u})
    {
        done = {};
        Reset();
        apiResult.text = reply;
        Check(Submit(), "LC10: route-failure fixture admitted");
        done = Finish();
        Check(PrepareBridge(done) && !done.secondText.empty(), "LC10: route-failure fixture split");
        routeFailAt = failAt;
        uint64_t const counted = g_bridgeSplit;
        DeliverBridge(done, {});
        DeliverBridge(done, {});
        if (failAt == 1)
            Check(routed.empty() && routeCalls == 1 && !recordCalls && g_bridgeSplit == counted,
                  "LC10: a failed first message sends no second and records nothing");
        else
            Check(routed == std::vector<std::string>{first} && routeCalls == 2 && recordCalls == 1 &&
                      g_bridgeSplit == counted,
                  "LC10: a failed second message is not counted as a split; the first stays recorded");
    }

    std::string const three = first + " " + std::string(199, 'c') + ". " + std::string(199, 'd') + ".";
    for (bool addressed : {true, false})
    {
        done = {};
        Reset();
        apiResult.text = three;
        Check(Submit(addressed), "LC10: three-message fixture admitted");
        done = Finish();
        auto const ticket = done.request.bridgeTicket;
        uint64_t const counted = g_bridgeRefusedLong, splitCount = g_bridgeSplit;
        bool const prepared = PrepareBridge(done);
        Check(g_bridgeRefusedLong == counted + 1 && g_bridgeSplit == splitCount,
              "LC10: a reply needing three messages is refused and counted once");
        if (addressed)
        {
            Check(prepared && done.bridgeOutcome == WorldOutcome::Filtered && done.text == DispatchOutageText() &&
                      done.secondText.empty(),
                  "LC10: refused whole; the outage line for an addressed human");
            DeliverBridge(done, {});
            DeliverBridge(done, {});
            Check(routed == std::vector<std::string>{DispatchOutageText()} && governorCalls == 1 &&
                      g_bridgeRefusedLong == counted + 1,
                  "LC10: only the outage line goes out, once");
        }
        else
        {
            Check(!prepared && !routeCalls, "LC10: refused in silence when not addressed");
            g_bridgeWorld.Cancel(ticket);
        }
    }

    // The real adapter remembers what the bot said: both messages, joined by a space.
    done = {};
    AdapterFixture();
    Check(AdapterSubmit(SRC_PARTY_LOCAL, "Tell me about the mill."), "LC10: adapter split fixture");
    apiResult.text = reply;
    done = Finish();
    Check(PrepareBridge(done), "LC10: adapter split prepared");
    DeliverBridge(done, {});
    Check(routed.size() == 2 && g_bridgeBindings.size() == 1 &&
              g_bridgeBindings[0].memory ==
                  "Earlier the same speaker said: Tell me about the mill.\nYou replied: " + reply,
          "LC10: the memory keeps both parts joined by a space");
    done = {};
    Reset();
}

int main()
{
    if (!InstallTestAbilities())  // LC3: the verbs and topics are the installed index's.
    {
        std::cerr << "FAIL: the ability double was refused\n";
        return 1;
    }
    try
    {
        GuardTests();
        AdmissionTests();
        CapacityTests();
        PublicationTest();
        ClaimedScopeTests();
        TerminalTests();
        WakeTests();
        SilentTests();
        SplitTests();
        DrainTest();
        AdapterTests();
        StateTests();
        BindingTests();
        g_queue.clear();
        g_done.clear();
        Check(!OllamaBridge::DispatchSlot::Used(), "final no leaked slots");
        std::cout << checks << " dispatch bridge checks passed (production dispatcher and adapter; game/API doubles)\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
