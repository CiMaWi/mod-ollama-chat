// Actual bridge/world translation units plus exact extracted dispatch helpers (see hub J4b-2a-check runner).
// Handler adapter is extracted verbatim too; game objects/route/governor/cleanup/API are controlled stand-ins.
// No full handler/dispatch TU or live-game proof.
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
#include <stdexcept>
#include <thread>
#include <vector>

#include "mod-ollama-chat_bridge_plan.h"
#include "mod-ollama-chat_bridge_query.h"
#include "mod-ollama-chat_bridge_world.h"
#include "mod-ollama-chat_persona_db.h"
#include "mod-ollama-chat_reply_split.h"

enum class OllamaProvider
{
    Ollama,
    Bridge
};
enum class OllamaRequestKind
{
    ChatReply,
    EventChatter
};
enum ChatChannelSourceLocal
{
    SRC_SAY_LOCAL = 1,
    SRC_PARTY_LOCAL = 2,
    SRC_RAID_LOCAL = 3,
    SRC_YELL_LOCAL = 6,
    SRC_WHISPER_LOCAL = 7
};
using OllamaBridgeObserver = bool (*)(OllamaBridge::WorldTicket const&, OllamaBridge::DispatchGuard&);
#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)

OllamaBridge::QueryWorldSource& OllamaChat_BridgeQuerySource();
// The production phase holds the post-map-join token; the stand-in executor below never reads it.
namespace OllamaBridge
{
struct EffectPhase
{
};
}  // namespace OllamaBridge
// The production channel names the world-only typed effect adapter, which needs game headers. This fixture stands in
// with the closed release gate (every command refused without a game read) unless a DS.14 silent-answer test opens
// it: then it asks the channel's authorization once and reports the chosen result only when granted.
OllamaBridge::EffectResult executorResult = OllamaBridge::EffectResult::Rejected;
OllamaBridge::CommandExecutor& OllamaChat_BridgeCommandExecutor()
{
    struct Closed final : OllamaBridge::CommandExecutor
    {
        OllamaBridge::EffectResult Execute(OllamaBridge::EffectPhase const&, OllamaBridge::WorldTicket const&,
                                           OllamaBridge::DispatchGuard const&, OllamaBridge::ChannelCommand const&,
                                           std::function<bool()> const& authorize) override
        {
            if (executorResult == OllamaBridge::EffectResult::Rejected || !authorize())
                return OllamaBridge::EffectResult::Rejected;
            return executorResult;
        }
    };
    static Closed executor;
    return executor;
}
OllamaPersona::ProposalStatus OllamaChat_BridgeProposeCard(OllamaBridge::WorldTicket const&, uint64_t, uint64_t,
                                                           std::string);

// Seams the bridge translation units name but this lifecycle check never reaches: the observer transport, the persona
// store lifecycle (gate closed here) and the events route of the production channel glue.
OllamaHttpResult OllamaHttp_PostObserver(OllamaBridge::Settings const&, OllamaObserverOperation, std::string const&,
                                         int64_t)
{
    return {};
}
namespace OllamaPersona
{
void ForgetPersona(uint64_t) {}
void RetirePersonas(uint64_t) {}
}  // namespace OllamaPersona
// The refresh's mark-change publication, counted per bot (self-mark guard, DS.10).
std::vector<uint64_t> g_eventBots;
bool OllamaDispatch_ChannelEvents(OllamaBridge::DispatchGuard const& guard, std::vector<uint8_t> const&)
{
    g_eventBots.push_back(guard.scope.botGuid);
    return false;
}

#include "dispatch-types.inc"

OllamaBridge::WorldPolicy& OllamaDispatch_BridgeWorld() { return g_bridgeWorld; }
bool OllamaDispatch_SubmitBridge(OllamaChatRequest, OllamaBridge::WorldScope const&, OllamaBridge::Context,
                                 OllamaBridge::DispatchGuard, int64_t);
bool OllamaChat_BridgeObserve(OllamaBridge::WorldTicket const&, OllamaBridge::DispatchGuard&);
void OllamaChat_BridgeRefresh();
void OllamaChat_BridgeNoteOwnMark(uint64_t botGuid);
void OllamaChat_BridgeReset();
void OllamaChat_BridgeRemember(OllamaBridge::WorldTicket const&, std::string const&);

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
bool g_DebugEnabled = false;
bool g_EnableTypingSimulation = false;
uint32_t g_TypingSimulationBaseDelay = 100;
uint32_t g_TypingSimulationDelayPerChar = 10;
uint32_t g_TypingSimulationMaxDelay = 1000;
uint32_t g_MaxQueueDepth = 64;
OllamaEndpointSettings config;
OllamaEndpointSettings observedSettings;
OllamaApiResult apiResult;
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
uint32_t g_MaxReplyLength = 240;  // The legacy clamp; the bridge path never passes it.
bool adapterMode = false;
bool secondBot = false;  // players[3] is a second bot of the group, sharing the first bot's AI double.
bool g_Enable = true;
bool g_EnableWhisperReplies = true;
bool g_DisableForSayYell = false;
bool g_DisableForParty = false;
float g_SayDistance = 25.0f;
float g_YellDistance = 100.0f;
std::string voice = "A quiet traveler.";

void Check(bool condition, char const* message)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}

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
void Memory_RunCondensation(uint64_t, std::string const&) {}
void Memory_RunRelationshipUpdate(uint64_t, uint64_t, std::string const&, std::string const&) {}

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
};
Player players[70];
Group party;
PlayerbotAI gameAI;
bool OllamaIsRealPlayer(Player* player) { return player && player->human; }
std::string Roleplay_BuildVoicePrompt(Player*) { return voice; }
struct Channel
{
};
struct OllamaWorldSnapshot
{
};
struct ObjectAccessor
{
    static Player* FindConnectedPlayer(ObjectGuid guid)
    {
        if (adapterMode)
        {
            for (auto& player : players)
                if (player.guid == guid.value && player.online)
                    return &player;
            return nullptr;
        }
        static Player player;
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
// The extracted capture reads the persona card through the production store (compiled in). It stays off (voice
// only, no card offered) except in CardTests. J14e: the handler's world-side fact gatherer (outside the extract) is a
// stand-in that fills fixed facts and counts its calls.
unsigned cardFactsCalls = 0;
void BridgeCardFacts(Player*, OllamaBridge::CardFacts& facts)
{
    ++cardFactsCalls;
    facts.race = "Dwarf";
    facts.className = "Warrior";
    facts.level = 42;
}
// J5f: the bot-state reader (playerbots, outside the extract) is a stand-in too. Each call reads 10 points less
// health, the rest stays fixed; the marker zone shows whether state leaked into a signature or a prompt.
unsigned stateFactsCalls = 0;
void BridgeBotStateFacts(Player*, Player*, OllamaBridge::BotState& state)
{
    ++stateFactsCalls;
    state.present = true;
    state.healthPct = 100.0f - 10.0f * static_cast<float>(stateFactsCalls);
    state.alive = true;
    state.zone = "MARKERZONE";
}
// LC10: the facts and traits gatherer (outside the extract) is a stand-in too; its marker shows a leak.
unsigned factsCalls = 0;
void BridgeFactsAndTraits(Player*, OllamaBridge::Context& input)
{
    ++factsCalls;
    input.facts.emplace().phrase = "MARKERFACT";
    input.traits.words = {"MARKERTRAIT"};
}
// J5f-b: the topic reader (playerbots, outside the extract); not exercised here.
bool BridgeBotStateTopic(uint64_t, OllamaBridge::StateTopic, std::string const&,
                         std::vector<OllamaBridge::StateEntry>& entries)
{
    entries.clear();
    return false;
}
// J6e-c: the watch fact reader and the wake's governor scope (outside the extract) are stand-ins.
OllamaBridge::WatchFacts watchFacts;
unsigned watchFactsCalls = 0;
bool BridgeWatchFacts(uint64_t, uint64_t, OllamaBridge::WatchFacts& facts)
{
    ++watchFactsCalls;
    facts = watchFacts;
    return facts.ready;
}
std::string BridgeWakeScopeKey(Player*, ChatChannelSourceLocal) { return "wake-scope"; }
bool Governor_IsRepetitive(ObjectGuid, std::string const&, std::string const&) { return false; }
bool Governor_TryConsumeSend(ObjectGuid, std::string const&, bool)
{
    ++governorCalls;
    return true;
}
void Governor_RecordUtterance(ObjectGuid, std::string const&, std::string const&) { ++recordCalls; }
bool RouteMessage(Player*, PlayerbotAI*, Completion const&, std::string const& text, OllamaWorldSnapshot const&,
                  Channel*&)
{
    if (++routeCalls == routeFailAt)
        return false;
    routedText = text;
    routed.push_back(text);
    return true;
}
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

#include "dispatch-helpers.inc"
#include "handler-bridge.inc"
#include "bridge-ability-double.h"

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
    config.provider = OllamaProvider::Bridge;
    config.bridge = {"fixture", "codex", "http://127.0.0.1:47800", "synthetic-game-link-bearer-only"};
    config.systemPrompt = "owned-system";
    g_bridgeWorld.Configured(true, config.bridge, "epoch-1");
    Check(g_bridgeWorld.ObserveDrain(true, OllamaBridge::RetirementProof::NoLiveCaptures), "selected");
    g_bridgeGeneration = g_bridgeWorld.Generation();
    gameGuard = {};
    gameGuard.scope = {1, "fixture", "codex", "epoch-1", 11, 22, "conversation", "party", "visible", "c1", "m1", "p1"};
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
    request.recordHistory = request.updateSentiment = request.triggerBotReplies = true;
    request.prompt = "FORBIDDEN LEGACY PROMPT";
    request.originMessage = "FORBIDDEN LEGACY HISTORY";
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
    changed.scope.personaRevision = "p2";
    reject(changed, "persona revision changed");
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
    OllamaChatRequest legacy;
    legacy.botGuid = 11;
    legacy.prompt = "legacy";
    Check(!OllamaDispatch_Submit(legacy), "prompt-only Bridge rejected");
    config.provider = OllamaProvider::Ollama;
    Check(OllamaDispatch_Submit(legacy), "legacy admission preserved");
    g_queue.clear();
    legacy.bridgeGeneration = 123;
    Check(!OllamaDispatch_Submit(legacy), "forged bridge tag rejected");
    config.provider = OllamaProvider::Bridge;
    Check(!Submit(true, std::string(8193, 'x')), "actual builder rejects oversize context");
    Check(Submit(), "bound request admitted");
    auto const original = config;
    config.bridge.token = "synthetic-game-link-bearer-rotated";
    config.systemPrompt = "reloaded";
    auto done = Finish();
    Check(apiCalls == 1 && observedSettings.bridge.token == original.bridge.token &&
              observedSettings.systemPrompt == original.systemPrompt,
          "worker uses immutable full snapshot");
    Check(!cleanupCalls && !filterCalls, "worker skips world cleanup/filter");
    Check(done.request.prompt.empty() && done.request.originMessage.empty() && !done.request.recordHistory &&
              !done.request.updateSentiment && !done.request.triggerBotReplies,
          "legacy context and effects excluded");
    g_bridgeWorld.Configured(true, config.bridge, "epoch-2");
    g_bridgeGeneration = 0;
    Check(!PrepareBridge(done) && !cleanupCalls && !routeCalls, "token reload rejects before cleanup");
}

void CapacityTests()
{
    Reset();
    for (unsigned i = 0; i < OllamaBridge::MaxWorldPending; ++i)
        Check(Submit(), "physical slot admitted");
    auto now = OllamaBridge::Now();
    now.unixMs += 1001;
    now.steadyMs += 1001;
    g_bridgeWorld.Expire(now);
    Check(!g_bridgeWorld.Pending() && OllamaBridge::DispatchSlot::Used() == 64, "expiry retains physical queue slots");
    Check(!Submit() && g_done.empty(), "full physical capacity cannot stage fallback");
    g_queue.pop_front();
    Check(OllamaBridge::DispatchSlot::Used() == 63 && Submit(), "actual disposal releases exactly one slot");
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
    for (unsigned i = 0; i < OllamaBridge::MaxWorldPending; ++i)
        Check(Submit(), "fill physical slots before publication");
    Task task = std::move(g_queue.front());
    g_queue.pop_front();
    RunBridgeTask(task);
    Check(OllamaBridge::DispatchSlot::Used() == 64 && g_done.size() == 1, "publication keeps capacity owned");
    Check(!task.bridgeSlot && !task.request.bridgeGeneration && task.request.bridgeSettings.bridge.token.empty() &&
              task.request.bridgeSettings.systemPrompt.empty() &&
              task.request.bridgeTicket.request.input.text.empty() && task.request.bridgeGuard.audience.empty(),
          "published completion leaves no worker request payload");
    Check(!Submit(), "completion still occupies final physical slot");
    g_bridgeWorld.Cancel(g_done.front().request.bridgeTicket);
    g_done.clear();
    Check(OllamaBridge::DispatchSlot::Used() == 63 && Submit(), "completion disposal admits exactly one replacement");
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
    for (auto outcome : {WorldOutcome::Unavailable, WorldOutcome::Timeout, WorldOutcome::CircuitOpen,
                         WorldOutcome::Malformed, WorldOutcome::Empty, WorldOutcome::Filtered, WorldOutcome::QueueFull})
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
    bool const waits =
        !g_bridgeWorld.ObserveDrain(g_bridgeInFlight == 0, OllamaBridge::RetirementProof::NoLiveCaptures);
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
    Check(g_bridgeWorld.ObserveDrain(true, OllamaBridge::RetirementProof::NoLiveCaptures),
          "actual drain permits selection");
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
    voice = "A quiet traveler.";
    g_bridgeObserver = OllamaChat_BridgeObserve;
}

bool AdapterSubmit(ChatChannelSourceLocal source = SRC_PARTY_LOCAL, std::string text = "A public question.")
{
    return BridgeSubmit(&players[0], &players[2], source, text, true, "party-99", 0);
}

// DS.14 (b): one typed act on the generation's own route, as the bridge sends it before the terminal result.
void SendAct(OllamaBridge::WorldTicket const& ticket, OllamaBridge::EffectResult result)
{
    static OllamaBridge::EffectPhase const phase{};
    executorResult = result;
    nlohmann::json const act = {{"version", 1},
                                {"instance", ticket.scope.instance},
                                {"provider", ticket.scope.provider},
                                {"epoch", ticket.scope.epoch},
                                {"requestId", ticket.request.requestId},
                                {"deadlineUnixMs", ticket.request.deadline.unixMs},
                                {"type", "command"},
                                {"commandId", "act-" + ticket.request.requestId},  // Ids are once per epoch.
                                {"issuerGuid", std::to_string(ticket.scope.controllerGuid)},
                                {"botGuid", std::to_string(ticket.scope.botGuid)},
                                {"intent", "attack_own_target"},
                                {"params", nlohmann::json::object()}};
    uint64_t receipt = 0;
    auto const before = g_bridgeChannel.Commands();
    Check(g_bridgeChannel.Receive(config.bridge.token, ticket.request.requestId, act.dump(), receipt) ==
              OllamaBridge::ChannelError::None,
          "DS.14: the act was received on the generation route");
    Check(g_bridgeChannel.Update(&phase) == 1, "DS.14: one world tick takes the act");
    Check(g_bridgeChannel.Commands().proposed == before.proposed + 1, "DS.14: the act was decided");
    std::unique_ptr<OllamaBridge::ChannelPacket> packet;
    char const* const outcome = result == OllamaBridge::EffectResult::Applied  ? "applied"
                                : result == OllamaBridge::EffectResult::Failed ? "failed"
                                                                               : "rejected";
    Check(g_bridgeChannel.Take(config.bridge.token, receipt, packet) == OllamaBridge::ChannelError::None && packet &&
              packet->error == OllamaBridge::ChannelError::None &&
              nlohmann::json::parse(packet->body).at("outcome") == outcome,
          "DS.14: the bridge sees the act's outcome");
    executorResult = OllamaBridge::EffectResult::Rejected;
}

// The channel binds an epoch once (ChannelPolicy never reuses one); Reset keeps the same selected identity.
void OpenChannel()
{
    g_channelEnabled = true;
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
    g_channelEnabled = false;
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
              !input.speakerControlsBot && input.persona == voice && input.memory.empty() &&
              input.systemPrompt.find("Speech grants") == std::string::npos,
          "DS.14: the speaker is the partner and does not control the bot; audited persona only");
    Check(input.systemPrompt != config.systemPrompt && input.party.size() == 3 && input.observations.size() == 2 &&
              input.party[0].role.empty() && input.party[0].healthPercent == -1 && !input.party[0].visibleTargetGuid,
          "restricted explicit context excludes legacy prompt, role, victim and private history");
    auto done = Finish();
    Check(PrepareBridge(done), "adapter completion prepared");
    DeliverBridge(done, {});
    Check(g_bridgeBindings[0].memory.find("A public question.") != std::string::npos, "successful exchange remembered");
    done = {};
    Check(AdapterSubmit(), "next same-audience request admitted");
    Check(!g_queue.front().request.bridgeTicket.request.input.memory.empty(), "same-scope memory carried");

    // Each mutation exercises the real adapter through preparation and delivery after a typing delay.
    std::vector<std::function<void()>> losses = {[]
                                                 {
                                                     gameAI.master = nullptr;
                                                     party.leader = 33;
                                                 },
                                                 [] { players[1].human = false; },
                                                 [] { players[2].human = false; },
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
                                                 [] { players[2].phase = 3; },
                                                 [] { players[1].visible = false; },
                                                 [] { players[2].visible = false; },
                                                 [] { players[2].distance = 26; },
                                                 []
                                                 {
                                                     players[3].online = false;
                                                     party.members.push_back({ObjectGuid(44)});
                                                 },
                                                 [] { voice += " Revised."; },
                                                 [] { players[2].name = "Renamed"; },
                                                 [] { g_DisableForParty = true; },
                                                 [] { g_Enable = false; },
                                                 [] { g_bridgeBindings[0].heartbeat.unixMs = 1; },
                                                 [] { g_bridgeBindings[0].heartbeat.steadyMs = 1; },
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
    // DS.14: who leads the bot is no part of the capture's identity; a follow that makes the partner the bot's
    // master, or any other change of master, keeps the in-flight reply.
    std::vector<std::function<void()>> const leads = {
        [] { gameAI.master = &players[3]; }, [] { gameAI.master = &players[2]; }, [] { gameAI.master = nullptr; }};
    for (auto const& lead : leads)
    {
        AdapterFixture();
        Check(AdapterSubmit(), "lead change fixture admitted");
        done = Finish();
        Check(PrepareBridge(done), "lead change fixture prepared");
        lead();
        DeliverBridge(done, {});
        Check(routeCalls == 1 && g_bridgeBindings.size() == 1, "DS.14: a change of master keeps the in-flight reply");
        done = {};
    }
    // DS.14: the speaker is the bot's conversation partner whoever leads it; speakerControlsBot only describes the
    // master or, for a masterless bot, the group leader.
    AdapterFixture();
    Check(BridgeSubmit(&players[0], &players[1], SRC_PARTY_LOCAL, "A public question.", true, "party-99", 0) &&
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
    party.marks[0] = 44;
    players[2].visible = false;
    players[2].distance = 1000;
    Check(AdapterSubmit(SRC_WHISPER_LOCAL, "Come here."), "DS.14: a far whisper to an unled bot is admitted");
    {
        auto const& ticket = g_queue.front().request.bridgeTicket;
        auto const& view = g_bridgeBindings.back().view;
        Check(ticket.scope.controllerGuid == 33 && ticket.scope.channel == "direct" && view.guard.groupGuid == 99 &&
                  view.guard.nonRaid && view.marks.size() == 1 && view.marks[0].targetGuid == 44 &&
                  !ticket.request.input.speakerControlsBot && ticket.request.input.party.size() == 2,
              "DS.14: the far whisper keeps the bot's group and its visible marks");
        IntentContext facts;
        facts.controllerGuid = ticket.scope.controllerGuid;
        facts.botGuid = ticket.scope.botGuid;
        facts.expectedGroupGuid = facts.controllerGroupGuid = facts.botGroupGuid = view.guard.groupGuid;
        Check(MapIntent({"f", 33, 11, ChannelIntent::Follow, std::monostate{}}, facts).has_value(),
              "DS.14: the far partner in the bot's group may ask it to follow");
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
        Check(ticket.scope.controllerGuid == 33 && !view.guard.groupGuid && !view.guard.nonRaid && view.marks.empty() &&
                  ticket.request.input.party.size() == 2,
              "DS.14: an ungrouped bot's whisper carries no group");
        IntentContext facts;
        facts.controllerGuid = ticket.scope.controllerGuid;
        facts.botGuid = ticket.scope.botGuid;
        facts.controllerGroupGuid = 99;
        CommandRefusal why = CommandRefusal::None;
        Check(MapIntent({"s", 33, 11, ChannelIntent::Stay, std::monostate{}}, facts).has_value() &&
                  !MapIntent({"f", 33, 11, ChannelIntent::Follow, std::monostate{}}, facts, &why) &&
                  why == CommandRefusal::NotWithYou,
              "DS.14: an ungrouped bot may stay; it follows nobody");
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
    Check(BridgeSubmit(&players[0], &players[1], SRC_WHISPER_LOCAL, "Other speaker", true, "scope", 0),
          "another direct speaker admitted");
    Check(g_queue.front().request.bridgeTicket.request.input.memory.empty(), "memory separated by speaker");
    AdapterFixture();
    Check(!AdapterSubmit(SRC_RAID_LOCAL), "unsupported raid rejected");
    Check(!BridgeSubmit(&players[0], &players[2], SRC_SAY_LOCAL, "ambient", false, "scope", 0),
          "unaddressed ambient skipped");
    Check(!BridgeSubmit(&players[0], &players[2], SRC_WHISPER_LOCAL, "bot chain", true, "scope", 1),
          "bot-origin chain rejected");
    Check(!AdapterSubmit(SRC_PARTY_LOCAL, std::string(4097, 'x')), "text bound fails closed");
    voice.assign(8193, 'x');
    Check(!AdapterSubmit(), "persona bound fails closed");
    voice = "voice";
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
        BridgeView view;
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
    for (unsigned loss = 0; loss < 5; ++loss)
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
            g_bridgeBindings[0].heartbeat.unixMs = 1;
        else if (loss == 2)
            g_bridgeBindings[0].heartbeat.steadyMs = 1;
        else if (loss == 3)
            party.members.push_back({ObjectGuid(44)});
        else
        {
            g_bridgeWorld.Configured(true, config.bridge, "new-epoch");
            Check(g_bridgeWorld.ObserveDrain(true, RetirementProof::NoLiveCaptures), "reload selected");
            g_bridgeGeneration = g_bridgeWorld.Generation();
        }
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
    for (unsigned i = 1; i <= MaxWorldScopes; ++i)
        Check(BridgeSubmit(&players[0], &players[i], SRC_WHISPER_LOCAL, "Question", true, "scope", 0),
              "bounded distinct conversation admitted");
    g_queue.clear();
    Check(!DispatchSlot::Used() && g_bridgeBindings.size() == MaxWorldScopes,
          "bindings bounded independently of slots");
    Check(!BridgeSubmit(&players[0], &players[65], SRC_WHISPER_LOCAL, "Question", true, "scope", 0),
          "sixty-fifth binding refused even with free worker capacity");
    for (auto& binding : g_bridgeBindings)
        binding.idleUntil.steadyMs = 1;
    OllamaChat_BridgeRefresh();
    Check(g_bridgeBindings.empty(), "idle bindings and memory retire within bounded lifetime");
    Check(BridgeSubmit(&players[0], &players[65], SRC_WHISPER_LOCAL, "Question", true, "scope", 0),
          "expired binding capacity reusable");
    Reset();
}

// DS.10: after its own mark order the executor calls OllamaChat_BridgeNoteOwnMark(bot); the next refresh then opens no
// events route for that bot's bindings, while other bots and human mark changes still publish.
void SelfMarkTests()
{
    using namespace OllamaBridge;
    auto const events = [](uint64_t bot)
    { return static_cast<std::size_t>(std::count(g_eventBots.begin(), g_eventBots.end(), bot)); };
    AdapterFixture();
    secondBot = true;
    players[3].human = false;
    party.members.push_back({ObjectGuid(44)});
    party.marks[0] = players[4].guid;
    Check(AdapterSubmit(), "self-mark: bot party binding");
    Check(BridgeSubmit(&players[0], &players[1], SRC_WHISPER_LOCAL, "Question", true, "scope", 0),
          "self-mark: second binding of the same bot");
    Check(BridgeSubmit(&players[3], &players[2], SRC_PARTY_LOCAL, "A public question.", true, "party-99", 0),
          "self-mark: other bot's binding");
    g_queue.clear();
    OllamaChat_BridgeRefresh();  // A new binding stores no marks; the first refresh takes [A] as its baseline.
    g_eventBots.clear();
    OllamaChat_BridgeRefresh();
    Check(g_bridgeBindings.size() == 3 && g_eventBots.empty(), "self-mark: baseline marks publish nothing");

    party.marks[1] = players[5].guid;
    OllamaChat_BridgeNoteOwnMark(11);
    Check(g_eventBots.empty() && g_bridgeBindings.size() == 3, "self-mark: the re-baseline itself publishes nothing");
    OllamaChat_BridgeRefresh();
    Check(events(11) == 0 && events(44) == 1 && g_eventBots.size() == 1,
          "self-mark: own mark opens no events route; the other bot still reacts once");

    g_eventBots.clear();
    party.marks[2] = players[6].guid;
    OllamaChat_BridgeRefresh();
    Check(events(11) == 2 && events(44) == 1, "self-mark: a human mark still opens one route per binding");

    g_eventBots.clear();
    party.marks[3] = players[7].guid;
    OllamaChat_BridgeNoteOwnMark(44);
    OllamaChat_BridgeRefresh();
    Check(events(11) == 2 && events(44) == 0, "self-mark: the guard is per bot, not per group");

    g_eventBots.clear();
    party.marks[4] = players[8].guid;
    OllamaChat_BridgeNoteOwnMark(999);
    OllamaChat_BridgeRefresh();
    Check(events(11) == 2 && events(44) == 1, "self-mark: a note for another bot keeps these baselines");

    g_eventBots.clear();
    party.marks[5] = players[9].guid;
    players[2].visible = false;
    OllamaChat_BridgeNoteOwnMark(11);
    Check(g_bridgeBindings.size() == 3 && g_eventBots.empty(), "self-mark: a failed capture keeps the binding");
    OllamaChat_BridgeRefresh();
    Check(g_bridgeBindings.size() == 1 && g_eventBots.empty(),
          "self-mark: the refresh retires lost bindings; the re-baselined one stays silent");
    Reset();
}

// The persona store's port: loads and writes wait in submitted until the test completes them.
struct CardPort final : OllamaPersona::StorePort
{
    std::deque<OllamaPersona::StoreOperation> submitted;
    std::deque<OllamaPersona::StoreCompletion> completed;
    bool TrySubmit(OllamaPersona::StoreOperation operation) override
    {
        submitted.push_back(std::move(operation));
        return true;
    }
    std::optional<OllamaPersona::StoreCompletion> TryPop() override
    {
        if (completed.empty())
            return std::nullopt;
        auto completion = std::move(completed.front());
        completed.pop_front();
        return completion;
    }
    // Completes the oldest operation: a load as a confirmed missing row or the given accepted card, a write as
    // committed with its exact readback.
    void Complete(std::string const& accepted = {})
    {
        auto operation = std::move(submitted.front());
        submitted.pop_front();
        OllamaPersona::StoreCompletion completion{operation.botGuid, operation.id, OllamaPersona::LookupStatus::Missing,
                                                  std::nullopt, false};
        if (operation.write)
        {
            completion.lookup = OllamaPersona::LookupStatus::Found;
            completion.row = operation.write->next;
            completion.committed = true;
        }
        else if (!accepted.empty())
        {
            OllamaPersona::State row;
            row.botGuid = operation.botGuid;
            row.revision = 1;
            row.accepted = OllamaPersona::Card{OllamaPersona::Source::Manual, accepted};
            completion.lookup = OllamaPersona::LookupStatus::Found;
            completion.row = std::move(row);
        }
        completed.push_back(std::move(completion));
        OllamaPersona::WorldStore().Pump(std::chrono::steady_clock::now());
    }
} cardPort;

void CardFixture()
{
    while (!cardPort.submitted.empty())
        cardPort.Complete();
    AdapterFixture();
    cardFactsCalls = 0;
    // J5e: a same-state Configure keeps loaded rows, so the fixture switches off first to start empty.
    OllamaPersona::WorldStore().Configure(false);
    OllamaPersona::WorldStore().Configure(true);
    Check(OllamaPersona::WorldStore().SetPort(&cardPort), "card: store port attached");
}

// The controlling human whispers the bot (DS.12: any speaker and channel reads the card).
bool CardSubmit(std::string const& text)
{
    return BridgeSubmit(&players[0], &players[1], SRC_WHISPER_LOCAL, text, true, "whisper-22", 0);
}

bool CardDeliver()
{
    auto done = Finish();
    bool const prepared = PrepareBridge(done);
    DeliverBridge(done, {});
    return prepared;
}

// J14c (DS.8): a card going from empty or loading to ready keeps the binding and its in-flight reply (Refresh and
// Observe); Submit compares strictly and renews the binding with the card, keeping its one-exchange memory. A ready
// card that changes under the same voice is churn: the binding is erased and counted. J14d: Submit keeps a tolerated
// binding while the new card is not ready either or while one of its replies is in flight.
// J5f (DS.14 (d)): the bot state is read on world for every request, outside the capture. Two messages whose
// state differs only in health keep the same binding (BridgeSame, no renewal) and each request carries its own
// reading; a refresh reads no state; no state value reaches the binding signature, system prompt or persona.
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
    Check(stateFactsCalls == 2 && first.request.input.state.present && second.request.input.state.present &&
              first.request.input.state.healthPct == 90.0f && second.request.input.state.healthPct == 80.0f,
          "state: each request carries its own fresh reading");
    Check(second.scope == first.scope && g_bridgeBindings.size() == 1,
          "state: a health-only difference stays BridgeSame (no renewal)");
    auto const leaks = [](std::string const& text) { return text.find("MARKERZONE") != std::string::npos; };
    Check(!leaks(g_bridgeBindings[0].view.signature) && !leaks(g_bridgeBindings[0].view.context.systemPrompt) &&
              !leaks(g_bridgeBindings[0].view.context.persona) && !leaks(second.request.input.systemPrompt) &&
              !leaks(second.request.input.persona) && g_bridgeBindings[0].view.context.state.zone.empty(),
          "state: no state value in the binding, signature, system prompt or persona");
    auto const marked = [](std::string const& text)
    { return text.find("MARKERFACT") != std::string::npos || text.find("MARKERTRAIT") != std::string::npos; };
    Check(factsCalls == 2 && first.request.input.facts && second.request.input.facts &&
              second.request.input.facts->phrase == "MARKERFACT" && second.request.input.traits.words.size() == 1,
          "facts: each request reads the facts and traits at submit");
    Check(!g_bridgeBindings[0].view.context.facts && g_bridgeBindings[0].view.context.traits.words.empty() &&
              !marked(g_bridgeBindings[0].view.signature) && !marked(g_bridgeBindings[0].view.context.persona) &&
              !marked(second.request.input.systemPrompt) && !marked(second.request.input.persona),
          "facts: no fact or trait in the binding, signature, system prompt or persona");
    g_queue.clear();
}

void CardTests()
{
    using namespace OllamaBridge;
    auto const persona = [] { return g_queue.back().request.bridgeTicket.request.input.persona; };

    // A: empty -> loading -> missing across refreshes; the first reply is delivered and remembered.
    CardFixture();
    Check(CardSubmit("First words."), "card A: first message admitted");
    auto const first = g_queue.front().request.bridgeTicket;
    Check(!first.request.input.card.offered && first.request.input.persona == voice && cardPort.submitted.size() == 1,
          "card A: the first message rides on the voice and requests one load");
    OllamaChat_BridgeRefresh();
    Check(g_bridgeBindings.size() == 1 && g_bridgeBindings[0].view.card.status == OllamaPersona::CardStatus::Unready &&
              g_bridgeWorld.Current(first, Now()),
          "card A: empty -> loading keeps the binding and its request");
    cardPort.Complete();
    OllamaChat_BridgeRefresh();
    Check(g_bridgeBindings.size() == 1 && g_bridgeWorld.Current(first, Now()),
          "card A: loading -> missing keeps the binding and its request");
    Check(CardDeliver() && routeCalls == 1 && g_bridgeBindings.size() == 1, "card A: the first reply is delivered");
    Check(g_bridgeBindings[0].memory.find("First words.") != std::string::npos, "card A: first exchange remembered");

    // B: strict Submit renews the binding once, offers the now-known card and keeps the exchange memory.
    auto const churn = g_bridgeCardChurn;
    Check(CardSubmit("Second words."), "card B: next message admitted");
    auto const second = g_queue.back().request.bridgeTicket;
    Check(!(second.scope == first.scope) && second.scope.personaRevision != first.scope.personaRevision &&
              !g_bridgeWorld.Current(first, Now()) && g_bridgeBindings.size() == 1,
          "card B: strict submit renews the binding and its persona revision");
    Check(second.request.input.card.offered && second.request.input.card.stamp && !second.request.input.card.revision &&
              !second.request.input.card.accepted && second.request.input.persona == voice,
          "card B: the renewal offers the missing card");
    Check(second.request.input.card.facts.race == "Dwarf" && second.request.input.card.facts.level == 42 &&
              cardFactsCalls == 1,
          "card B: the offer carries the bot's facts, gathered once on world");
    Check(second.request.input.memory.find("First words.") != std::string::npos,
          "card B: the renewal keeps the one-exchange memory");
    Check(g_bridgeCardChurn == churn, "card B: a renewal from an empty card is not churn");
    Check(CardSubmit("Third words.") && g_queue.back().request.bridgeTicket.scope == second.scope,
          "card B: an unchanged ready card keeps the renewed binding");

    // C: a ready card that changes (a committed proposal) erases the binding and counts churn.
    Check(OllamaPersona::ProposeCard(OllamaPersona::WorldStore(), g_bridgeWorld.Generation(), players[0].guid, "codex",
                                     0, second.request.input.card.stamp,
                                     "A careful hunter.") == OllamaPersona::ProposalStatus::Queued,
          "card C: the generated card is staged for direct application");
    OllamaChat_BridgeRefresh();
    Check(g_bridgeBindings.empty() && g_bridgeCardChurn == churn + 1 && !g_bridgeWorld.Current(second, Now()),
          "card C: a changed ready card erases the binding and counts churn");
    g_queue.clear();
    cardPort.Complete();
    Check(CardSubmit("Fourth words.") && g_queue.back().request.bridgeTicket.request.input.card.revision == 1 &&
              g_queue.back().request.bridgeTicket.request.input.memory.empty(),
          "card C: after churn a fresh binding starts without memory");
    g_queue.clear();

    // D: Observe alone (no refresh between) tolerates a card that became accepted; the renewal carries the card.
    CardFixture();
    Check(CardSubmit("Hello."), "card D: first message admitted");
    cardPort.Complete("A careful hunter.");
    Check(CardDeliver() && routeCalls == 1 && g_bridgeBindings.size() == 1,
          "card D: observe tolerates empty -> accepted, the reply is delivered");
    Check(CardSubmit("Again.") && persona() == voice + "\n\n" + "A careful hunter." &&
              g_queue.back().request.bridgeTicket.request.input.card.accepted &&
              g_queue.back().request.bridgeTicket.request.input.card.revision == 1 &&
              g_queue.back().request.bridgeTicket.request.input.card.facts.race.empty() && !cardFactsCalls,
          "card D: the renewal carries the accepted card and no facts");
    g_queue.clear();

    // E: a message while the card loads keeps the not-ready binding and its revision (J14d: the renewal would bring
    // no card); that binding tolerates loading -> ready and the next message renews it.
    CardFixture();
    Check(CardSubmit("One."), "card E: first message admitted");
    auto const one = g_queue.front().request.bridgeTicket;
    Check(CardDeliver() && routeCalls == 1 && !g_bridgeWorld.PendingFor(one.scope, Now()),
          "card E: the first reply is delivered while the card loads");
    Check(CardSubmit("Two."), "card E: message while loading admitted");
    auto const loading = g_queue.back().request.bridgeTicket;
    Check(g_bridgeBindings.size() == 1 && loading.scope == one.scope &&
              g_bridgeBindings[0].view.card.status == OllamaPersona::CardStatus::Unready &&
              !loading.request.input.card.offered,
          "card E: a loading card keeps the binding and its revision");
    cardPort.Complete();
    OllamaChat_BridgeRefresh();
    Check(g_bridgeBindings.size() == 1 && g_bridgeWorld.Current(loading, Now()) && CardDeliver() && routeCalls == 2,
          "card E: loading -> missing keeps the request and delivers its reply");
    Check(CardSubmit("Three.") && !(g_queue.back().request.bridgeTicket.scope == one.scope) &&
              g_queue.back().request.bridgeTicket.request.input.card.offered &&
              g_queue.back().request.bridgeTicket.request.input.memory.find("Two.") != std::string::npos,
          "card E: the next message renews with the card and the exchange memory");
    g_queue.clear();

    // G (J14d): a quick second line on a first meeting, after the card loaded but before reply 1 arrived, keeps the
    // tolerated binding while reply 1 is in flight. Both replies are delivered in order on the same voice-only view;
    // the next message renews with the card and the latest exchange.
    CardFixture();
    Check(CardSubmit("Hi."), "card G: first message admitted");
    auto const hi = g_queue.front().request.bridgeTicket;
    cardPort.Complete();
    OllamaChat_BridgeRefresh();
    Check(CardSubmit("How are you?"), "card G: second line admitted while reply 1 is in flight");
    auto const how = g_queue.back().request.bridgeTicket;
    Check(g_queue.size() == 2 && how.scope == hi.scope && g_bridgeWorld.Current(hi, Now()) &&
              g_bridgeWorld.Current(how, Now()) && g_bridgeBindings.size() == 1,
          "card G: the in-flight reply keeps the binding and its ticket");
    Check(!how.request.input.card.offered && how.request.input.persona == voice && how.request.input.memory.empty(),
          "card G: the second line rides on the same voice-only view");
    Check(CardDeliver() && routeCalls == 1 && g_bridgeBindings.size() == 1 &&
              g_bridgeBindings[0].memory.find("Hi.") != std::string::npos,
          "card G: reply 1 is delivered first");
    Check(CardDeliver() && routeCalls == 2 && g_bridgeBindings.size() == 1 &&
              g_bridgeBindings[0].memory.find("How are you?") != std::string::npos,
          "card G: reply 2 is delivered next");
    Check(CardSubmit("Glad to hear."), "card G: next message admitted");
    auto const glad = g_queue.back().request.bridgeTicket;
    Check(!(glad.scope == hi.scope) && glad.scope.personaRevision != hi.scope.personaRevision &&
              glad.request.input.card.offered && glad.request.input.persona == voice &&
              glad.request.input.memory.find("How are you?") != std::string::npos && g_bridgeCardChurn == churn + 1,
          "card G: the next message renews with the card and the latest exchange");
    g_queue.clear();

    // F: the tolerance covers card fields only; a voice or audience change still erases a not-ready binding.
    for (unsigned change = 0; change < 2; ++change)
    {
        CardFixture();
        Check(CardSubmit("Hi."), "card F: first message admitted");
        cardPort.Complete();
        if (change == 0)
            voice += " Revised.";
        else
            players[1].name = "Renamed";  // DS.14: a whisper partner's sight is no audience change, a name is.
        OllamaChat_BridgeRefresh();
        Check(g_bridgeBindings.empty() && g_bridgeCardChurn == churn + 1,
              "card F: a non-card change erases a not-ready binding and is not churn");
        g_queue.clear();
    }

    // H (J14e, DS.12): a bot without a card is offered at most MaxCardOffers times per bridge generation.
    CardFixture();
    Check(CardSubmit("Morning."), "card H: first message admitted");
    cardPort.Complete();
    OllamaChat_BridgeRefresh();
    Check(CardDeliver(), "card H: the first reply is delivered");
    unsigned offers = 0;
    for (unsigned i = 0; i < OllamaPersona::MaxCardOffers + 2; ++i)
    {
        Check(CardSubmit("Still there? " + std::to_string(i)), "card H: message admitted");
        auto const card = g_queue.back().request.bridgeTicket.request.input.card;
        if (card.offered)
            ++offers;
        Check(card.offered ? !card.accepted && card.facts.race == "Dwarf" : card.facts.race.empty(),
              "card H: an offer carries the facts, no offer carries none");
        Check(CardDeliver(), "card H: reply delivered");
    }
    Check(offers == OllamaPersona::MaxCardOffers && cardFactsCalls == OllamaPersona::MaxCardOffers,
          "card H: three offers per bot and generation, facts gathered only for an offer");
    g_queue.clear();

    // I (J14e notes): a submit the dispatcher refuses sent no offer and does not use up the bot's offers.
    CardFixture();
    Check(CardSubmit("Morning."), "card I: first message admitted");
    cardPort.Complete();
    OllamaChat_BridgeRefresh();
    Check(CardDeliver(), "card I: the first reply is delivered");
    {
        std::vector<OllamaBridge::DispatchSlot> held;
        while (auto slot = OllamaBridge::DispatchSlot::Acquire())
            held.push_back(std::move(slot));
        for (int i = 0; i < 5; ++i)
            Check(!CardSubmit("Refused " + std::to_string(i)), "card I: a full dispatcher refuses");
    }
    unsigned countedOffers = 0;
    for (unsigned i = 0; i < OllamaPersona::MaxCardOffers + 2; ++i)
    {
        Check(CardSubmit("Back again " + std::to_string(i)), "card I: message admitted");
        if (g_queue.back().request.bridgeTicket.request.input.card.offered)
            ++countedOffers;
        Check(CardDeliver(), "card I: reply delivered");
    }
    Check(countedOffers == OllamaPersona::MaxCardOffers,
          "card I: refused submits did not use up the three offers per bot and generation");
    g_queue.clear();

    // J6d -> J14e: a stranger's whisper reads the bot's own card too (DS.12: the player never sees it).
    CardFixture();
    gameAI.master = nullptr;
    players[0].group = nullptr;
    Check(CardSubmit("Who are you?") && cardPort.submitted.size() == 1,
          "card J14e: a stranger's whisper requests the bot's card");
    g_queue.clear();

    while (!cardPort.submitted.empty())
        cardPort.Complete();
    OllamaPersona::WorldStore().Configure(false);
    Check(OllamaPersona::WorldStore().SetPort(nullptr), "card: store port detached");
    Reset();
}

// J6e-c (DS.14 (c)): one act on a route as the bridge sends it; the act's result.
nlohmann::json WakeAct(OllamaBridge::WorldTicket const& ticket, std::string const& id)
{
    static OllamaBridge::EffectPhase const phase{};
    executorResult = OllamaBridge::EffectResult::Applied;
    nlohmann::json const act = {{"version", 1},
                                {"instance", ticket.scope.instance},
                                {"provider", ticket.scope.provider},
                                {"epoch", ticket.scope.epoch},
                                {"requestId", ticket.request.requestId},
                                {"deadlineUnixMs", ticket.request.deadline.unixMs},
                                {"type", "command"},
                                {"commandId", id},
                                {"issuerGuid", std::to_string(ticket.scope.controllerGuid)},
                                {"botGuid", std::to_string(ticket.scope.botGuid)},
                                {"intent", "attack_own_target"},
                                {"params", nlohmann::json::object()}};
    uint64_t receipt = 0;
    Check(g_bridgeChannel.Receive(config.bridge.token, ticket.request.requestId, act.dump(), receipt) ==
              OllamaBridge::ChannelError::None,
          "wake: the act was received on the wake route");
    Check(g_bridgeChannel.Update(&phase) == 1, "wake: one world tick takes the act");
    std::unique_ptr<OllamaBridge::ChannelPacket> packet;
    Check(g_bridgeChannel.Take(config.bridge.token, receipt, packet) == OllamaBridge::ChannelError::None && packet &&
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
    Check(stateFactsCalls == 2 && input.state.present, "wake: the bot's state is read again for the wake");
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

    Check(BridgeSubmit(&players[0], &players[2], SRC_SAY_LOCAL, "Bot, wait for me.", true, "say-key", 0),
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
        SelfMarkTests();
        CardTests();
        StateTests();
        g_queue.clear();
        g_done.clear();
        Check(!OllamaBridge::DispatchSlot::Used(), "final no leaked slots");
        std::cout << checks << " dispatch bridge checks passed (extracted helpers; game/API stubs)\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
