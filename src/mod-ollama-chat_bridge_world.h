#ifndef MOD_OLLAMA_CHAT_BRIDGE_WORLD_H
#define MOD_OLLAMA_CHAT_BRIDGE_WORLD_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "mod-ollama-chat_bridge.h"
#include "mod-ollama-chat_bridge_channel.h"
#include "mod-ollama-chat_bridge_effect.h"
#include "mod-ollama-chat_bridge_watch.h"

namespace OllamaBridge
{
// LC.38: the world's tables (scopes, tickets, bindings, rate, watches) hold one entry per bot in talk and have no
// fixed count; their size is bounded by the bots in talk and each ticket's lifetime.
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
    Silent,  // DS.14: an action-only answer; it stands only when its route made an authorized effect attempt.
    Unavailable,
    Empty,
    Filtered,
    Malformed,
    Timeout,
    QueueFull,
    Cancelled
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

// LCB3c: one capture of a conversation by the world adapter: the trusted guard and the context the bridge sees.
// Facts, traits and state are read for each request and are never part of a capture.
struct BindingCapture
{
    DispatchGuard guard;
    Context context;
};

// LCB3c: the adapter captures a binding afresh once per heartbeat, not on every world tick.
constexpr int64_t BindingHeartbeatMs = WorldLivenessMs;

// LCB3c: a binding is its scope and audience. A fresh capture keeps the binding while the selected world, the bot,
// the conversation (partner, channel, context id; for a party line its group) and the sorted audience are the same.
// Everything else a capture carries (names, the group's other members and leader, map, phase, master) and the facts,
// traits and state of a request are taken afresh and never end the binding, so a talk survives such changes.
bool SameScopeAndAudience(BindingCapture const& bound, BindingCapture const& current);

enum class TerminalAction : uint8_t
{
    Silence,
    Reply,
    Outage
};

TerminalAction DispatchTerminal(WorldOutcome outcome, bool addressedHuman);
char const* DispatchOutageText();

// One physical slot for the entire owned task/completion lifetime, independent of logical reservations. LC.38: counted
// for the drain proof only, with no fixed count; the generation queue's depth bounds the work.
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

// Single world-thread owner. No game pointers, game references, native grants or tools.
// Configured is an explicit notification even when only the bearer changes; no per-tick config polling.
// Selected means module circuit selection only. J4b-X must emit authenticated fresh state to native code.
class WorldPolicy
{
public:
    WorldPolicy() = default;
    void Configured(bool enabled, Settings settings, std::string epoch);
    void Shutdown();
    // Generation intents and effects are synchronous and nothing outlives them (LC.36), so a drained retirement is
    // the whole proof.
    bool ObserveDrain(bool drained);
    bool ObserveScope(WorldScope scope, Clock now);
    void InvalidateScope(uint64_t botGuid, std::string const& contextId);
    void Expire(Clock now);

    WorldTicket Reserve(WorldScope const& scope, Context input, Clock now, int64_t lifetimeMs);
    bool MarkQueued(WorldTicket const& ticket, Clock now);
    bool Complete(WorldTicket const& ticket, WorldOutcome outcome, Clock now);
    // LCB6b: the one answer to "is this conversation current"; the channel asks it once before a packet's read or
    // effect and once after its executor. Checks() counts the calls (a selftest proves one per query packet).
    bool Current(WorldTicket const& ticket, Clock now) const;
    uint64_t Checks() const { return _checks; }
    // A current reservation of exactly this scope is still in flight (reserved, queued or completed, not delivered).
    bool PendingFor(WorldScope const& scope, Clock now) const;
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

    uint64_t _generation = 0;
    uint64_t _sequence = 0;  // Never reset, including reload, rejection, cancellation and queue-full.
    mutable uint64_t _checks = 0;
    bool _enabled = false;
    bool _retiring = false;
    Settings _settings;
    Identity _desired;
    Identity _selected;
    std::unordered_multimap<uint64_t, ScopeEntry> _scopes;  // Keyed by bot: one entry per conversation it is in.
    std::unordered_map<std::string, PendingEntry> _pending;
};
class ChannelAnswer;

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
    Deadline _until;
    std::weak_ptr<ChannelAnswer> _answer;  // Weak: a wait that gave up drops the packet with its answer.

public:
    ChannelError error = ChannelError::None;
    std::string body;
};

// LCB6b: one packet's answer. The listener thread that posted the packet waits on it, with no poll; the world thread
// sets it once, with the result or the word the packet was discarded with (closed, stale, inactive, ...).
class ChannelAnswer
{
public:
    // The first call wins; a later one drops its packet. A null packet is the listener's own stop (inactive).
    void Set(std::unique_ptr<ChannelPacket> packet);
    // Waits until the answer is set or until; true when it is set (packet null: the listener stopped).
    bool Wait(std::chrono::steady_clock::time_point until, std::unique_ptr<ChannelPacket>& packet);

private:
    std::mutex _mutex;
    std::condition_variable _ready;
    bool _set = false;
    std::unique_ptr<ChannelPacket> _packet;
};

// Provider-neutral reverse channel. Receive is I/O-thread safe; all other methods are world-only.
// There is no HTTP, waiting, worker callback or game pointer in these mailboxes. Native adapters bound sockets
// and bytes before Receive; their async write owns the answered packet until disposal, including cancellation.
// LCB6b: every packet Receive takes is answered through its ChannelAnswer, and only WorldPolicy says whether its
// conversation is current.
class QueryWorldSource;

class WorldChannel
{
public:
    using Observer = std::function<bool(WorldTicket const&, DispatchGuard&)>;
    static constexpr std::size_t MaxPackets = 64;
    static constexpr std::size_t TickBudget = 8;
    // A null executor keeps every command on the fixed refusal.
    explicit WorldChannel(WorldPolicy& world, Observer observer, ReadClock clock = Now,
                          QueryWorldSource* source = nullptr, CommandExecutor* executor = nullptr);
    void Configure(bool enabled);
    void Retire();
    bool Activate();
    bool Register(WorldTicket const& ticket, DispatchGuard const& guard);
    // Returns whether the closed route made an authorized effect attempt (Applied or Failed): the DS.14 rule for a
    // silent answer. Nothing else reads the flag, and a second Close reports false. The route leaves the replay
    // ledger with its query and command ids (LC.35); its queued packets are answered closed (LCB6b).
    bool Close(WorldTicket const& ticket);
    // World tick. Typed effects run only with the post-map-join phase, which is used during this call only;
    // without a phase every command gets the fixed refusal.
    std::size_t Update(EffectPhase const* phase = nullptr);
    bool Drained() const { return _used->load() == 0; }
    std::size_t Used() const { return _used->load(); }
    CommandStats const& Commands() const { return _stats; }
    // J6e-c: the bots' watches, armed through Update and scanned by the post-map-join dispatcher; world only.
    WatchTable& Watches() { return _watches; }
    WatchTable const& Watches() const { return _watches; }
    // Unbound, it answers Inactive; the listener checked the bearer before it calls it (LCB6a). A route with no open
    // generation is answered Closed at once, a full mailbox Capacity (overload). None: the world answers later.
    ChannelError Receive(std::string_view route, std::string_view body, std::shared_ptr<ChannelAnswer> const& answer);

private:
    struct Binding
    {
        WorldTicket ticket;
        DispatchGuard guard;
        bool acted = false;  // An authorized effect attempt on this route, its receipt published or not (DS.14).
    };
    // LCB6b: the one check before a read or an effect: the adapter's fresh capture, then WorldPolicy.
    bool Live(Binding const& binding) const;
    // The packet's answer word; its result body in packet.body.
    ChannelError Handle(ChannelPacket& packet, EffectPhase const* phase);
    ChannelError Read(Binding const& binding, ChannelEnvelope const& envelope, ChannelQuery const& query,
                      std::string& body);
    // Forgets an open route under the mutex and answers its queued packets with error.
    void Forget(std::string const& route, ChannelError error);
    static void Answer(std::unique_ptr<ChannelPacket> packet, ChannelError error);
    // Finishes one admitted command exactly once; attempted reports an authorized effect attempt.
    ChannelError Command(Binding const& binding, ChannelEnvelope const& envelope, ChannelCommand const& command,
                         EffectPhase const* phase, Deadline until, bool& attempted, std::string& body);
    // J6e-c: arms, replaces or clears the route bot's watch exactly once; armed reports a new watch (DS.14 acted).
    ChannelError Arm(Binding const& binding, ChannelEnvelope const& envelope, ChannelWatch const& watch,
                     EffectPhase const* phase, bool& armed, std::string& body);
    ChannelError Finish(ChannelEnvelope const& envelope, std::string const& commandId, ChannelOutcome outcome,
                        WorldScope const& scope, Clock now, std::string& body,
                        CommandRefusal refusal = CommandRefusal::None, std::span<ChannelStep const> steps = {},
                        std::span<ChannelStepResult const> ran = {});
    // LCB6b: the one check after the executor.
    bool ReceiptCurrent(WorldTicket const& ticket) const;
    WorldPolicy& _world;
    QueryWorldSource* const _source;   // WORLD ONLY; never copied into a packet or worker task.
    CommandExecutor* const _executor;  // WORLD ONLY; invoked synchronously from Update with a world phase.
    Observer const _observer;          // Invoked only synchronously by the world owner, never by Receive/Take.
    ReadClock const _clock;
    ChannelPolicy _policy;
    CommandRate _rate;  // Survives Retire: a new epoch never reopens a bot's command interval.
    CommandStats _stats;
    WatchTable _watches;  // Retire ends every watch with its epoch.
    bool _enabled = false;
    Identity _selected;
    std::unordered_map<std::string, Binding> _bindings;
    std::shared_ptr<std::atomic<std::size_t>> _used = std::make_shared<std::atomic<std::size_t>>(0);
    mutable std::mutex _mutex;
    bool _bound = false;  // Bound to the selected epoch; guarded by _mutex.
    std::unordered_set<std::string> _open;  // The bound routes, for Receive's closed answer; guarded by _mutex.
    std::deque<std::unique_ptr<ChannelPacket>> _receive;
};
}  // namespace OllamaBridge

#endif
