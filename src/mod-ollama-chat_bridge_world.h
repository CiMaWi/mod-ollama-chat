#ifndef MOD_OLLAMA_CHAT_BRIDGE_WORLD_H
#define MOD_OLLAMA_CHAT_BRIDGE_WORLD_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "mod-ollama-chat_bridge.h"
#include "mod-ollama-chat_bridge_channel.h"

namespace OllamaBridge
{
constexpr std::size_t MaxWorldPending = 64;
constexpr std::size_t MaxWorldScopes = 64;
constexpr uint64_t MaxEpochRequests = 4096;
constexpr int64_t WorldLivenessMs = 5000;

struct WorldTicket
{
    uint64_t generation = 0;
    Request request;
    WorldScope scope;
};

enum class WorldOutcome : uint8_t
{
    Success,
    Unavailable,
    Empty,
    Filtered,
    Malformed,
    Timeout,
    CircuitOpen,
    QueueFull,
    Cancelled
};

enum class RetirementProof : uint8_t
{
    Unknown,
    GenerationOnlyNoCaptures
};

enum class DispatchSource : uint8_t
{
    Unsupported,
    Whisper,
    Say,
    Yell,
    Party
};

// Trusted world adapter output, never decoded from model/native input. Audience is sorted and unique.
// The adapter must resolve current humans, authority, exact group, map/phase and visibility/hearing afresh.
struct DispatchGuard
{
    WorldScope scope;
    DispatchSource source = DispatchSource::Unsupported;
    uint64_t speakerGuid = 0;
    uint64_t groupGuid = 0;
    uint32_t mapId = 0;
    uint32_t instanceId = 0;
    uint32_t phaseMask = 0;
    std::vector<uint64_t> audience;
    bool humanSpeaker = false;
    bool authority = false;
    bool perceptible = false;
    bool audienceOnline = false;
    bool nonRaid = false;
};

bool DispatchGuardCurrent(WorldTicket const& ticket, DispatchGuard const& captured, DispatchGuard const& current,
                          Clock now);

enum class TerminalAction : uint8_t
{
    Silence,
    Reply,
    Outage
};

TerminalAction DispatchTerminal(WorldOutcome outcome, bool addressedHuman);
char const* DispatchOutageText();

// One physical slot for the entire owned task/completion lifetime, independent of logical reservations.
// Move-only value: no game pointer, reference, callback or allocator crosses the worker boundary.
class DispatchSlot
{
public:
    DispatchSlot() = default;
    ~DispatchSlot();
    DispatchSlot(DispatchSlot const&) = delete;
    DispatchSlot& operator=(DispatchSlot const&) = delete;
    DispatchSlot(DispatchSlot&& other) noexcept;
    DispatchSlot& operator=(DispatchSlot&& other) noexcept;
    static DispatchSlot Acquire();
    static std::size_t Used();
    explicit operator bool() const { return _held; }

private:
    void Release();
    bool _held = false;
    static std::atomic<std::size_t> _used;
};

// One independent owned HTTP lane. Only Poll/BindEpoch/Publish on the world thread can accept authority.
// Each challenge consumes a new complete capture; no sender retry or cached heartbeat exists.
class WorldObserver
{
public:
    using Transport =
        std::function<OllamaHttpResult(Settings const&, OllamaObserverOperation, std::string const&, int64_t)>;
    struct View
    {
        bool enabled = false;
        std::string incarnation;
        std::string epoch;
        bool pending = false;
    };

    explicit WorldObserver(Transport transport, ReadClock clock = Now);
    ~WorldObserver();
    void Start();
    void Stop();  // Terminal shutdown only; never called on tick/reload.
    void Configured(bool enabled, Settings settings, Clock now);
    void Poll(Clock now);
    View State() const;
    void Open(bool worldDrained, Clock now);
    bool BindEpoch(std::string epoch, bool worldDrained, Clock now);
    void Publish(std::vector<WorldScope> scopes, bool worldDrained, Clock now);
    bool Current(WorldScope const& scope, Clock now) const;
    void Forget(WorldScope const& scope);
    void ForgetAll();

private:
    struct Job
    {
        uint64_t serial = 0;
        Settings settings;
        OllamaObserverOperation operation = OllamaObserverOperation::Open;
        std::string body;
        Deadline deadline;
        ObserverChallenge previous;
        std::vector<WorldScope> scopes;
    };
    struct Reply
    {
        Job job;
        bool valid = false;
        ObserverChallenge challenge;
        Deadline nextDeadline;
    };
    void Run();
    void Reset(Clock now);
    void RetirePacket(Clock now);

    Transport const _transport;
    ReadClock const _clock;
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    std::thread _worker;
    bool _running = false;
    bool _busy = false;
    bool _enabled = false;
    uint64_t _serial = 1;
    Settings _settings;
    ObserverChallenge _challenge;
    std::string _epoch;
    Deadline _challengeUntil;
    Deadline _liveUntil;
    int64_t _lastSteady = 0;
    int64_t _retryAt = 0;
    int64_t _refreshAt = 0;
    std::vector<WorldScope> _accepted;
    std::vector<WorldScope> _observed;
    std::vector<WorldScope> _sent;
    std::optional<Job> _job;
    std::optional<Reply> _reply;
};

// Single world-thread owner. No game pointers, game references, native grants or tools.
// Configured is an explicit notification even when only the bearer changes; no per-tick config polling.
// Selected means module circuit selection only. J4b-X must emit authenticated fresh state to native code.
class WorldPolicy
{
public:
    explicit WorldPolicy(WorldObserver* observer = nullptr) : _observer(observer) {}
    std::vector<WorldScope> Scopes(Clock now) const;
    void Configured(bool enabled, Settings settings, std::string epoch);
    void Shutdown();
    bool ObserveDrain(bool drained, RetirementProof proof);
    bool ObserveScope(WorldScope scope, Clock now);
    void InvalidateScope(uint64_t botGuid, std::string const& contextId);
    void Expire(Clock now);

    WorldTicket Reserve(WorldScope const& scope, Context input, Clock now, int64_t lifetimeMs);
    bool MarkQueued(WorldTicket const& ticket, Clock now);
    bool Complete(WorldTicket const& ticket, WorldOutcome outcome, Clock now);
    bool Current(WorldTicket const& ticket, Clock now) const;
    // Claims once; retains current scope/deadline validation until the world caller calls Cancel on every exit.
    bool Consume(WorldTicket const& ticket, Clock now);
    void Cancel(WorldTicket const& ticket);

    uint64_t Generation() const { return _generation; }
    Identity Selected() const { return _selected; }
    Settings Snapshot() const { return _settings; }
    std::size_t Pending() const { return _pending.size(); }
    bool Retiring() const { return _retiring; }

private:
    enum class Stage : uint8_t
    {
        Reserved,
        Queued,
        Completed,
        Consumed
    };

    struct ScopeEntry
    {
        WorldScope scope;
        Deadline liveUntil;
    };

    struct PendingEntry
    {
        WorldTicket ticket;
        Stage stage = Stage::Reserved;
    };

    bool ScopeCurrent(WorldScope const& scope, Clock now) const;
    void Retire();

    WorldObserver* const _observer;  // Owned lane outlives policy; never carried to a generation task.
    uint64_t _generation = 0;
    uint64_t _sequence = 0;  // Never reset, including reload, rejection, cancellation and queue-full.
    uint64_t _issued = 0;
    bool _enabled = false;
    bool _retiring = false;
    Settings _settings;
    Identity _desired;
    Identity _selected;
    std::vector<ScopeEntry> _scopes;
    std::unordered_map<std::string, PendingEntry> _pending;
};
// An owned response keeps its physical slot until its body has actually been destroyed.
// Native adapters may hold this value during asynchronous writes, but must not copy its body into untracked work.
class ChannelPacket
{
    friend class WorldChannel;
    struct Lease
    {
        explicit Lease(std::shared_ptr<std::atomic<std::size_t>> count) : used(std::move(count)) { ++*used; }
        ~Lease() { --*used; }
        std::shared_ptr<std::atomic<std::size_t>> used;
    };
    std::unique_ptr<Lease> _lease;  // Declared first: destroyed AFTER every owned payload below.
    std::string _route;
    uint64_t _serial = 0;
    Deadline _until;

public:
    uint64_t receipt = 0;
    ChannelError error = ChannelError::None;
    std::string body;
};

// Provider-neutral reverse channel. Receive/Take are I/O-thread safe; all other methods are world-only.
// There is no HTTP, waiting, worker callback or game pointer in these mailboxes. Native adapters bound sockets
// and bytes before Receive; their async write owns the returned packet until disposal, including cancellation.
class QueryWorldSource;

struct EventPublication
{
    std::string route;
    uint64_t receipt = 0;
};

class WorldChannel
{
public:
    using Observer = std::function<bool(WorldTicket const&, DispatchGuard&)>;
    static constexpr std::size_t MaxPackets = 64;
    static constexpr std::size_t TickBudget = 8;
    explicit WorldChannel(WorldPolicy& world, Observer observer, ReadClock clock = Now,
                          QueryWorldSource* source = nullptr);
    void Configure(bool enabled);
    void Retire();
    bool Activate();
    bool Register(WorldTicket const& ticket, DispatchGuard const& guard);
    // World-only: a separate game reservation, never an already registered generation route.
    // J5c supplies freshly filtered marks; native adapters discover the receipt out of band.
    bool RegisterEvents(WorldTicket const& ticket, DispatchGuard const& guard,
                        std::vector<ChannelObservedMark> const& events, uint64_t& receipt,
                        std::function<bool()> const& current = {});
    // Synchronous world capture; reserves a separate event route and never renews an observation.
    bool ObserveEvents(DispatchGuard const& guard, std::vector<uint8_t> const& icons, EventPublication& publication);
    void Close(WorldTicket const& ticket);
    std::size_t Update();
    bool Drained() const { return _used->load() == 0; }
    std::size_t Used() const { return _used->load(); }
    ChannelError Receive(std::string_view bearer, std::string_view route, std::string_view body, uint64_t& receipt);
    ChannelError Take(std::string_view bearer, uint64_t receipt, std::unique_ptr<ChannelPacket>& packet);

private:
    struct Binding
    {
        WorldTicket ticket;
        DispatchGuard guard;
        bool events = false;
    };
    bool Authenticated(std::string_view bearer) const;  // Caller holds _mutex.
    bool Fresh(Binding const& binding, Clock now) const;
    WorldPolicy& _world;
    QueryWorldSource* const _source;  // WORLD ONLY; never copied into a packet or worker task.
    Observer const _observer;         // Invoked only synchronously by the world owner, never by Receive/Take.
    ReadClock const _clock;
    ChannelPolicy _policy;
    bool _enabled = false;
    Identity _selected;
    std::unordered_map<std::string, Binding> _bindings;
    std::shared_ptr<std::atomic<std::size_t>> _used = std::make_shared<std::atomic<std::size_t>>(0);
    mutable std::mutex _mutex;
    std::string _bearer;
    uint64_t _serial = 0;
    uint64_t _sequence = 0;
    std::deque<std::unique_ptr<ChannelPacket>> _receive;
    std::unordered_map<uint64_t, std::unique_ptr<ChannelPacket>> _done;
};
}  // namespace OllamaBridge

#endif
