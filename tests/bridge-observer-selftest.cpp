// Production observer/serializer/world policy plus verbatim extracted dispatch lifecycle helpers.
// Transport, world config and cryptographic entropy are controlled stand-ins; no live authority/delivery proof.
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "mod-ollama-chat_bridge_world.h"
#include "observer-settings.inc"

namespace
{
using namespace OllamaBridge;
unsigned checks = 0;

void Check(bool value, char const* label)
{
    ++checks;
    if (!value)
        throw std::runtime_error(label);
}

template <typename Predicate>
void Until(Predicate condition)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!condition())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("observer condition deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

Settings Config()
{
    return {"fixture",
            "codex",
            "http://127.0.0.1:47800",
            "synthetic-game-link-bearer-only",
            true,
            "C:/synthetic/observer.json",
            std::string(64, 'a')};
}

std::string Challenge(uint32_t sequence, char incarnation = 'b')
{
    return nlohmann::json{{"version", 1},
                          {"type", "observer.challenge"},
                          {"instance", "fixture"},
                          {"provider", "codex"},
                          {"serviceId", "service"},
                          {"incarnation", std::string(32, incarnation)},
                          {"challenge", std::string(31, 'c') + "0123456789abcdef"[sequence % 16]},
                          {"sequence", sequence},
                          {"freshnessMs", 5000}}
        .dump();
}

WorldScope Scope(std::string const& epoch)
{
    return {1, "fixture", "codex", epoch, UINT64_MAX, 22, "conversation", "direct", "visible", "r1", "m1", "p1"};
}

OllamaHttpResult Respond(OllamaObserverOperation operation, std::string const& body)
{
    auto const packet = nlohmann::json::parse(body);
    return {200, Challenge(operation == OllamaObserverOperation::Open ? 1 : packet["sequence"].get<uint32_t>() + 1),
            ""};
}

void Wire()
{
    auto settings = Config();
    ObserverChallenge first;
    Check(ParseObserverChallenge(settings, Challenge(1), nullptr, first), "exact open challenge");
    ObserverChallenge next;
    Check(ParseObserverChallenge(settings, Challenge(2), &first, next), "exact next challenge");
    Check(!ParseObserverChallenge(settings, Challenge(3), &first, next), "reordered challenge");
    Check(!ParseObserverChallenge(settings, Challenge(1), &first, next), "replayed challenge");
    Check(!ParseObserverChallenge(settings, Challenge(2, 'd'), &first, next), "changed incarnation");
    auto const valid = nlohmann::json::parse(Challenge(1));
    for (auto it = valid.begin(); it != valid.end(); ++it)
    {
        auto missing = valid;
        missing.erase(it.key());
        Check(!ParseObserverChallenge(settings, missing.dump(), nullptr, next), "missing challenge field");
        missing = valid;
        missing[it.key()] = nullptr;
        Check(!ParseObserverChallenge(settings, missing.dump(), nullptr, next), "null challenge field");
    }
    auto wrong = valid;
    wrong["extra"] = 1;
    Check(!ParseObserverChallenge(settings, wrong.dump(), nullptr, next), "unknown challenge field");
    wrong = valid;
    wrong["freshnessMs"] = 5001;
    Check(!ParseObserverChallenge(settings, wrong.dump(), nullptr, next), "no widened freshness");
    wrong = valid;
    wrong["sequence"] = 1.0;
    Check(!ParseObserverChallenge(settings, wrong.dump(), nullptr, next), "integer sequence only");
    wrong = valid;
    wrong["instance"] = "other";
    Check(!ParseObserverChallenge(settings, wrong.dump(), nullptr, next), "identity mismatch");
    auto duplicate = Challenge(1);
    duplicate.insert(1, "\"version\":1,");
    Check(!ParseObserverChallenge(settings, duplicate, nullptr, next), "duplicate challenge field");
    Check(!ParseObserverChallenge(settings, std::string(32769, 'x'), nullptr, next), "bounded response");
    std::string body;
    Check(BuildObserverOpen(settings, body) && nlohmann::json::parse(body).size() == 4, "exact open packet");
    std::string const epoch = first.incarnation + "." + std::string(32, 'd');
    auto scope = Scope(epoch);
    Check(BuildObserverSnapshot(settings, first, epoch, true, true, {scope}, body), "complete snapshot");
    auto packet = nlohmann::json::parse(body);
    Check(packet.size() == 12 && packet["scopes"][0].size() == 12, "exact snapshot and scope shape");
    Check(!BuildObserverSnapshot(settings, first, epoch, false, true, {scope}, body), "inactive requires empty");
    Check(BuildObserverSnapshot(settings, first, epoch, false, false, {}, body), "inactive retirement packet");
    Check(!BuildObserverSnapshot(settings, first, "old-epoch", true, true, {}, body), "incarnation namespace");
    auto other = scope;
    other.contextId = "other-context";
    Check(!BuildObserverSnapshot(settings, first, epoch, true, true, {scope, other}, body), "unique peer");
    other = scope;
    other.controllerGuid++;
    Check(!BuildObserverSnapshot(settings, first, epoch, true, true, {scope, other}, body), "unique slot");
    std::vector<WorldScope> scopes;
    for (unsigned i = 0; i < 64; ++i)
    {
        scope.botGuid = i + 1;
        scopes.push_back(scope);
    }
    Check(BuildObserverSnapshot(settings, first, epoch, true, true, scopes, body), "64 bounded scopes");
    scopes.push_back(scope);
    Check(!BuildObserverSnapshot(settings, first, epoch, true, true, scopes, body), "65 scopes refused");
    scopes.pop_back();
    auto largeSettings = settings;
    largeSettings.instance = std::string(128, 'x');
    for (auto& entry : scopes)
    {
        entry.instance = largeSettings.instance;
        entry.contextId = entry.perceptionScope = entry.contextRevision = entry.membershipRevision =
            entry.personaRevision = std::string(128, 'x');
    }
    Check(!BuildObserverSnapshot(largeSettings, first, epoch, true, true, scopes, body), "complete encoded byte cap");
    first.sequence = 2147483647;
    Check(!BuildObserverSnapshot(settings, first, epoch, true, true, {}, body), "sequence never wraps");

    WorldPolicy policy;
    policy.Configured(true, settings, epoch);
    Check(policy.ObserveDrain(true, RetirementProof::GenerationOnlyNoCaptures), "policy selected after drain");
    scope = Scope(epoch);
    Clock const now{100000, 200000};
    Check(policy.ObserveScope(scope, now), "scope world capture");
    Context input;
    input.speakerGuid = 99;
    input.channel = "direct";
    input.text = "hello";
    input.persona = std::string(8192, 'p');
    input.memory = std::string(8192, 'm');
    input.party = {{UINT64_MAX, "visible", "", -1, 0}};
    auto ticket = policy.Reserve(scope, input, now, 30000);
    Check(ticket.generation && Build(settings, ticket.request, now, body), "production reserve bound serializer");
    packet = nlohmann::json::parse(body);
    Check(packet["input"]["version"] == 2 && packet["scope"]["botGuid"] == "18446744073709551615" &&
              packet["input"]["speakerGuid"] == "99",
          "exact uint64 binding and distinct speaker");
    Check(packet["input"]["persona"] == input.persona && packet["input"]["memory"] == input.memory &&
              !packet["input"]["party"][0].contains("healthPercent"),
          "lossless fields and absence");
    ticket.request.scope.channel = "party";
    Check(!Build(settings, ticket.request, now, body), "scope channel mismatch refused");
    ticket.request.scope = {};
    Check(!Build(settings, ticket.request, now, body), "native missing binding refused");
    settings.observerEnabled = false;
    Check(Build(settings, ticket.request, now, body) && !nlohmann::json::parse(body).contains("scope"),
          "legacy off wire unchanged");
}

void Lane()
{
    std::atomic<int64_t> ms{200000};
    auto clock = [&] { return Clock{100000 + ms.load(), ms.load()}; };
    std::atomic<unsigned> calls{0};
    std::mutex gate;
    std::condition_variable wake;
    bool blocked = false;
    bool entered = false;
    bool fail = false;
    bool restart = false;
    WorldObserver observer(
        [&](Settings const&, OllamaObserverOperation operation, std::string const& body, int64_t)
        {
            ++calls;
            std::unique_lock<std::mutex> lock(gate);
            entered = true;
            wake.notify_all();
            wake.wait(lock, [&] { return !blocked; });
            auto result = fail ? OllamaHttpResult{503, "", "synthetic"} : Respond(operation, body);
            if (restart && result.ok())
            {
                auto packet = nlohmann::json::parse(result.body);
                packet["incarnation"] = std::string(32, 'd');
                result.body = packet.dump();
            }
            return result;
        },
        clock);
    observer.Start();
    observer.Configured(true, Config(), clock());
    observer.Open(false, clock());
    Check(calls == 0, "no open before actual world drain");
    observer.Open(true, clock());
    Until(
        [&]
        {
            observer.Poll(clock());
            return !observer.State().incarnation.empty();
        });
    auto const epoch = observer.State().incarnation + "." + std::string(32, 'e');
    auto scope = Scope(epoch);
    Check(!observer.Current(scope, clock()), "challenge is not authority");
    Check(!observer.BindEpoch(epoch, false, clock()), "no epoch without actual drain");
    Check(observer.BindEpoch(epoch, true, clock()), "world epoch bound after drain");
    WorldPolicy policy(&observer);
    policy.Configured(true, Config(), epoch);
    Check(policy.ObserveDrain(true, RetirementProof::GenerationOnlyNoCaptures) && policy.ObserveScope(scope, clock()),
          "world policy attached to observer invalidation");
    observer.Publish({scope}, true, clock());
    Until(
        [&]
        {
            observer.Poll(clock());
            return observer.Current(scope, clock());
        });
    auto old = scope;
    old.contextRevision = "stale";
    Check(!observer.Current(old, clock()), "exact binding, no newest-peer alias");
    ms += 1000;
    auto before = calls.load();
    observer.Publish({scope}, false, clock());
    Until(
        [&]
        {
            observer.Poll(clock());
            return calls > before && !observer.State().pending;
        });
    Check(observer.Current(scope, clock()), "refresh while generation workers not drained");
    for (unsigned refresh = 0; refresh < 5; ++refresh)
    {
        ms += 1000;
        observer.Publish({scope}, false, clock());
        Until(
            [&]
            {
                observer.Poll(clock());
                return !observer.State().pending;
            });
    }
    Check(observer.Current(scope, clock()), "fresh world refresh survives original deadline with inference held");
    // A reply delayed across scope loss must never restore its old exact binding.
    {
        std::lock_guard<std::mutex> lock(gate);
        blocked = true;
        entered = false;
    }
    ms += 1000;
    observer.Publish({scope}, false, clock());
    Until(
        [&]
        {
            observer.Poll(clock());
            observer.Publish({scope}, false, clock());
            std::lock_guard<std::mutex> lock(gate);
            return entered;
        });
    before = calls;
    policy.InvalidateScope(scope.botGuid, scope.contextId);
    for (unsigned i = 0; i < 1000; ++i)
        observer.Publish({}, false, clock());
    Check(calls == before && !observer.Current(scope, clock()), "bounded stalled lane and immediate scope loss");
    {
        std::lock_guard<std::mutex> lock(gate);
        blocked = false;
    }
    wake.notify_all();
    Until(
        [&]
        {
            observer.Poll(clock());
            observer.Publish({}, false, clock());
            return calls > before;
        });
    Check(!observer.Current(scope, clock()), "delayed snapshot cannot revive an invalidated scope");
    {
        std::lock_guard<std::mutex> lock(gate);
        blocked = true;
        entered = false;
    }
    scope.contextRevision = "r2";
    Until(
        [&]
        {
            observer.Poll(clock());
            observer.Publish({scope}, false, clock());
            std::lock_guard<std::mutex> lock(gate);
            return entered;
        });
    // Reload changes only observer bearer. It revokes synchronously and cannot join the held transport.
    auto rotated = Config();
    rotated.observerToken = std::string(64, 'f');
    observer.Configured(true, rotated, clock());
    Check(observer.State().epoch.empty() && !observer.Current(scope, clock()), "token-only reload revokes");
    {
        std::lock_guard<std::mutex> lock(gate);
        blocked = false;
    }
    wake.notify_all();
    Until(
        [&]
        {
            observer.Poll(clock());
            observer.Open(true, clock());
            return !observer.State().incarnation.empty();
        });
    auto const newEpoch = observer.State().incarnation + "." + std::string(32, 'f');
    Check(observer.BindEpoch(newEpoch, true, clock()), "reopen needs fresh world epoch");
    scope = Scope(newEpoch);
    observer.Publish({scope}, true, clock());
    Until(
        [&]
        {
            observer.Poll(clock());
            return observer.Current(scope, clock());
        });
    ms += 5000;
    Check(!observer.Current(scope, clock()), "exact liveness boundary before poll");
    observer.Poll(clock());
    Check(observer.State().epoch.empty(), "expiry requires retirement and reopen");
    observer.Configured(true, Config(), clock());
    {
        std::lock_guard<std::mutex> lock(gate);
        fail = true;
    }
    before = calls;
    observer.Open(true, clock());
    Until(
        [&]
        {
            observer.Poll(clock());
            return calls > before && !observer.State().pending;
        });
    Check(observer.State().epoch.empty() && !observer.Current(scope, clock()), "failed open cannot activate");
    {
        std::lock_guard<std::mutex> lock(gate);
        fail = false;
    }
    ms += 1000;
    Until(
        [&]
        {
            observer.Poll(clock());
            observer.Open(true, clock());
            return !observer.State().incarnation.empty();
        });
    auto thirdEpoch = observer.State().incarnation + "." + std::string(32, '1');
    Check(observer.BindEpoch(thirdEpoch, true, clock()), "new epoch after failed open");
    scope = Scope(thirdEpoch);
    observer.Publish({scope}, true, clock());
    Until(
        [&]
        {
            observer.Poll(clock());
            return observer.Current(scope, clock());
        });
    {
        std::lock_guard<std::mutex> lock(gate);
        restart = true;
    }
    ms += 1000;
    observer.Publish({scope}, false, clock());
    Until(
        [&]
        {
            observer.Poll(clock());
            return observer.State().epoch.empty();
        });
    Check(!observer.Current(scope, clock()), "native restart retires old epoch");
    ms += 1000;
    Until(
        [&]
        {
            observer.Poll(clock());
            observer.Open(true, clock());
            return !observer.State().incarnation.empty();
        });
    Check(!observer.BindEpoch(thirdEpoch, true, clock()), "restart refuses old world namespace");
    auto fourthEpoch = observer.State().incarnation + "." + std::string(32, '2');
    Check(observer.BindEpoch(fourthEpoch, true, clock()), "world creates restarted namespace epoch");
    scope = Scope(fourthEpoch);
    observer.Publish({scope}, true, clock());
    Until(
        [&]
        {
            observer.Poll(clock());
            return observer.Current(scope, clock());
        });
    --ms;
    Check(!observer.Current(scope, clock()), "monotonic rollback is not current");
    observer.Poll(clock());
    Check(observer.State().epoch.empty(), "rollback retires epoch");
    observer.Configured(false, {}, clock());
    observer.Open(true, clock());
    Check(!observer.State().enabled, "disable cannot reopen");
    observer.Stop();
}

// These stand-ins make only the unchanged external seams synthetic. The included lifecycle bodies are production.
enum class OllamaProvider
{
    Ollama,
    Bridge
};
struct OllamaEndpointSettings
{
    OllamaProvider provider = OllamaProvider::Bridge;
    Settings bridge = Config();
};
OllamaEndpointSettings config;
OllamaEndpointSettings OllamaConfig_Snapshot() { return config; }
bool g_Enable = true;
std::atomic<unsigned> adapterCalls{0};
WorldObserver g_nativeObserver(
    [](Settings const&, OllamaObserverOperation operation, std::string const& body, int64_t)
    {
        ++adapterCalls;
        return Respond(operation, body);
    });
WorldPolicy g_bridgeWorld(&g_nativeObserver);
bool g_nativeSelected = false;
Settings g_nativeSettings;
std::atomic<uint64_t> g_bridgeGeneration{0};
std::atomic<uint32_t> g_bridgeInFlight{0};
struct Task
{
    struct
    {
        uint64_t bridgeGeneration = 0;
    } request;
};
using Completion = Task;
std::mutex g_queueMutex;
std::mutex g_doneMutex;
std::deque<Task> g_queue;
std::deque<Completion> g_done;
Identity activated;
void OllamaBridge_SetActivation(Identity identity) { activated = std::move(identity); }
unsigned resets = 0;
void OllamaChat_BridgeReset() { ++resets; }
namespace Acore::Crypto
{
template <std::size_t N>
std::array<unsigned char, N> GetRandomBytes()
{
    static unsigned char sequence = 0;
    std::array<unsigned char, N> bytes{};
    bytes.fill(++sequence);
    return bytes;
}
}  // namespace Acore::Crypto
// The channel, its event queue and the enable flag the extracted lifecycle also drives. Queries and commands are not
// part of this check, so the channel runs without a query source or executor (nothing is admitted).
bool g_channelEnabled = false;
std::deque<EventPublication> g_eventPublications;
WorldChannel g_bridgeChannel(g_bridgeWorld, [](WorldTicket const&, DispatchGuard&) { return false; });
#include "observer-adapter.inc"

void Adapter()
{
    g_nativeObserver.Start();
    g_bridgeInFlight = 1;
    OllamaDispatch_BridgeConfigured(false);
    UpdateNativeObserver();
    Check(g_nativeSelected && activated.epoch.empty() && adapterCalls == 0, "adapter waits for retired worker");
    g_bridgeInFlight = 0;
    auto slot = DispatchSlot::Acquire();
    UpdateNativeObserver();
    Check(adapterCalls == 0, "cancelled reservations cannot replace physical slot drain");
    slot = {};
    Until(
        [&]
        {
            UpdateNativeObserver();
            return !activated.epoch.empty();
        });
    auto const first = activated.epoch;
    Check(first.size() == 65 && first.substr(0, 32) == std::string(32, 'b'), "world incarnation epoch creation");
    Check(g_bridgeGeneration != 0, "selected generation published after actual drain");
    config.bridge.observerToken = std::string(64, 'f');
    g_bridgeInFlight = 1;
    OllamaDispatch_BridgeConfigured(false);
    Check(activated.epoch.empty() && g_bridgeGeneration == 0, "actual configured helper token reload revokes");
    UpdateNativeObserver();
    Check(g_bridgeWorld.Selected().epoch.empty(), "actual helper waits for retiring worker");
    g_bridgeInFlight = 0;
    Until(
        [&]
        {
            UpdateNativeObserver();
            return !activated.epoch.empty();
        });
    Check(first != activated.epoch, "world creates fresh suffix on reopen");
    config.provider = OllamaProvider::Ollama;
    OllamaDispatch_BridgeConfigured(false);
    Check(!g_nativeSelected && activated.epoch.empty() && !g_nativeObserver.State().enabled, "provider switch revokes");
    config.provider = OllamaProvider::Bridge;
    config.bridge.observerEnabled = false;
    OllamaDispatch_BridgeConfigured(false);
    Check(!g_nativeSelected && g_bridgeWorld.ObserveDrain(true, RetirementProof::GenerationOnlyNoCaptures),
          "legacy selection unchanged");
    g_Enable = false;
    OllamaDispatch_BridgeConfigured(false);
    Check(!g_nativeSelected && g_bridgeWorld.Selected().epoch.empty(), "module disabled inactive");
    g_nativeObserver.Stop();
}
}  // namespace

int main()
{
    try
    {
        Wire();
        Lane();
        Adapter();
        std::cout << "bridge-observer-selftest: " << checks << " checks passed (synthetic; no game delivery)\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "bridge-observer-selftest failed: " << error.what() << '\n';
        return 1;
    }
}
