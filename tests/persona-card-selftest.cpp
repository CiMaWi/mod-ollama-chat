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
    UpdatePlan plan = PlanUpdate(state, update);
    Check(plan.status == UpdateStatus::Ready && plan.next && ValidState(*plan.next) &&
              plan.next->revision == state.revision + 1 && plan.next->botGuid == state.botGuid,
          "write plans produce a valid, identity-preserving next row");
    return std::move(*plan.next);
}

State MissingRow() { return {BotGuid, 0, {}}; }

// DS.12: a generated card is applied directly to a bot without a card; no acceptance step exists.
void DirectApplication()
{
    State const missing{BotGuid, 0, {}};
    Check(ValidState(missing) && !missing.accepted, "confirmed missing card is explicit");
    Update claude{UpdateKind::ApplyGenerated, BotGuid, 0, Source::Claude, "A traveling blacksmith"};
    State const first = Apply(missing, claude);
    Check(first.revision == 1 && first.accepted && first.accepted->source == Source::Claude &&
              first.accepted->body == claude.body,
          "a generated card is applied at once with its provenance");
    Refused(PlanUpdate(first, claude), UpdateStatus::Stale, "retry from the missing revision is stale");
    claude.expectedRevision = 1;
    Refused(PlanUpdate(first, claude), UpdateStatus::Unchanged, "same card does not churn revisions");
    Update const other{UpdateKind::ApplyGenerated, BotGuid, 1, Source::Claude, "A retired sea captain"};
    Refused(PlanUpdate(first, other), UpdateStatus::HasCard, "a second card never replaces the first");
    Update const codex{UpdateKind::ApplyGenerated, BotGuid, 1, Source::Codex, "A retired sea captain"};
    Refused(PlanUpdate(first, codex), UpdateStatus::HasCard, "another provider never replaces a card either");

    State const codexFirst = Apply(missing, {UpdateKind::ApplyGenerated, BotGuid, 0, Source::Codex, "A gnome tinker"});
    Check(codexFirst.accepted->source == Source::Codex, "either native provider may apply");

    // A row from before J14e held only drafts: a nonzero revision without a card, which a card fills.
    State const legacy{BotGuid, 4, {}};
    Check(ValidState(legacy), "a draft-only legacy row loads as a bot without a card");
    State const filled = Apply(legacy, {UpdateKind::ApplyGenerated, BotGuid, 4, Source::Claude, "A herbalist"});
    Check(filled.revision == 5 && filled.accepted->body == "A herbalist", "a legacy row takes a card at revision 5");
}

void ManualAndConflicts()
{
    State const generated{BotGuid, 3, Card{Source::Claude, "Generated"}};
    State const manual = Apply(generated, {UpdateKind::SetManual, BotGuid, 3, Source::Manual, "Edited by the owner"});
    Check(manual.accepted->source == Source::Manual && manual.accepted->body == "Edited by the owner",
          "the owner may replace a generated card");
    Refused(PlanUpdate(manual, {UpdateKind::ApplyGenerated, BotGuid, 4, Source::Claude, "New"}),
            UpdateStatus::ManualProtected, "a provider never replaces the owner's card");
    Refused(PlanUpdate(manual, {UpdateKind::ApplyGenerated, BotGuid, 4, Source::Claude, "Edited by the owner"}),
            UpdateStatus::ManualProtected, "not even with the same text");
    Refused(PlanUpdate(manual, {UpdateKind::SetManual, BotGuid, 4, Source::Manual, "Edited by the owner"}),
            UpdateStatus::Unchanged, "same manual card does not churn revisions");
    Refused(PlanUpdate(manual, {UpdateKind::SetManual, BotGuid, 3, Source::Manual, "Late edit"}), UpdateStatus::Stale,
            "a stale manual edit is refused");
    State const longManual =
        Apply(MissingRow(), {UpdateKind::SetManual, BotGuid, 0, Source::Manual, std::string(MaxCardBytes, 'm')});
    Check(longManual.accepted->body.size() == MaxCardBytes, "the owner's card keeps the full card bound");
}

void InvalidInputsAndLimits()
{
    State const missing = MissingRow();
    Update update{UpdateKind::ApplyGenerated, BotGuid, 0, Source::Claude, {}};
    for (std::string const& body :
         {std::string{}, std::string(" \t\r\n"), std::string("a\0b", 3), std::string("a\x1b"), std::string("a\x7f"),
          std::string("\xc0\xaf"), std::string("\xed\xa0\x80"), std::string("\xf0\x9f\x8c"),
          std::string(MaxGeneratedCardBytes + 1, 'a'), std::string(MaxCardBytes + 1, 'a')})
    {
        update.body = body;
        Refused(PlanUpdate(missing, update), UpdateStatus::Invalid, "malformed or overbound generated card");
    }
    update.body = std::string(MaxGeneratedCardBytes - 4, 'a') + "\xf0\x9f\x8c\xb1";
    Check(Apply(missing, update).accepted->body.size() == MaxGeneratedCardBytes, "exact generated UTF-8 byte ceiling");
    update.body = "Line one\n\tLine two\r\n";
    Check(Apply(missing, update).accepted->body == update.body, "tabs and line breaks are allowed");

    Refused(PlanUpdate(missing, {UpdateKind::ApplyGenerated, BotGuid + 1, 0, Source::Claude, "Wrong bot"}),
            UpdateStatus::Invalid, "wrong bot identity");
    Refused(PlanUpdate(missing, {UpdateKind::ApplyGenerated, BotGuid, 0, Source::Manual, "Forged source"}),
            UpdateStatus::Invalid, "manual source cannot use the generated route");
    Refused(PlanUpdate(missing, {UpdateKind::SetManual, BotGuid, 0, Source::Codex, "Forged manual"}),
            UpdateStatus::Invalid, "provider source cannot use the manual route");
    Refused(PlanUpdate(missing, {UpdateKind::ApplyGenerated, BotGuid, 0, Source(99), "Unknown provider"}),
            UpdateStatus::Invalid, "unknown source");
    Refused(PlanUpdate(missing, {UpdateKind(99), BotGuid, 0, Source::Manual, "Unknown operation"}),
            UpdateStatus::Invalid, "unknown operation");

    Check(!ValidState({0, 0, {}}), "reject corrupt row identity");
    Check(!ValidState({BotGuid, 0, Card{Source::Claude, "Body"}}), "revision zero is reserved for an absent row");
    Check(!ValidState({BotGuid, 1, Card{Source(0), "Body"}}), "reject invalid stored provenance");
    Check(!ValidState({BotGuid, 1, Card{Source::Manual, ""}}), "reject invalid stored accepted body");
    Check(!ValidState({BotGuid, 1, Card{Source::Manual, std::string(MaxCardBytes + 1, 'a')}}),
          "reject oversized stored card");
    update.body = "Fine";
    Refused(PlanUpdate({BotGuid, 0, Card{Source::Claude, "Corrupt"}}, update), UpdateStatus::Invalid,
            "refuse to write over a corrupt cached row");

    uint64_t const maxRevision = std::numeric_limits<uint64_t>::max();
    State const full{BotGuid, maxRevision, Card{Source::Manual, "Kept"}};
    Refused(PlanUpdate(full, {UpdateKind::SetManual, BotGuid, maxRevision, Source::Manual, "New"}),
            UpdateStatus::RevisionExhausted, "revision must never wrap");
    Refused(PlanUpdate(full, {UpdateKind::SetManual, BotGuid, maxRevision, Source::Manual, "Kept"}),
            UpdateStatus::Unchanged, "no-op needs no revision at the ceiling");
    Refused(PlanUpdate({BotGuid, maxRevision, {}},
                       {UpdateKind::ApplyGenerated, BotGuid, maxRevision, Source::Claude, "New"}),
            UpdateStatus::RevisionExhausted, "a generated card never wraps the revision either");
}
}  // namespace

int main()
{
    try
    {
        DirectApplication();
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
