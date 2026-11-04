#ifndef MOD_OLLAMA_CHAT_BRIDGE_WORLD_H
#define MOD_OLLAMA_CHAT_BRIDGE_WORLD_H

#include <atomic>
#include <unordered_map>

#include "mod-ollama-chat_bridge.h"

namespace OllamaBridge
{
constexpr std::size_t MaxWorldPending = 64;
constexpr std::size_t MaxWorldScopes = 64;
constexpr uint64_t MaxEpochRequests = 4096;
constexpr int64_t WorldLivenessMs = 5000;

// Complete trusted-world observation. GUIDs and revisions become decimal/string wire values in J4b-X.
// This is an emitter contract, not an implemented/authenticated native transport.
struct WorldScope
{
    uint32_t version = 1;
    std::string instance;
    std::string provider;
    std::string epoch;
    uint64_t botGuid = 0;
    uint64_t controllerGuid = 0;
    std::string contextId;
    std::string channel;  // direct or party
    std::string perceptionScope;
    std::string contextRevision;
    std::string membershipRevision;
    std::string personaRevision;

    bool operator==(WorldScope const& other) const;
};

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

// Single world-thread owner. No pointers, game references, callbacks, native acknowledgements or tools.
// Configured is an explicit notification even when only the bearer changes; no per-tick config polling.
// Selected means module circuit selection only. J4b-X must emit authenticated fresh state to native code.
class WorldPolicy
{
public:
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
}  // namespace OllamaBridge

#endif
