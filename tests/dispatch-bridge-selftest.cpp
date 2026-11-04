// Actual bridge/world translation units plus exact extracted dispatch helpers (see hub J4b-2a-check runner).
// Game adapter, route, governor, cleanup, config and API are stubs. No full dispatch TU or live-game proof.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "mod-ollama-chat_bridge_world.h"

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

#include "dispatch-types.inc"

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
std::string ProcessLlmResponse(std::string const& text, std::string const&, uint32_t*)
{
    ++cleanupCalls;
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
};
struct Player
{
    bool IsInWorld() const { return true; }
    ObjectGuid GetGUID() const { return ObjectGuid(11); }
};
struct PlayerbotAI
{
};
struct Channel
{
};
struct OllamaWorldSnapshot
{
};
struct ObjectAccessor
{
    static Player* FindConnectedPlayer(ObjectGuid)
    {
        static Player player;
        return &player;
    }
};
struct PlayerbotsMgr
{
    static PlayerbotsMgr& instance()
    {
        static PlayerbotsMgr manager;
        return manager;
    }
    PlayerbotAI* GetPlayerbotAI(Player*)
    {
        static PlayerbotAI ai;
        return &ai;
    }
};
bool Governor_IsRepetitive(ObjectGuid, std::string const&, std::string const&) { return false; }
bool Governor_TryConsumeSend(ObjectGuid, std::string const&, bool)
{
    ++governorCalls;
    return true;
}
void Governor_RecordUtterance(ObjectGuid, std::string const&, std::string const&) { ++recordCalls; }
bool RouteMessage(Player*, PlayerbotAI*, Completion const& c, OllamaWorldSnapshot const&, Channel*&)
{
    ++routeCalls;
    routedText = c.text;
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
    g_queue.clear();
    g_done.clear();
    Check(OllamaBridge::DispatchSlot::Used() == 0, "all physical slots disposed");
    config = {};
    config.provider = OllamaProvider::Bridge;
    config.bridge = {"fixture", "codex", "http://127.0.0.1:47800", "synthetic-game-link-bearer-only"};
    config.systemPrompt = "owned-system";
    g_bridgeWorld.Configured(true, config.bridge, "epoch-1");
    Check(g_bridgeWorld.ObserveDrain(true, OllamaBridge::RetirementProof::GenerationOnlyNoCaptures), "selected");
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
}

bool Submit(bool addressed = true, std::string memory = {}, int64_t lifetimeMs = 1000)
{
    OllamaChatRequest request;
    request.botGuid = 11;
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
        !g_bridgeWorld.ObserveDrain(g_bridgeInFlight == 0, OllamaBridge::RetirementProof::GenerationOnlyNoCaptures);
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
    Check(g_bridgeWorld.ObserveDrain(true, OllamaBridge::RetirementProof::GenerationOnlyNoCaptures),
          "actual drain permits selection");
}
}  // namespace

int main()
{
    try
    {
        GuardTests();
        AdmissionTests();
        CapacityTests();
        PublicationTest();
        ClaimedScopeTests();
        TerminalTests();
        DrainTest();
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
