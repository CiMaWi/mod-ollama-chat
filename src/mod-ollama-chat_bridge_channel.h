#ifndef MOD_OLLAMA_CHAT_BRIDGE_CHANNEL_H
#define MOD_OLLAMA_CHAT_BRIDGE_CHANNEL_H

#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "mod-ollama-chat_bridge.h"

namespace OllamaBridge
{
constexpr std::size_t ChannelMaxIds = 4096;
constexpr std::size_t ChannelMaxPending = 64;
constexpr std::size_t ChannelMaxScopes = 64;
constexpr std::size_t ChannelMaxEvents = 32;
constexpr std::size_t ChannelMaxUnits = 40;

enum class ChannelError : uint8_t
{
    None,
    Unauthorized,
    Inactive,
    Invalid,
    TooLarge,
    Stale,
    Duplicate,
    Capacity,
    Closed,
    Unknown
};

char const* ChannelErrorName(ChannelError error);

enum class ChannelWork : uint8_t
{
    Generation,
    Events,
    Query
};

struct ChannelEnvelope
{
    std::string instance;
    std::string provider;
    std::string epoch;
    std::string requestId;
    int64_t deadlineUnixMs = 0;
};

enum class ChannelIntent : uint8_t
{
    Follow,
    Stay,
    Flee,
    Regroup,
    AttackOwnTarget,
    Pull,
    Mark,
    FocusHealing,
    AssignRole,
    SaveMana,
    Formation,
    Position,
    DungeonClear
};

enum class ChannelRole : uint8_t
{
    Tank,
    Healer,
    Damage
};
enum class ChannelShape : uint8_t
{
    Close,
    Spread,
    Line
};
enum class ChannelMode : uint8_t
{
    On,
    Off,
    Pause
};
enum class ChannelFailure : uint8_t
{
    Unavailable,
    Timeout,
    InvalidInput,
    Cancelled
};
enum class ChannelOutcome : uint8_t
{
    Applied,
    Rejected,
    Failed
};

struct ChannelTarget
{
    uint64_t guid = 0;
};
struct ChannelMark
{
    uint64_t guid = 0;
    uint8_t icon = 0;
};
struct ChannelPosition
{
    double x = 0;
    double y = 0;
    double z = 0;
    uint32_t mapId = 0;
};
using ChannelParams = std::variant<std::monostate, ChannelTarget, ChannelMark, ChannelRole, bool, ChannelShape,
                                   ChannelPosition, ChannelMode>;

struct ChannelCommand
{
    std::string commandId;
    uint64_t issuerGuid = 0;
    uint64_t botGuid = 0;
    ChannelIntent intent = ChannelIntent::Stay;
    ChannelParams params;
};

struct ChannelQuery
{
    uint64_t controllerGuid = 0;
    uint64_t botGuid = 0;
};
struct ChannelText
{
    std::string text;
};
struct ChannelAck
{
};
using ChannelPayload = std::variant<ChannelQuery, ChannelCommand, ChannelText, ChannelAck, ChannelFailure>;

// Owned values only. Successful decode is staging, never an effect-execution grant.
struct ChannelMessage
{
    ChannelEnvelope envelope;
    ChannelPayload payload;
};

struct ChannelObservedMark
{
    uint64_t targetGuid = 0;
    uint8_t icon = 0;
};

// World-thread owner; no mutex, game objects, transport, queue or activation acknowledgement.
// J5b must bind only the actual selected/drained game epoch, authenticate EVERY transport operation,
// own physical payload slots through disposal and revalidate live world authority before publication.
class ChannelPolicy
{
public:
    explicit ChannelPolicy(std::size_t maxIds = ChannelMaxIds, std::size_t maxPending = ChannelMaxPending);
    ChannelPolicy(ChannelPolicy const&) = delete;
    ChannelPolicy& operator=(ChannelPolicy const&) = delete;

    bool Bind(Identity identity, std::string localBearer);
    void Retire();
    bool ObserveScope(WorldScope scope, Clock now);
    void RemoveScope(uint64_t botGuid, std::string const& contextId);
    void Expire(Clock now);
    ChannelError OpenGeneration(WorldScope const& scope, std::string requestId, Deadline deadline, Clock now);
    ChannelError OpenEvents(WorldScope const& scope, std::string requestId, Deadline deadline,
                            std::vector<ChannelObservedMark> const& events, Clock now, std::string& body);

    // Authentication is checked before size/JSON parsing and before any replay/state mutation.
    // current is a trusted fresh game capture, NEVER a scope deserialized from this body.
    ChannelError Receive(std::string const& localBearer, std::string const& body, WorldScope const& current, Clock now,
                         ChannelMessage& message);
    bool Current(ChannelEnvelope const& envelope, WorldScope const& current, Clock now, bool requireOpen = true) const;
    ChannelError FinishQuery(ChannelEnvelope const& envelope, WorldScope const& current,
                             std::vector<Participant> const& observedUnits, Clock now, std::string& body);
    ChannelError FinishFailure(ChannelEnvelope const& envelope, ChannelFailure failure, WorldScope const& current,
                               Clock now, std::string& body);
    ChannelError FinishCommand(ChannelEnvelope const& envelope, std::string const& commandId, ChannelOutcome outcome,
                               WorldScope const& current, Clock now, std::string& body);
    std::size_t RequestReservations() const { return _requests.size(); }
    std::size_t CommandReservations() const { return _commands.size(); }
    std::size_t Pending() const;

private:
    struct ScopeEntry
    {
        WorldScope scope;
        Deadline until;
    };
    struct RequestEntry
    {
        ChannelEnvelope envelope;
        WorldScope scope;
        Deadline until;
        ChannelWork work = ChannelWork::Generation;
        bool terminal = false;
        bool invalid = false;
    };
    struct CommandEntry
    {
        std::string requestId;
        bool terminal = false;
    };
    bool Authenticated(std::string const& bearer) const;
    bool ScopeCurrent(WorldScope const& scope, Clock now) const;
    ChannelError Open(WorldScope const& scope, std::string requestId, Deadline deadline, ChannelWork work, Clock now);
    bool const _validLimits;
    std::size_t const _maxIds;
    std::size_t const _maxPending;
    Identity _identity;
    std::string _bearer;
    // Bounded lifetime epoch history also prevents accidental reconnect with a retired identity.
    std::unordered_set<std::string> _epochs;
    std::vector<ScopeEntry> _scopes;
    std::unordered_map<std::string, RequestEntry> _requests;
    std::unordered_map<std::string, CommandEntry> _commands;
};
}  // namespace OllamaBridge

#endif
