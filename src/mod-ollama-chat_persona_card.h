#ifndef MOD_OLLAMA_CHAT_PERSONA_CARD_H
#define MOD_OLLAMA_CHAT_PERSONA_CARD_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace OllamaPersona
{
constexpr std::size_t MaxCardBytes = 8192;

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

// One complete row per bot. A confirmed missing row has revision zero and no cards.
// Map absent SQL bodies to empty alternatives and a disengaged accepted card.
struct State
{
    uint64_t botGuid = 0;
    uint64_t revision = 0;
    std::optional<Card> accepted;
    std::string claudeAlternative;
    std::string codexAlternative;
};

enum class UpdateKind : uint8_t
{
    ProposeAlternative,
    AcceptAlternative,
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
    Ineligible,
    Stale,
    MissingAlternative,
    ManualProtected,
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

// The pointer borrows from a validated, loaded state. Copy the body before queuing work.
Card const* SelectAccepted(State const& state, bool acquainted);

// The world owner supplies eligibility, authority, and authenticated proposal provenance.
// AcceptAlternative and SetManual require explicit trusted selection. A provider may only propose.
// Ready proposes a full replacement row. Insert only if absent at revision zero, otherwise compare
// and swap by bot GUID and expected revision. Publish only after the write succeeds.
UpdatePlan PlanUpdate(State const& state, bool acquainted, Update const& update);
}  // namespace OllamaPersona

#endif
