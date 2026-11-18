#ifndef MOD_OLLAMA_CHAT_BRIDGE_H
#define MOD_OLLAMA_CHAT_BRIDGE_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "BridgeBotFacts.h"
#include "BridgeBotState.h"
#include "mod-ollama-chat_httpclient.h"

// Plain owned values only. The world owner supplies identity, permitted observations,
// replay reservations and authority; this helper never obtains them from game objects.
namespace OllamaBridge
{
constexpr std::size_t MaxRequestBytes = 65536;
constexpr std::size_t MaxResponseBytes = 32768;
// LC16c: the catalogue answer's bound, the hub link's MAX_REPLY_BYTES; every other body keeps MaxResponseBytes.
constexpr std::size_t MaxCatalogueResponseBytes = 65536;
constexpr std::size_t MaxTextBytes = 4096;
// The fact rule: each fact string in `input.facts` and `input.state` is at most this long.
constexpr std::size_t MaxFactBytes = 64;
// LC10: a recent deed in `input.state.deeds` is one whole record sentence, the cap LC7 carries across the wire
// (playerbots RecordWords::MAX_SENTENCE, ChannelMaxTopicText, the bridge's MAX_STATE_DEED_BYTES); never the 64-byte
// rule.
constexpr std::size_t MaxStateDeedBytes = 200;
constexpr std::size_t MaxStateDeeds = 3;
constexpr std::size_t MaxStateAround = 12;
constexpr std::size_t MaxPhraseBytes = 96;    // `input.facts.phrase`.
constexpr std::size_t MaxFactWordBytes = 32;  // A craft's rank word, a standing word.
constexpr std::size_t MaxTraitBytes = 1024;   // Each word and interest of `input.traits`, never cut at 64.
constexpr std::size_t MaxTraitWords = 32;
constexpr std::size_t MaxTraitInterests = 3;
constexpr int64_t MaxLifetimeMs = 120000;
constexpr int64_t CircuitCooldownMs = 5000;

struct Settings
{
    std::string instance;
    std::string provider;
    std::string endpoint;
    std::string token;  // Local game-link bearer only; never native subscription credentials.
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

// DS.14/J5f: the bot's own state for one request, raw as playerbots read it on world (PlayerbotBridge::BotStateFacts,
// BridgeBotState.h). It changes constantly, so it is never part of a capture. The envelope filters it
// (the partner only in the bot's group or in its sight, only the bot's own visible victim) and bounds every value;
// strings follow the fact rule (MaxFactBytes, no controls, valid UTF-8). Absent from the wire unless read.
// LCB4: playerbots' open rows (StateKey) go to the wire as they are, each under the open-key rule: a key of
// [a-z][a-z_]{0,23} the envelope does not write itself, at most MaxOpenKeys per object, text by the fact rule, whole
// numbers clamped to +-MaxOpenNumber.
constexpr std::size_t MaxOpenKeys = 8;
constexpr int64_t MaxOpenNumber = 1000000000;
constexpr uint8_t StatePlanMovement = 1;  // Bits of Context::statePlans, in PlanKind order.
constexpr uint8_t StatePlanFormation = 2;
constexpr uint8_t StatePlanSaveMana = 4;
constexpr uint8_t StatePlanFocusHealing = 8;

// LC10 `input.facts.who`: who the bot is, from the bot's own getters; the rest of `input.facts` are playerbots' lasting
// facts (PlayerbotBridge::BotFacts, ReadBotFacts). The whole object is dropped when a value of who or the phrase does
// not survive the bounds; lists keep the reader's order and are cut at their wire caps, an empty list is left out.
struct BotWho
{
    std::string name;
    std::string race;
    std::string className;
    std::string gender;  // male or female
    uint32_t level = 0;
    uint8_t team = 2;  // The game's TeamId: 0 Alliance, 1 Horde; any other leaves the facts out.
};

// LC10 `input.traits` (playerbots ReadBotPersonality): every word and interest whole within MaxTraitBytes. Absent
// when both lists are empty.
struct BotTraits
{
    std::vector<std::string> words;
    std::vector<std::string> areas;  // LC15c: one per word, same order; sent only when it is as long as words.
    std::vector<std::string> interests;
};

struct Context
{
    uint64_t speakerGuid = 0;
    std::string channel;
    std::string text;
    std::string memory;
    std::string groupScope;
    bool directAddress = false;
    // Descriptive data, never an execution grant. World guards remain authoritative.
    bool speakerControlsBot = false;
    std::vector<std::string> observations;
    std::vector<Participant> party;
    std::optional<PlayerbotBridge::BotStateFacts> state;  // Per request, filled at submit; never in a capture.
    uint8_t statePlans = 0;  // StatePlan* bits of the bot's live bridge plans, read with the state.
    // LC10: per request like state, filled at submit; never in a capture.
    std::optional<PlayerbotBridge::BotFacts> facts;
    BotWho who;  // Set with facts.
    BotTraits traits;
    // J6e-c: a wake request carries its condition and watch id as `input.wake`; depth and origin stay in the module
    // (a watch armed on this route opens wake depth + 1 and may not outlast origin + 10 minutes).
    std::string wakeCondition;  // Empty unless this request is a wake.
    std::string wakeWatchId;
    uint8_t wakeDepth = 0;
    int64_t originUnixMs = 0;  // The player's line that started this conversation's chain.
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
    WorldScope scope;  // The scope the request was reserved for.
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
    // DS.14: an action-only answer (wire "silent": true, no text); the module accepts it only after an act.
    bool silent = false;
    std::string error;  // Fixed reason only; never remote text, credentials or JSON exceptions.
    int status = 0;
};

bool ValidIdentifier(std::string const& value);
bool ValidEndpoint(std::string const& value);
bool ValidSettings(Settings const& settings);
// The module generates only through the bridge: enabled without valid settings, it is idle (no listener, no
// workers).
bool BridgeUsable(bool enabled, Settings const& settings);
Clock Now();
Deadline CaptureDeadline(Clock now, int64_t lifetimeMs);
bool Expired(Deadline deadline, Clock now);
int64_t RemainingMs(Deadline deadline, Clock now);
Identity MakeIdentity(Settings const& settings, Request const& request);
OllamaHttpHeaders Headers(Settings const& settings);
bool Build(Settings const& settings, Request const& request, Clock now, std::string& body);
// DS.14/J6e-c: the `input.state` activity kind as a code: 0 idle, 1 flying, 2 fighting, 3 casting, 4 looting,
// 5 resting, 6 questing, 7 travelling (one classifier for the wire and the watches).
uint8_t ActivityCode(PlayerbotBridge::BotStateFacts const& state);
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
