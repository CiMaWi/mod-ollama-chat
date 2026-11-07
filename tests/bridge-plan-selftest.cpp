// Offline checks of the pure J12 plan ledger (src/mod-ollama-chat_bridge_plan.h) with a synthetic state type:
// refusal, group changes, stale epochs, expiration, repeated application, a person's change, restoration and
// capacity. No game, no playerbots, no clock.
#include <iostream>
#include <limits>
#include <stdexcept>

#include "mod-ollama-chat_bridge_plan.h"

namespace
{
using namespace OllamaBridge;

unsigned checks = 0;
constexpr uint64_t Bot = 9007199254740994ULL;
constexpr uint64_t OtherBot = 9007199254740995ULL;
constexpr uint64_t ThirdBot = 9007199254740996ULL;
constexpr uint64_t Controller = 9007199254740993ULL;
constexpr uint64_t OtherController = 77;
constexpr uint64_t Group = 15;
constexpr uint64_t OtherGroup = 16;
constexpr uint64_t Generation = 3;
constexpr int64_t Start = 1000000;

void Check(bool value, char const* label)
{
    ++checks;
    if (!value)
        throw std::runtime_error(label);
}

struct Snapshot
{
    int value = 0;
    bool operator==(Snapshot const&) const = default;
};

using Ledger = PlanLedger<Snapshot>;

PlanKey Key(PlanKind kind, uint64_t bot = Bot) { return {bot, kind}; }
PlanOwner Owner(uint64_t generation = Generation) { return {Group, Controller, generation}; }

bool SameOwner(PlanOwner const& left, PlanOwner const& right)
{
    return left.groupGuid == right.groupGuid && left.controllerGuid == right.controllerGuid &&
           left.generation == right.generation;
}

bool Apply(Ledger& ledger, PlanKey const& key, int before, int applied, int64_t now, PlanOwner const& owner = Owner())
{
    return ledger.Record(key, owner, Snapshot{before}, Snapshot{applied}, now);
}

void Mapping()
{
    Check(PlanKindFor(ChannelIntent::Follow) == PlanKind::Movement, "follow is a movement plan");
    Check(PlanKindFor(ChannelIntent::Stay) == PlanKind::Movement, "stay shares the movement plan");
    Check(PlanKindFor(ChannelIntent::Formation) == PlanKind::Formation, "formation plan");
    Check(PlanKindFor(ChannelIntent::SaveMana) == PlanKind::SaveMana, "save mana plan");
    Check(PlanKindFor(ChannelIntent::FocusHealing) == PlanKind::FocusHealing, "focus healing plan");
    for (ChannelIntent intent :
         {ChannelIntent::Flee, ChannelIntent::Regroup, ChannelIntent::AttackOwnTarget, ChannelIntent::Mark,
          ChannelIntent::Pull, ChannelIntent::AssignRole, ChannelIntent::Position, ChannelIntent::DungeonClear})
        Check(!PlanKindFor(intent), "one-shot verbs (marks included) and refused verbs have no plan");
    Check(!PlanKindFor(static_cast<ChannelIntent>(200)), "an unknown verb has no plan");
}

void Refusal()
{
    Ledger ledger;
    Check(ledger.Admit(Key(PlanKind::Formation), Owner(), Start) == PlanAdmission::Refused, "closed ledger refuses");
    Check(ledger.Admit(Key(PlanKind::Formation), Owner(0), Start) == PlanAdmission::Refused,
          "closed ledger refuses generation 0");
    ledger.Open(Generation);
    Check(ledger.Generation() == Generation, "open generation");
    Check(ledger.Admit(Key(PlanKind::Formation), Owner(), Start) == PlanAdmission::Capture, "first order captures");
    Check(ledger.Admit(Key(PlanKind::Count), Owner(), Start) == PlanAdmission::Refused, "unknown kind refused");
    Check(ledger.Admit(Key(static_cast<PlanKind>(200)), Owner(), Start) == PlanAdmission::Refused,
          "out-of-range kind refused");
    Check(ledger.Admit(Key(PlanKind::Formation, 0), Owner(), Start) == PlanAdmission::Refused, "zero bot refused");
    Check(ledger.Admit(Key(PlanKind::Formation), {0, Controller, Generation}, Start) == PlanAdmission::Refused,
          "zero group refused");
    Check(ledger.Admit(Key(PlanKind::Formation), {Group, 0, Generation}, Start) == PlanAdmission::Refused,
          "zero controller refused");
    Check(ledger.Admit(Key(PlanKind::Formation), Owner(0), Start) == PlanAdmission::Refused, "zero generation refused");
    Check(ledger.Admit(Key(PlanKind::Formation), Owner(Generation + 1), Start) == PlanAdmission::Refused,
          "another generation refused");
    Check(ledger.Admit(Key(PlanKind::Formation), Owner(), -1) == PlanAdmission::Refused, "negative clock refused");
    Check(
        ledger.Admit(Key(PlanKind::Formation), Owner(), std::numeric_limits<int64_t>::max()) == PlanAdmission::Refused,
        "deadline overflow refused");
    Check(ledger.Admit(Key(PlanKind::Formation), Owner(), std::numeric_limits<int64_t>::max() - PlanLifetimeMs) ==
              PlanAdmission::Capture,
          "largest representable deadline admitted");

    Check(!Apply(ledger, Key(PlanKind::Count), 1, 2, Start), "unknown kind records nothing");
    Check(!Apply(ledger, Key(PlanKind::Formation), 1, 2, Start, Owner(Generation + 1)), "stale order records nothing");
    Check(ledger.Empty() && ledger.Counters().applied == 0, "refusals record nothing");

    Check(Apply(ledger, Key(PlanKind::Formation), 1, 2, Start), "apply formation");
    Check(ledger.Admit(Key(PlanKind::SaveMana), {Group, OtherController, Generation}, Start) == PlanAdmission::Refused,
          "another controller's plan for the bot refused");
    Check(ledger.Admit(Key(PlanKind::SaveMana), {OtherGroup, Controller, Generation}, Start) == PlanAdmission::Refused,
          "a plan from another group refused");
    Check(ledger.Admit(Key(PlanKind::Formation), {Group, OtherController, Generation}, Start) == PlanAdmission::Refused,
          "another controller cannot renew");
    Check(!Apply(ledger, Key(PlanKind::Formation), 9, 9, Start, {Group, OtherController, Generation}),
          "another controller records nothing");
    Check(ledger.Admit(Key(PlanKind::SaveMana, OtherBot), {OtherGroup, OtherController, Generation}, Start) ==
              PlanAdmission::Capture,
          "another bot has its own owner");
    Check(ledger.Size() == 1 && ledger.Counters().applied == 1 && ledger.Counters().renewed == 0,
          "only the one valid plan recorded");
}

void GroupChanges()
{
    PlanOwner const otherOwner{OtherGroup, OtherController, Generation};
    Ledger ledger;
    ledger.Open(Generation);
    Check(Apply(ledger, Key(PlanKind::Formation), 1, 2, Start), "apply formation");
    Check(Apply(ledger, Key(PlanKind::Movement), 3, 4, Start), "apply movement");
    Check(Apply(ledger, Key(PlanKind::SaveMana, OtherBot), 5, 6, Start, otherOwner), "apply other bot");
    Check(ledger.Observe(Bot, Group, true) == 0, "same group and controller keeps plans");
    Check(ledger.Pending(Start).empty(), "nothing pending while unchanged");
    auto const live = ledger.LiveBots();
    Check(live.size() == 2, "two live bots, listed once each");
    Check(live[0].botGuid == Bot && SameOwner(live[0].owner, Owner()), "live bot reports its recorded owner");
    Check(live[1].botGuid == OtherBot && SameOwner(live[1].owner, otherOwner), "each bot reports its own owner");
    Check(ledger.LiveApplied(Key(PlanKind::Movement)) == Snapshot{4}, "live entry exposes what the bridge set");
    Check(!ledger.LiveApplied(Key(PlanKind::SaveMana)), "no live value for a kind the bot has no plan of");

    Check(ledger.Observe(Bot, 0, true) == 2, "bot left its group");
    Check(ledger.Pending(Start).size() == 2 && ledger.Counters().regrouped == 2, "both of the bot's plans pending");
    Check(!ledger.LiveApplied(Key(PlanKind::Movement)), "a pending entry has no live value");
    Check(ledger.Observe(Bot, 0, true) == 0 && ledger.Counters().regrouped == 2, "pending entries move once");
    Check(ledger.LiveBots().size() == 1 && ledger.LiveBots()[0].botGuid == OtherBot,
          "pending bot leaves the live list");
    Check(ledger.Admit(Key(PlanKind::SaveMana), Owner(), Start) == PlanAdmission::Refused,
          "a bot with a pending restore takes no new plan");
    Check(ledger.Admit(Key(PlanKind::SaveMana, OtherBot), otherOwner, Start) == PlanAdmission::Renew,
          "other bot unaffected");

    Ledger controllerLost;
    controllerLost.Open(Generation);
    Check(Apply(controllerLost, Key(PlanKind::FocusHealing), 1, 2, Start), "apply focus healing");
    Check(controllerLost.Observe(Bot, Group, false) == 1, "controller left the group");
    Check(controllerLost.Pending(Start).size() == 1, "controller loss makes the plan pending");

    Ledger regroup;
    regroup.Open(Generation);
    Check(Apply(regroup, Key(PlanKind::Movement), 1, 2, Start), "apply movement");
    Check(regroup.Observe(Bot, OtherGroup, true) == 1, "group id changed");
    Check(regroup.Pending(Start).size() == 1 && regroup.Counters().regrouped == 1, "new group makes the plan pending");
    Check(regroup.Observe(OtherBot, 0, false) == 0, "unknown bot changes nothing");
}

void StaleEpochs()
{
    Ledger ledger;
    ledger.Open(Generation);
    Check(Apply(ledger, Key(PlanKind::Formation), 1, 2, Start), "apply formation");
    Check(Apply(ledger, Key(PlanKind::SaveMana, OtherBot), 3, 4, Start), "apply other bot");
    ledger.Open(Generation);
    Check(ledger.Pending(Start).empty(), "reopening the same generation keeps plans");

    ledger.RetireAll();
    Check(ledger.Generation() == 0, "retirement closes admission");
    Check(ledger.Pending(Start).size() == 2 && ledger.Counters().retired == 2, "retirement makes every plan pending");
    Check(!ledger.Empty(), "pending restores keep the ledger non-empty");
    Check(ledger.Admit(Key(PlanKind::Movement, ThirdBot), Owner(), Start) == PlanAdmission::Refused,
          "old generation refused after retirement");
    ledger.RetireAll();
    Check(ledger.Counters().retired == 2, "a second retirement moves nothing");

    ledger.Open(Generation + 1);
    Check(ledger.Admit(Key(PlanKind::Movement, ThirdBot), Owner(Generation), Start) == PlanAdmission::Refused,
          "stale generation refused after reopen");
    Check(ledger.Admit(Key(PlanKind::Movement, ThirdBot), Owner(Generation + 1), Start) == PlanAdmission::Capture,
          "new generation admitted for a clean bot");
    Check(ledger.Admit(Key(PlanKind::Movement), Owner(Generation + 1), Start) == PlanAdmission::Refused,
          "bot with a restore pending from the old generation refused");

    Check(Apply(ledger, Key(PlanKind::Movement, ThirdBot), 5, 6, Start, Owner(Generation + 1)), "apply new gen");
    ledger.Open(Generation + 2);
    Check(ledger.Pending(Start).size() == 3 && ledger.Counters().retired == 3,
          "a generation change retires live plans");
}

void Expiration()
{
    Ledger ledger;
    ledger.Open(Generation);
    Check(Apply(ledger, Key(PlanKind::Formation), 1, 2, Start), "apply formation");
    Check(ledger.Expire(Start + PlanLifetimeMs - 1) == 0, "live one millisecond before the deadline");
    Check(ledger.Admit(Key(PlanKind::Formation), Owner(), Start + PlanLifetimeMs - 1) == PlanAdmission::Renew,
          "renewable before the deadline");
    Check(ledger.Admit(Key(PlanKind::Formation), Owner(), Start + PlanLifetimeMs) == PlanAdmission::Refused,
          "a late repeat never revives an expired plan");
    Check(ledger.Expire(Start + PlanLifetimeMs) == 1, "expires exactly at the deadline");
    Check(ledger.Counters().expired == 1 && ledger.Pending(Start + PlanLifetimeMs).size() == 1,
          "expiry makes the plan pending");
    Check(ledger.Expire(Start + 2 * PlanLifetimeMs) == 0 && ledger.Counters().expired == 1, "expires once");

    Ledger rollback;
    rollback.Open(Generation);
    Check(Apply(rollback, Key(PlanKind::Movement), 1, 2, Start), "apply movement");
    Check(rollback.Expire(Start) == 0, "not expired at application");
    Check(rollback.Expire(Start - 1) == 1, "a clock rollback expires instead of extending");

    Ledger renewed;
    renewed.Open(Generation);
    Check(Apply(renewed, Key(PlanKind::SaveMana), 1, 2, Start), "apply save mana");
    int64_t const later = Start + 5000;
    Check(Apply(renewed, Key(PlanKind::SaveMana), 2, 3, later), "repeat renews");
    Check(renewed.Expire(Start + PlanLifetimeMs) == 0, "the renewal moved the deadline");
    Check(renewed.Expire(later - 1) == 1, "a rollback behind the renewal expires");

    Ledger renewedDeadline;
    renewedDeadline.Open(Generation);
    Check(Apply(renewedDeadline, Key(PlanKind::SaveMana), 1, 2, Start), "apply save mana");
    Check(Apply(renewedDeadline, Key(PlanKind::SaveMana), 2, 3, later), "repeat renews");
    Check(renewedDeadline.Expire(later + PlanLifetimeMs - 1) == 0, "live until the renewed deadline");
    Check(renewedDeadline.Expire(later + PlanLifetimeMs) == 1, "expires at the renewed deadline");
}

void RepeatedApplication()
{
    Ledger ledger;
    ledger.Open(Generation);
    Check(ledger.Admit(Key(PlanKind::Movement), Owner(), Start) == PlanAdmission::Capture, "first follow captures");
    Check(Apply(ledger, Key(*PlanKindFor(ChannelIntent::Follow)), 10, 11, Start), "apply follow");
    Check(ledger.Admit(Key(PlanKind::Movement), Owner(), Start + 1) == PlanAdmission::Renew, "stay renews movement");
    int last = 11;
    for (int i = 0; i < 20; ++i)
    {
        // Each repeat starts from what the bridge set last: nobody changed the bot in between.
        Check(Apply(ledger, Key(*PlanKindFor(i % 2 ? ChannelIntent::Follow : ChannelIntent::Stay)), last, 20 + i,
                    Start + 1 + i),
              "repeat follow and stay");
        last = 20 + i;
    }
    Check(ledger.Size() == 1, "repeats never stack");
    Check(ledger.Counters().applied == 1 && ledger.Counters().renewed == 20, "one capture, twenty renewals");
    Check(ledger.LiveApplied(Key(PlanKind::Movement)) == Snapshot{39}, "live value is the latest");
    Check(Apply(ledger, Key(PlanKind::Formation), 30, 31, Start + 30), "another kind adds an entry");
    Check(ledger.Size() == 2 && ledger.LiveBots().size() == 1, "one entry per kind, one bot");

    ledger.RetireAll();
    auto const pending = ledger.Pending(Start + 31);
    Check(pending.size() == 2, "both pending");
    Check(pending[0].key == Key(PlanKind::Movement), "pending in application order");
    Check(pending[0].baseline.value == 10, "restore target captured once, before the first change");
    Check(pending[0].applied.value == 39, "applied value is the latest");
    Check(pending[0].attempts == 0, "no restore attempted yet");
}

void PersonChange()
{
    Ledger ledger;
    ledger.Open(Generation);
    Check(Apply(ledger, Key(PlanKind::Movement), 10, 11, Start), "bridge follow from the bot's own state");
    // A person sets 50 through playerbots chat; the bridge's next stay starts from it.
    Check(Apply(ledger, Key(PlanKind::Movement), 50, 12, Start + 1), "bridge stay after a person's change");
    Check(ledger.Counters().applied == 1 && ledger.Counters().renewed == 1, "still one entry, renewed");
    Check(Apply(ledger, Key(PlanKind::Movement), 12, 13, Start + 2), "bridge repeat with nobody in between");
    ledger.RetireAll();
    auto const pending = ledger.Pending(Start + 3);
    Check(pending.size() == 1 && pending[0].baseline.value == 50, "the person's change is the restore target");
    Check(pending[0].applied.value == 13, "applied value is the bridge's latest");
}

void Restoration()
{
    Ledger ledger;
    ledger.Open(Generation);
    Check(Apply(ledger, Key(PlanKind::Formation), 1, 2, Start), "apply formation");
    Check(Apply(ledger, Key(PlanKind::SaveMana), 3, 4, Start), "apply save mana");
    Check(Apply(ledger, Key(PlanKind::Movement, OtherBot), 5, 6, Start), "apply other bot");
    Check(Apply(ledger, Key(PlanKind::FocusHealing, ThirdBot), 7, 8, Start), "apply third bot");
    Check(!ledger.Restored(Key(PlanKind::Formation)), "a live plan is not restorable");
    Check(!ledger.RestoreFailed(Key(PlanKind::Formation), Start), "a live plan has no restore attempt");
    Check(ledger.Size() == 4, "live plans untouched");

    ledger.RetireAll();
    Check(ledger.Restored(Key(PlanKind::Formation)), "restore once");
    Check(!ledger.Restored(Key(PlanKind::Formation)), "a second restore finds nothing");
    Check(ledger.Counters().restored == 1 && ledger.Size() == 3, "restored entry removed and counted once");
    Check(ledger.Pending(Start).size() == 3, "untried restores are due");

    Check(ledger.RestoreFailed(Key(PlanKind::SaveMana), Start), "first failure retried");
    Check(ledger.Size() == 3, "failed restore stays pending");
    Check(ledger.Pending(Start).size() == 2, "a failed restore waits before its retry");
    Check(ledger.Pending(Start + PlanRestoreRetryMs - 1).size() == 2, "not due one millisecond early");
    auto const due = ledger.Pending(Start + PlanRestoreRetryMs);
    Check(due.size() == 3 && due[0].key == Key(PlanKind::SaveMana) && due[0].attempts == 1,
          "due again after the retry interval, attempt counted");
    Check(ledger.Pending(Start - 1).size() == 3, "a clock rollback retries at once instead of stalling");
    Check(ledger.RestoreFailed(Key(PlanKind::SaveMana), Start + PlanRestoreRetryMs), "second failure retried");
    Check(ledger.Size() == 3 && ledger.Counters().abandoned == 0, "still pending after two failures");
    Check(ledger.Pending(Start + 2 * PlanRestoreRetryMs - 1).size() == 2, "the second retry waits from its failure");
    Check(ledger.RestoreFailed(Key(PlanKind::SaveMana), Start + 2 * PlanRestoreRetryMs), "third failure");
    Check(ledger.Size() == 2 && ledger.Counters().abandoned == 1, "dropped after the bounded attempts");
    Check(!ledger.RestoreFailed(Key(PlanKind::SaveMana), Start), "a dropped entry is gone");
    Check(!ledger.Restored(Key(PlanKind::SaveMana)), "a dropped entry is never restored");

    Check(ledger.RestoreFailed(Key(PlanKind::Movement, OtherBot), Start), "a brief failure");
    Check(ledger.Pending(Start + PlanRestoreRetryMs).size() == 2, "retried after the interval");
    Check(ledger.Restored(Key(PlanKind::Movement, OtherBot)), "a restore succeeds after one failure");
    Check(ledger.Counters().restored == 2 && ledger.Counters().abandoned == 1, "counted as restored, not abandoned");

    Check(ledger.Departed(ThirdBot) == 1, "a bot that logged out drops its pending plan");
    Check(ledger.Counters().departed == 1 && ledger.Counters().restored == 2, "departure is not a restore");
    Check(ledger.Empty(), "ledger empty after every entry ended");

    Ledger fresh;
    fresh.Open(Generation);
    Check(Apply(fresh, Key(PlanKind::Formation), 1, 2, 1), "apply formation early on the steady clock");
    Check(Apply(fresh, Key(PlanKind::Movement, OtherBot), 3, 4, 1), "apply other bot");
    fresh.RetireAll();
    Check(fresh.Pending(1).size() == 2, "a never-tried restore is due at once");
    Check(fresh.Departed(Bot) == 1, "the departed bot's pending plan dropped");
    auto const left = fresh.Pending(1);
    Check(left.size() == 1 && left[0].key == Key(PlanKind::Movement, OtherBot),
          "another bot's pending restore stays in place");

    Ledger live;
    live.Open(Generation);
    Check(Apply(live, Key(PlanKind::Formation), 1, 2, Start), "apply formation");
    Check(Apply(live, Key(PlanKind::Movement), 3, 4, Start), "apply movement");
    Check(Apply(live, Key(PlanKind::Formation, OtherBot), 5, 6, Start), "apply other bot");
    Check(live.Departed(Bot) == 2, "logout drops the bot's live plans without a restore");
    Check(live.Pending(Start).empty() && live.Size() == 1, "other bot's plan stays");
    Check(live.Admit(Key(PlanKind::Formation), Owner(), Start) == PlanAdmission::Capture,
          "a returning bot starts from a fresh baseline");
}

void Capacity()
{
    Ledger ledger(2);
    ledger.Open(Generation);
    Check(Apply(ledger, Key(PlanKind::Formation), 1, 2, Start), "first entry");
    Check(Apply(ledger, Key(PlanKind::Formation, OtherBot), 3, 4, Start), "second entry");
    Check(ledger.Admit(Key(PlanKind::SaveMana), Owner(), Start) == PlanAdmission::Refused, "full table refuses");
    Check(!Apply(ledger, Key(PlanKind::SaveMana), 5, 6, Start), "full table records nothing");
    Check(ledger.Admit(Key(PlanKind::Formation), Owner(), Start + 1) == PlanAdmission::Renew,
          "a full table still renews a live plan");
    ledger.RetireAll();
    ledger.Open(Generation + 1);
    Check(ledger.Admit(Key(PlanKind::SaveMana, ThirdBot), Owner(Generation + 1), Start) == PlanAdmission::Refused,
          "pending restores keep their slots; nothing is evicted");
    Check(ledger.Restored(Key(PlanKind::Formation)), "restore frees a slot");
    Check(ledger.Admit(Key(PlanKind::SaveMana, ThirdBot), Owner(Generation + 1), Start) == PlanAdmission::Capture,
          "a freed slot admits");

    Ledger clamped(MaxPlanEntries + 10);
    clamped.Open(Generation);
    for (std::size_t i = 0; i < MaxPlanEntries; ++i)
        Check(Apply(clamped, Key(PlanKind::Formation, 1000 + i), 1, 2, Start), "fill to the hard bound");
    Check(clamped.Admit(Key(PlanKind::Formation, 5000), Owner(), Start) == PlanAdmission::Refused,
          "capacity never exceeds MaxPlanEntries");
}
}  // namespace

int main()
{
    try
    {
        Mapping();
        Refusal();
        GroupChanges();
        StaleEpochs();
        Expiration();
        RepeatedApplication();
        PersonChange();
        Restoration();
        Capacity();
        std::cout << "PASS " << checks << " checks\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
