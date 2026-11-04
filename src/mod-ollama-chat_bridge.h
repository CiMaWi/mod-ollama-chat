#ifndef MOD_OLLAMA_CHAT_BRIDGE_H
#define MOD_OLLAMA_CHAT_BRIDGE_H

#include <cstdint>
#include <functional>
#include <mutex>
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
};

struct Request
{
    std::string epoch;
    std::string requestId;
    Deadline deadline;
    uint64_t controllerGuid = 0;
    uint64_t botGuid = 0;
    Context input;
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
