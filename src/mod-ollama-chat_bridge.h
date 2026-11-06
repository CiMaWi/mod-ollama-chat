#ifndef MOD_OLLAMA_CHAT_BRIDGE_H
#define MOD_OLLAMA_CHAT_BRIDGE_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "mod-ollama-chat_httpclient.h"

// Plain owned values only. The world owner supplies identity, permitted observations,
// replay reservations and authority; this helper never obtains them from game objects.
namespace OllamaBridge
{
constexpr std::size_t MaxRequestBytes = 65536;
constexpr std::size_t MaxResponseBytes = 32768;
constexpr std::size_t MaxTextBytes = 4096;
// Persona card proposal body; equal to OllamaPersona::MaxCardBytes (checked where both are visible).
constexpr std::size_t MaxCardProposalBytes = 8192;
constexpr int64_t MaxLifetimeMs = 120000;
constexpr int64_t CircuitCooldownMs = 5000;

struct Settings
{
    std::string instance;
    std::string provider;
    std::string endpoint;
    std::string token;  // Local game-link bearer only; never native subscription credentials.
    bool observerEnabled = false;
    std::string observerTokenRef;  // Absolute protected observer.json path; resolved only on config load.
    std::string observerToken;     // Owned local observer bearer, distinct from the game-link bearer.
};

struct Clock
{
    int64_t unixMs = 0;
    int64_t steadyMs = 0;
};

struct Deadline
{
    int64_t unixMs = 0;
    int64_t steadyMs = 0;
};

struct Participant
{
    uint64_t guid = 0;
    std::string name;
    std::string role;            // Empty when the participant cannot observe a role.
    int32_t healthPercent = -1;  // -1 omits unobserved health.
    uint64_t visibleTargetGuid = 0;
};

// Optional persona card opportunity, set on world only for the eligible pair of a ready card store. The bridge may
// then return one proposal for exactly this revision. The stamp names the store snapshot and never goes on the wire.
struct CardOpportunity
{
    bool offered = false;
    uint64_t revision = 0;  // Canonical decimal on the wire; 0 means no row yet.
    uint64_t stamp = 0;
    bool accepted = false;  // Only when the persona text ends with the accepted card (not when it did not fit).
};

struct Context
{
    uint64_t speakerGuid = 0;
    std::string channel;
    std::string text;
    std::string systemPrompt;
    std::string persona;
    std::string memory;
    std::string groupScope;
    bool directAddress = false;
    // Descriptive data, never an execution grant. World guards remain authoritative.
    bool speakerControlsBot = false;
    std::vector<std::string> observations;
    std::vector<Participant> party;
    CardOpportunity card;  // Absent from the wire unless offered.
};

// Exact captured game binding; owned values only, never a grant supplied by the model.
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

struct Request
{
    std::string epoch;
    std::string requestId;
    Deadline deadline;
    uint64_t controllerGuid = 0;
    uint64_t botGuid = 0;
    Context input;
    WorldScope scope;  // Required only by the opt-in Codex observer protocol.
};

struct Identity
{
    std::string instance;
    std::string provider;
    std::string endpoint;
    std::string epoch;

    bool operator==(Identity const& other) const;
};

struct Result
{
    bool ok = false;
    std::string text;
    std::string error;  // Fixed reason only; never remote text, credentials or JSON exceptions.
    int status = 0;
    // Validated card proposal body for the offered revision; never part of text. Staging it is a separate world step.
    std::optional<std::string> personaProposal;
};

bool ValidIdentifier(std::string const& value);
bool ValidEndpoint(std::string const& value);
bool ValidSettings(Settings const& settings);
// Windows-only, matching the native service. Refusal always clears the cached secret.
bool LoadObserverToken(Settings& settings);
bool ValidObserverSettings(Settings const& settings);
Clock Now();
Deadline CaptureDeadline(Clock now, int64_t lifetimeMs);
bool Expired(Deadline deadline, Clock now);
int64_t RemainingMs(Deadline deadline, Clock now);
Identity MakeIdentity(Settings const& settings, Request const& request);
OllamaHttpHeaders Headers(Settings const& settings);
bool Build(Settings const& settings, Request const& request, Clock now, std::string& body);
Result Parse(Settings const& settings, Request const& request, std::string const& body, Clock now);

struct ObserverChallenge
{
    std::string serviceId;
    std::string incarnation;
    std::string challenge;
    uint32_t sequence = 0;
};

bool BuildObserverOpen(Settings const& settings, std::string& body);
bool BuildObserverSnapshot(Settings const& settings, ObserverChallenge const& challenge, std::string const& epoch,
                           bool active, bool worldDrained, std::vector<WorldScope> const& scopes, std::string& body);
bool ParseObserverChallenge(Settings const& settings, std::string const& body, ObserverChallenge const* previous,
                            ObserverChallenge& challenge);

// Reset is called ONLY by the trusted world activation owner after its retirement
// obligations are met. It does not acknowledge CLI intent, drain work or restore strategies.
// Tickets prevent late failures/successes from changing a newer activation or recovery.
class Circuit
{
public:
    struct Ticket
    {
        uint64_t revision = 0;
        bool admitted = false;
        bool recovery = false;
    };

    void Reset(Identity identity);
    Ticket Acquire(Identity const& identity, int64_t steadyMs);
    void Complete(Ticket ticket, bool success, int64_t steadyMs);

private:
    std::mutex _mutex;
    Identity _identity;
    uint64_t _revision = 0;
    int64_t _retryAtMs = 0;
    bool _open = false;
    bool _recovering = false;
};

// Injection is local to this call, never a callback captured in a queued task.
using ReadClock = std::function<Clock()>;
using Transport = std::function<OllamaHttpResult(Settings const&, std::string const&, int64_t)>;
Result Generate(Settings const& settings, Request const& request, Circuit& circuit, ReadClock const& readClock,
                Transport const& transport);
}  // namespace OllamaBridge

#endif
