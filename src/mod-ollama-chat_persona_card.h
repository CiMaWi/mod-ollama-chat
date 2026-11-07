#ifndef MOD_OLLAMA_CHAT_PERSONA_CARD_H
#define MOD_OLLAMA_CHAT_PERSONA_CARD_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace OllamaPersona
{
constexpr std::size_t MaxCardBytes = 8192;
// DS.12: a generated card is short enough that the voice and the card fit MaxCardBytes together.
constexpr std::size_t MaxGeneratedCardBytes = 2048;

// Persistent values. Zero is reserved for an absent accepted card in SQL.
enum class Source : uint8_t
{
    Manual = 1,
    Claude = 2,
    Codex = 3
};

struct Card
{
    Source source = Source::Manual;
    std::string body;
};

// One complete row per bot. A confirmed missing row has revision zero and no card. A nonzero revision may hold no
// card: rows from before J14e kept provider drafts, which are no longer read (their columns stay, written empty).
struct State
{
    uint64_t botGuid = 0;
    uint64_t revision = 0;
    std::optional<Card> accepted;
};

enum class UpdateKind : uint8_t
{
    ApplyGenerated,  // DS.12: a provider's card is applied directly, only to a bot without a card.
    SetManual
};

struct Update
{
    UpdateKind kind;
    uint64_t botGuid;
    uint64_t expectedRevision;
    Source source;
    std::string body;
};

enum class UpdateStatus : uint8_t
{
    Invalid,
    Stale,
    ManualProtected,
    HasCard,
    Unchanged,
    RevisionExhausted,
    Ready
};

struct UpdatePlan
{
    UpdateStatus status;
    std::optional<State> next;
};

// Validate at cache ingress. A failed or pending load is distinct from a confirmed missing row.
bool ValidState(State const& state);

// The world owner supplies authority and authenticated provenance. ApplyGenerated takes a Claude or Codex body that
// passes the card rules (nonblank well-formed UTF-8, no control characters but tab and line breaks, at most
// MaxGeneratedCardBytes) for a bot without a card; a Manual card is never replaced by a provider. SetManual is the
// owner's edit. Ready proposes a full replacement row. Insert only if absent at revision zero, otherwise compare and
// swap by bot GUID and expected revision. Publish only after the write succeeds.
UpdatePlan PlanUpdate(State const& state, Update const& update);
}  // namespace OllamaPersona

#endif
