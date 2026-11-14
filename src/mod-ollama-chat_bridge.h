#ifndef MOD_OLLAMA_CHAT_BRIDGE_H
#define MOD_OLLAMA_CHAT_BRIDGE_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
// Persona card proposal body; equal to OllamaPersona::MaxCardBytes (checked where both are visible).
constexpr std::size_t MaxCardProposalBytes = 8192;
constexpr std::size_t MaxCardFactBytes = 64;  // Each string of `input.personaCard.context`.
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

// DS.12 in-world facts about the bot for a card generation, gathered on world. Empty strings and level 0 are left
// out of the wire; the envelope bounds and sanitizes every string.
struct CardFacts
{
    std::string name;
    std::string race;
    std::string className;
    std::string gender;
    uint32_t level = 0;
    std::string zone;
    std::string faction;
    std::string guild;
    std::string traits;
};

// DS.12/C6: a personality template key (GRUMPY_VETERAN) as a short in-world manner word ("gruff"). Keys with no
// in-world, non-vulgar meaning (GLITCHED_AI, STONER, GAMER ...) and unknown keys give an empty string.
std::string CardTraitWord(std::string const& personalityKey);

// Optional persona card opportunity, set on world only for a ready card store. With an accepted card in the persona
// text it says so; for a bot without a card it carries the facts (bounded per bot and generation), and the bridge
// may return one card for exactly this revision, which the module applies itself. The stamp names the store
// snapshot and never goes on the wire.
struct CardOpportunity
{
    bool offered = false;
    uint64_t revision = 0;  // Canonical decimal on the wire; 0 means no row yet.
    uint64_t stamp = 0;
    bool accepted = false;  // Only when the persona text ends with the accepted card (not when it did not fit).
    CardFacts facts;        // On the wire as `context` exactly when the card is offered and not accepted.
};

// DS.14/J5f: the bot's own state for one request, raw as playerbots read it on world (BridgeBotState). It changes
// constantly, so it is never part of a capture signature, the system prompt or the persona. The envelope filters it
// (the partner only in the bot's group or in its sight, only the bot's own visible victim) and bounds every value;
// strings follow the card-fact rule (MaxCardFactBytes, no controls, valid UTF-8). Absent from the wire unless present.
constexpr uint8_t StatePlanMovement = 1;  // Bits of BotState::plans, in PlanKind order.
constexpr uint8_t StatePlanFormation = 2;
constexpr uint8_t StatePlanSaveMana = 4;
constexpr uint8_t StatePlanFocusHealing = 8;

struct BotState
{
    bool present = false;
    float healthPct = 0.0f;
    uint8_t powerType = 0xFF;  // Game power code: 0 mana, 1 rage, 2 focus, 3 energy, 6 runic power; others omitted.
    uint32_t powerCurrent = 0;
    uint32_t powerMax = 0;
    uint32_t level = 0;
    bool alive = false;
    bool ghost = false;
    bool inCombat = false;
    bool victim = false;  // The bot's own current victim.
    bool victimVisible = false;
    std::string victimName;
    uint32_t victimLevel = 0;
    float victimHealthPct = 0.0f;
    bool flying = false;
    bool casting = false;
    bool looting = false;
    bool resting = false;
    bool travelling = false;
    bool working = false;  // At its travel destination.
    std::string questTitle;
    std::string zone;
    std::string area;
    uint32_t mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool partnerOnline = false;
    bool partnerInGroup = false;
    bool partnerVisible = false;
    bool partnerSameMap = false;  // Yards and position are set only then.
    float partnerYards = 0.0f;
    std::string partnerZone;
    float partnerX = 0.0f;
    float partnerY = 0.0f;
    float partnerZ = 0.0f;
    bool mounted = false;
    bool swimming = false;
    uint32_t groupSize = 0;  // 0 when ungrouped.
    bool leads = false;
    std::string masterName;
    bool follow = false;  // Non-combat movement strategies.
    bool stay = false;
    bool guard = false;
    bool grind = false;
    bool passive = false;
    uint8_t plans = 0;
    uint32_t freeBagSlots = 0;
    uint32_t money = 0;
    // LC10 (LC7, LC8): written after money, each only when not empty. `around` nearest first, one entry per name (a
    // name repeated after the bound is left out), kind one of hostile, service, task, person, object; `deeds` newest
    // first, each a whole sentence of at most MaxStateDeedBytes.
    struct Around
    {
        std::string name;
        std::string kind;
        std::string detail;
        uint32_t count = 0;
    };
    std::vector<Around> around;
    std::string servant;
    std::string mount;
    std::vector<std::string> deeds;
};

// LC10 `input.facts`: the bot's lasting facts (playerbots ReadBotFacts) and who it is, as the wire has them. Absent
// unless the reader succeeded; dropped whole when a required value (who, phrase) does not survive the bounds. Lists
// keep the reader's order and are cut at their wire caps; an empty list is left out.
struct BotFacts
{
    std::string name;
    std::string race;
    std::string className;
    std::string gender;  // male or female
    uint32_t level = 0;
    std::string side;                                           // Alliance or Horde
    std::string phrase;                                         // How it fights; required.
    std::vector<std::pair<std::string, std::string>> crafts;    // Name, rank word; at most 8.
    std::vector<std::string> useful;                            // At most 12.
    std::vector<std::string> servants;                          // At most 8.
    std::vector<std::pair<std::string, std::string>> standing;  // Faction, standing word; at most 6.
    std::string guild;                                          // Empty: none.
    std::vector<std::string> relatives;                         // At most 8.
};

// LC10 `input.traits` (playerbots ReadBotPersonality): every word and interest whole within MaxTraitBytes. Absent
// when both lists are empty.
struct BotTraits
{
    std::vector<std::string> words;
    std::vector<std::string> interests;
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
    BotState state;        // Per request, filled at submit; never in a capture.
    // LC10: per request like state, filled at submit; never in a capture or a signature.
    std::optional<BotFacts> facts;
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
    // DS.14: an action-only answer (wire "silent": true, no text); the module accepts it only after an act.
    bool silent = false;
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
// DS.14/J6e-c: the `input.state` activity kind as a code: 0 idle, 1 flying, 2 fighting, 3 casting, 4 looting,
// 5 resting, 6 questing, 7 travelling (one classifier for the wire and the watches).
uint8_t ActivityCode(BotState const& state);
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
