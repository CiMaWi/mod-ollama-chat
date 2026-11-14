#ifndef MOD_OLLAMA_CHAT_BRIDGE_CHANNEL_H
#define MOD_OLLAMA_CHAT_BRIDGE_CHANNEL_H

#include <span>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "mod-ollama-chat_bridge.h"
#include "mod-ollama-chat_bridge_intent.h"
#include "mod-ollama-chat_bridge_watch.h"

namespace OllamaBridge
{
constexpr std::size_t ChannelMaxIds = 4096;
constexpr std::size_t ChannelMaxPending = 64;
constexpr std::size_t ChannelMaxScopes = 64;
constexpr std::size_t ChannelMaxEvents = 32;
constexpr std::size_t ChannelMaxUnits = 40;
constexpr std::size_t ChannelMaxTopicBytes = 8192;  // DS.14 (d): one bot_state topic's entries, encoded.
constexpr std::size_t ChannelMaxTopicName = 64;     // Bytes of one entry's name, cut on a character boundary.
constexpr std::size_t ChannelMaxTopicText = 200;    // LC7: bytes of a record topic entry's sentence, cut the same.

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
    Unknown,
    Standing  // J5e: named invalid_input on the wire, with reason standing_not_supported.
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

// LC3: what one step of a stepped command came to, worded by the executor: a refusal of playerbots' table in its word
// and sentence (the catalogue's), any other reason in DescribeRefusal's. Applied leaves both empty.
struct ChannelStepResult
{
    ChannelOutcome outcome = ChannelOutcome::Rejected;
    std::string reason;
    std::string reasonText;
};

enum class QueryKind : uint8_t
{
    ObservableGroup,
    BotState,  // DS.14 (d), J5f-b: one topic of the bot's own state.
    Catalogue  // LC3: the ability catalogue; plain data, it names no unit.
};

enum class StateTopic : uint8_t
{
    Inventory,
    Quests,
    Cooldowns,
    Strategies,
    Spells,
    Plan,  // WT18b: the planner's topics (append only; the wire index).
    Options,
    Route,
    QuestPlan,
    Bags,
    Places,
    History,  // LC7: the bot's record (playerbots RecordWords.h); each entry carries a sentence.
    Lately,
    Known
};
// The fourteen topics with an entry shape of their own. LC16c: any other topic row of the installed index (playerbots'
// TopicRow past Known, its BotStateTopic value) is answered in the lines form; handler.cpp pins these fourteen values.
constexpr std::size_t StateTopicCount = 14;

char const* StateTopicName(StateTopic topic);
// Most entries a topic result carries; an open topic's is its row's limit (0 for a topic the index lacks).
std::size_t StateTopicLimit(StateTopic topic);
// WT18b: the row word of a planner or record topic's raw code ("" for an unknown code or an older topic). LC16c:
// playerbots gives an open topic's entries no row word, so each reads as the topic's own word.
char const* StateTopicRow(StateTopic topic, uint8_t code);

// LC3: the ability catalogue as plain data, mod-playerbots' one table of acts and topics (PlayerbotBridge::Catalogue)
// with its hash. The game adapter installs it once on the world thread before the channel takes requests; a selftest
// installs a double. Commands and bot_state queries read their verbs and topic names from it and query: catalogue
// answers with it, so the channel keeps no word list of its own.
enum class AbilityField : uint8_t  // PlayerbotBridge::OrderField: the order field an act's parameter fills.
{
    Target,
    Icon,
    PlaceName,
    Position,
    QuestId,
    Shape,
    Enabled,
    ItemName,  // LC16c: an item or a spell by name (Place's text rules), a choice by its word, a count 1..1000.
    SpellName,
    Choice,
    Count
};

struct AbilityParam
{
    std::string name;
    std::string kind;  // unit, place, item, spell, quest, choice, flag or count
    std::string sentence;
    std::vector<std::string> choices;
    AbilityField field = AbilityField::Target;  // An act's; a topic's argument leaves it unread.
};

struct AbilityRefusal
{
    std::string word;
    std::string sentence;
};

// One catalogue row, its parts in PlayerbotBridge::AbilityData's order.
struct AbilityRow
{
    bool topic = false;
    uint8_t code = 0;  // An act's playerbots Intent value (a ChannelIntent below 22), a topic's BotStateTopic value.
    std::string name;
    std::string sentence;
    std::vector<AbilityParam> params;
    std::vector<AbilityRefusal> refusals;
    std::string effect;  // The word of the lasting part the act changes.
    std::string done;    // The word of when it is done.
    std::string state;
    std::string event;
    std::size_t limit = 0;
};

// A verb of the old command form: every ChannelIntent's word, the acts' and those of the four without a row.
struct AbilityVerb
{
    std::string name;
    ChannelIntent intent = ChannelIntent::Stay;
};

struct AbilityIndex
{
    uint64_t hash = 0;             // The catalogue text's FNV-1a 64; the wire sends it as 16 lowercase hex digits.
    uint32_t version = 0;          // The catalogue text's format.
    std::size_t maxSteps = 0;      // The most steps one order takes.
    std::vector<AbilityRow> rows;  // The acts, then the topics, in the table's order.
    std::vector<AbilityVerb> verbs;
};

// The bytes the encoded catalogue may take (the hub's MAX_CATALOGUE_BYTES); the bridge reads it whole, its answer
// within MaxCatalogueResponseBytes.
constexpr std::size_t ChannelMaxCatalogueBytes = 49152;

// Installs the index and returns the bytes its catalogue encodes to (0 when the index is refused or its text is not
// UTF-8). An index with a missing, duplicate or unknown verb, a duplicate row word or code, an act below 22 that is not
// its verb, one past it named like a verb, a parameter field the channel does not fill or a topic with more than one
// parameter is refused: the channel then holds an empty index (every verb and topic unknown, no catalogue). A catalogue
// that is not UTF-8 or over ChannelMaxCatalogueBytes drops only the catalogue answer: the verbs and topics stand, so
// the old wire keeps working (LC.23). World thread only, once at startup.
std::size_t SetAbilityIndex(AbilityIndex index);
AbilityIndex const& Abilities();
bool CatalogueInstalled();  // Whether a catalogue query has an answer.

// One raw entry of a bot_state topic, as the game read it; FinishTopic filters, bounds and names the keys.
//   inventory:  name, kind "equipped"/"bag", first = item quality 0..7, second = stack count
//   quests:     name = title, kind "active"/"complete"/"failed", first = objectives done, second = needed
//   cooldowns:  name, first = seconds left
//   strategies: name (whitelisted), kind "combat"/"non_combat"
//   spells:     name, first = rank (0 = none)
//   plan, options, route, quest_plan, bags, places (WT18b): kind = the row word (StateTopicRow), name (words, or a
//               quest title or bind area), first = value, second = extra (0..4294967295 each)
//   history, lately, known (LC7): the planner shape and text = one sentence (1..ChannelMaxTopicText bytes)
//   any other topic of the index (LC16c): history's shape, kind = any wire word (StateTopicRow gives the topic's)
struct StateEntry
{
    std::string name;
    std::string kind;
    int64_t first = 0;
    int64_t second = 0;
    std::string text = {};  // A default, so older brace inits stay complete.
};

struct ChannelQuery
{
    uint64_t controllerGuid = 0;
    uint64_t botGuid = 0;
    QueryKind kind = QueryKind::ObservableGroup;
    StateTopic topic = StateTopic::Inventory;  // Only for QueryKind::BotState.
    std::string place;                         // WT18b: route only, the place the route goes to.
};
struct ChannelText
{
    std::string text;
};
struct ChannelAck
{
};
// J6e-c: arm, replace or clear the route bot's watch; its id shares the command replay namespace.
struct ChannelWatch
{
    std::string commandId;
    uint64_t botGuid = 0;
    WatchCondition condition = WatchCondition::None;
    int64_t untilUnixMs = 0;
};
using ChannelPayload =
    std::variant<ChannelQuery, ChannelCommand, ChannelText, ChannelAck, ChannelFailure, ChannelWatch>;

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
    // current and expectedRequestId come from the trusted game route, NEVER from this body.
    // Non-query ids must match that route before state changes; queries carry independent ids.
    ChannelError Receive(std::string const& localBearer, std::string const& body, WorldScope const& current, Clock now,
                         ChannelMessage& message, std::string const& expectedRequestId);
    bool Current(ChannelEnvelope const& envelope, WorldScope const& current, Clock now, bool requireOpen = true) const;
    ChannelError FinishQuery(ChannelEnvelope const& envelope, WorldScope const& current,
                             std::vector<Participant> const& observedUnits, Clock now, std::string& body);
    // J5f-b: a bot_state result (`topic`, `entries`); a query id still never authorizes a command.
    ChannelError FinishTopic(ChannelEnvelope const& envelope, WorldScope const& current, StateTopic topic,
                             std::vector<StateEntry> const& entries, Clock now, std::string& body);
    // LC3: a catalogue result (`catalogue`: hash, version, abilities, maxSteps, rows), the installed index's.
    ChannelError FinishCatalogue(ChannelEnvelope const& envelope, WorldScope const& current, Clock now,
                                 std::string& body);
    ChannelError FinishFailure(ChannelEnvelope const& envelope, ChannelFailure failure, WorldScope const& current,
                               Clock now, std::string& body);
    // J5e: a rejected or failed outcome carries `reason` and `reasonText` (None words as cannot_now or failed).
    // LC3: a stepped command (steps, as submitted) also carries `steps`, one entry per step: the ones that ran (ran,
    // each but the last applied) with their outcome, the others skipped. Its outcome is applied only when every step
    // applied, and a step that ran and ended the order gives the order its reason. The old form has no `steps`.
    ChannelError FinishCommand(ChannelEnvelope const& envelope, std::string const& commandId, ChannelOutcome outcome,
                               WorldScope const& current, Clock now, std::string& body,
                               CommandRefusal refusal = CommandRefusal::None, std::span<ChannelStep const> steps = {},
                               std::span<ChannelStepResult const> ran = {});
    // J6e-c: `watch.result` with outcome armed, cleared or rejected (rejected also `reason` and `reasonText`).
    ChannelError FinishWatch(ChannelEnvelope const& envelope, std::string const& commandId, WatchOutcome outcome,
                             WorldScope const& current, Clock now, std::string& body,
                             CommandRefusal refusal = CommandRefusal::None);
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
    Clock _observed;  // Trusted clock high-water marks: heartbeat expiry cannot be undone by clock rollback.
    std::string _bearer;
    // Bounded lifetime epoch history also prevents accidental reconnect with a retired identity.
    std::unordered_set<std::string> _epochs;
    std::vector<ScopeEntry> _scopes;
    std::unordered_map<std::string, RequestEntry> _requests;
    std::unordered_map<std::string, CommandEntry> _commands;
};
}  // namespace OllamaBridge

#endif
