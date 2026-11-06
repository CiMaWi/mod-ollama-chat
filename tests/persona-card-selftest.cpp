// Offline proof of the production persona policy. No game objects or database connections.
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

#include "mod-ollama-chat_persona_card.h"

namespace
{
using namespace OllamaPersona;
constexpr uint64_t BotGuid = 9007199254740993ULL;
unsigned checks = 0;

void Check(bool condition, char const* label)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(label);
}

void Refused(UpdatePlan const& plan, UpdateStatus expected, char const* label)
{
    Check(plan.status == expected && !plan.next, label);
}

State Apply(State const& state, Update const& update)
{
    UpdatePlan plan = PlanUpdate(state, true, update);
    Check(plan.status == UpdateStatus::Ready && plan.next && ValidState(*plan.next) &&
              plan.next->revision == state.revision + 1 && plan.next->botGuid == state.botGuid,
          "write plans produce a valid, identity-preserving next row");
    return std::move(*plan.next);
}

void MissingAndProviderSwitch()
{
    State const missing{BotGuid, 0, {}, {}, {}};
    Check(ValidState(missing) && !SelectAccepted(missing, true), "confirmed missing card is explicit");
    Update claude{UpdateKind::ProposeAlternative, BotGuid, 0, Source::Claude, "A traveling blacksmith"};
    Refused(PlanUpdate(missing, false, claude), UpdateStatus::Ineligible, "unknown acquaintance cannot write");
    Refused(PlanUpdate(missing, true, {UpdateKind::AcceptAlternative, BotGuid, 0, Source::Claude, {}}),
            UpdateStatus::MissingAlternative, "promotion requires a stored draft");

    State const first = Apply(missing, claude);
    Check(first.claudeAlternative == claude.body && first.codexAlternative.empty() && !SelectAccepted(first, true),
          "first provider draft stays separate from the accepted card");
    Refused(PlanUpdate(first, true, claude), UpdateStatus::Stale, "retry from the missing revision is stale");
    claude.expectedRevision = 1;
    Refused(PlanUpdate(first, true, claude), UpdateStatus::Unchanged, "same draft does not churn revisions");

    Update const promote{UpdateKind::AcceptAlternative, BotGuid, 1, Source::Claude, {}};
    State const accepted = Apply(first, promote);
    Card const* card = SelectAccepted(accepted, true);
    Check(card && card->source == Source::Claude && card->body == claude.body, "accepted card retains provenance");
    Check(!SelectAccepted(accepted, false), "unknown acquaintance cannot read");

    Update const codex{UpdateKind::ProposeAlternative, BotGuid, 2, Source::Codex, "A retired sea captain"};
    State const switched = Apply(accepted, codex);
    Check(switched.accepted->source == Source::Claude && switched.accepted->body == claude.body &&
              switched.claudeAlternative == claude.body && switched.codexAlternative == codex.body,
          "switching provider preserves the accepted identity and the other provider's draft");
    Refused(PlanUpdate(switched, true, promote), UpdateStatus::Stale, "older selection cannot replace a newer row");
    Refused(PlanUpdate(switched, true, {UpdateKind::AcceptAlternative, BotGuid, 3, Source::Claude, {}}),
            UpdateStatus::Unchanged, "accepting the same card does not churn revisions");

    State const refreshed = Apply(switched, {UpdateKind::ProposeAlternative, BotGuid, 3, Source::Claude, "New draft"});
    Check(refreshed.accepted->body == claude.body && refreshed.claudeAlternative == "New draft",
          "refreshing the accepted provider's draft preserves the accepted copy");
    State const selected = Apply(refreshed, {UpdateKind::AcceptAlternative, BotGuid, 4, Source::Codex, {}});
    Check(selected.accepted->source == Source::Codex && selected.accepted->body == codex.body,
          "a current explicit selection can change a generated accepted card");
}

void ManualAndConflicts()
{
    State const loaded{BotGuid, 24, Card{Source::Manual, "A manually curated identity"}, "Claude draft", "Codex draft"};
    Check(ValidState(loaded) && SelectAccepted(loaded, true)->source == Source::Manual,
          "load a persistent manual card");
    Update const claude{UpdateKind::ProposeAlternative, BotGuid, 24, Source::Claude, "Revised Claude draft"};
    Update const codex{UpdateKind::ProposeAlternative, BotGuid, 24, Source::Codex, "Revised Codex draft"};
    State const changed = Apply(loaded, claude);
    Refused(PlanUpdate(changed, true, codex), UpdateStatus::Stale, "concurrent updates share one revision");
    Check(changed.accepted->body == loaded.accepted->body && changed.codexAlternative == loaded.codexAlternative,
          "draft update preserves the manual identity and unrelated draft");
    Refused(PlanUpdate(changed, true, {UpdateKind::AcceptAlternative, BotGuid, 25, Source::Claude, {}}),
            UpdateStatus::ManualProtected, "provider draft cannot displace a manual card");
    Refused(PlanUpdate(changed, true, {UpdateKind::SetManual, BotGuid, 25, Source::Manual, loaded.accepted->body}),
            UpdateStatus::Unchanged, "same manual card does not churn revisions");
    State const edited = Apply(changed, {UpdateKind::SetManual, BotGuid, 25, Source::Manual, "Edited by the owner"});
    Check(edited.accepted->source == Source::Manual && edited.accepted->body == "Edited by the owner" &&
              edited.claudeAlternative == claude.body && edited.codexAlternative == loaded.codexAlternative,
          "explicit manual edit preserves provider drafts");
}

void InvalidInputsAndLimits()
{
    State const missing{BotGuid, 0, {}, {}, {}};
    Update update{UpdateKind::ProposeAlternative, BotGuid, 0, Source::Codex, {}};
    for (std::string const& body :
         {std::string{}, std::string(" \t\r\n"), std::string("a\0b", 3), std::string("a\x1b"), std::string("\xc0\xaf"),
          std::string("\xed\xa0\x80"), std::string("\xf0\x9f\x8c"), std::string(MaxCardBytes + 1, 'a')})
    {
        update.body = body;
        Refused(PlanUpdate(missing, true, update), UpdateStatus::Invalid, "malformed or overbound body");
    }
    update.body = std::string(MaxCardBytes - 4, 'a') + "\xf0\x9f\x8c\xb1";
    Check(Apply(missing, update).codexAlternative.size() == MaxCardBytes, "exact UTF-8 byte ceiling");

    Refused(PlanUpdate(missing, true, {UpdateKind::ProposeAlternative, BotGuid + 1, 0, Source::Codex, "Wrong bot"}),
            UpdateStatus::Invalid, "wrong bot identity");
    Refused(PlanUpdate(missing, true, {UpdateKind::ProposeAlternative, BotGuid, 0, Source::Manual, "Forged source"}),
            UpdateStatus::Invalid, "manual source cannot use provider proposal route");
    Refused(PlanUpdate(missing, true, {UpdateKind::SetManual, BotGuid, 0, Source::Codex, "Forged manual"}),
            UpdateStatus::Invalid, "provider source cannot use manual route");
    Refused(PlanUpdate(missing, true, {UpdateKind::AcceptAlternative, BotGuid, 0, Source::Codex, "Injected body"}),
            UpdateStatus::Invalid, "promotion can select only a persisted body");
    Refused(PlanUpdate(missing, true, {UpdateKind::ProposeAlternative, BotGuid, 0, Source(99), "Unknown provider"}),
            UpdateStatus::Invalid, "unknown source");
    Refused(PlanUpdate(missing, true, {UpdateKind(99), BotGuid, 0, Source::Manual, "Unknown operation"}),
            UpdateStatus::Invalid, "unknown operation");

    Check(!ValidState({0, 0, {}, {}, {}}) && !ValidState({BotGuid, 1, {}, {}, {}}), "reject corrupt row identity");
    Check(!ValidState({BotGuid, 0, {}, "Draft", {}}), "revision zero is reserved for an absent row");
    Check(!ValidState({BotGuid, 1, Card{Source(0), "Body"}, {}, {}}), "reject invalid stored provenance");
    Check(!ValidState({BotGuid, 1, Card{Source::Manual, ""}, {}, {}}), "reject invalid stored accepted body");
    Check(!ValidState({BotGuid, 1, {}, {}, std::string(MaxCardBytes + 1, 'a')}), "reject oversized stored draft");
    Refused(PlanUpdate({BotGuid, 0, {}, "Corrupt", {}}, true, update), UpdateStatus::Invalid,
            "refuse to write over a corrupt cached row");

    uint64_t const maxRevision = std::numeric_limits<uint64_t>::max();
    State const full{BotGuid, maxRevision, Card{Source::Manual, "Kept"}, {}, {}};
    Refused(PlanUpdate(full, true, {UpdateKind::SetManual, BotGuid, maxRevision, Source::Manual, "New"}),
            UpdateStatus::RevisionExhausted, "revision must never wrap");
    Refused(PlanUpdate(full, true, {UpdateKind::SetManual, BotGuid, maxRevision, Source::Manual, "Kept"}),
            UpdateStatus::Unchanged, "no-op needs no revision at the ceiling");
}
}  // namespace

int main()
{
    try
    {
        MissingAndProviderSwitch();
        ManualAndConflicts();
        InvalidInputsAndLimits();
        std::cout << "persona-card selftest passed " << checks << " checks\n";
    }
    catch (std::exception const& error)
    {
        std::cerr << "persona-card selftest failed at check " << checks << " - " << error.what() << '\n';
        return 1;
    }
}
