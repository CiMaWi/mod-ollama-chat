#include "mod-ollama-chat_bridge_effect.h"

#include "BridgeAbilities.h"
#include "BridgeIntentEffect.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "mod-ollama-chat_bridge_plan.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"

// World-only production typed effect adapter. WorldChannel::Update calls it synchronously with the post-map-join
// phase, after the channel's epoch, route, deadline, rate and replay joins. It resolves every game fact afresh by
// GUID, keeps no pointer, phase or callback past the call and hands one typed order to the playerbots boundary,
// which calls authorize once immediately before its single mutation. Lasting verbs (J12) also go through the plan
// ledger: admission and the bot's state before the change first, the ledger record after the attempt, and the world
// tick restores that state on expiry, a group change, the partner's loss and retirement.
namespace OllamaBridge
{
namespace
{
using BridgePlans = PlanLedger<PlayerbotBridge::Baseline>;

class BridgeCommandExecutor final : public CommandExecutor
{
public:
    EffectResult Execute(EffectPhase const& phase, WorldTicket const& ticket, DispatchGuard const& guard,
                         ChannelCommand const& command, std::function<bool()> const& authorize) override
    {
        // Release gate (open since J6b4): closed, it refuses before any game read, authorization or playerbots call.
        _refusal = CommandRefusal::None;
        _steps.clear();
        if constexpr (!kBridgeEffectsReleased)
        {
            _refusal = CommandRefusal::NotSupported;
            return EffectResult::Rejected;
        }
        else if (!command.steps.empty())
            return Stepped(phase, ticket, guard, command, authorize, _refusal, _steps);
        else
            return Effect(phase, ticket, guard, command, authorize, _refusal);
    }

    CommandRefusal Refusal() const override { return _refusal; }
    std::vector<ChannelStepResult> Steps() const override { return _steps; }

    // World thread only; memory only (a restart forgets it). One ledger for the one executor.
    static BridgePlans& Plans()
    {
        static BridgePlans plans;
        return plans;
    }

    // World tick, post-map-join: expiry, then each bot with live plans (logged out: dropped unrestored; another group,
    // its plans' partner offline, or out of the bot's group for a plan that needed it there: restore-pending), then
    // every due restore. Nothing is read or allocated while the ledger is empty.
    static void UpdatePlans(EffectPhase const& phase, int64_t nowMs)
    {
        BridgePlans& plans = Plans();
        if (plans.Empty() || !phase.join.Current())
            return;
        plans.Expire(nowMs);
        for (PlanLiveBot const& live : plans.LiveBots())
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(live.botGuid));
            if (!bot)
            {
                plans.Departed(live.botGuid);
                continue;
            }
            uint64_t const group = GroupGuid(bot);
            Player* partner = ObjectAccessor::FindConnectedPlayer(ObjectGuid(live.owner.controllerGuid));
            plans.Observe(live.botGuid, group, partner != nullptr, PartnerInGroup(partner, group));
        }
        for (PlanRestore<PlayerbotBridge::Baseline> const& restore : plans.Pending(nowMs))
        {
            switch (PlayerbotBridge::Restore(phase.join, restore.key.botGuid, restore.baseline, restore.applied))
            {
                case PlayerbotBridge::RestoreResult::Restored:
                case PlayerbotBridge::RestoreResult::Superseded:  // A later command won; left as it is.
                    plans.Restored(restore.key);
                    break;
                case PlayerbotBridge::RestoreResult::Gone:  // Logged out: nothing to write.
                    plans.Departed(restore.key.botGuid);
                    break;
                default:  // Failed (a teleport) or Rejected: retried a bounded number of times, then dropped.
                    plans.RestoreFailed(restore.key, nowMs);
                    break;
            }
        }
    }

private:
    // The same conservative participant-observable unit check as handler.cpp's BridgeVisibleUnit.
    static bool VisibleUnit(Player* observer, Unit* unit, float distance)
    {
        return observer && observer->IsInWorld() && unit && unit->IsInWorld() && distance > 0.0f &&
               observer->IsInMap(unit) && observer->InSamePhase(unit) && observer->IsWithinDistInMap(unit, distance) &&
               observer->CanSeeOrDetect(unit, false, true);
    }

    static uint64_t GroupGuid(Player* player)
    {
        Group* group = player->GetGroup();
        return group ? group->GetGUID().GetRawValue() : 0;
    }

    // DS.14: whether the partner is in the bot's group (groupGuid, the bot's current one; never for an ungrouped bot).
    // A plan holds while its partner is online and the bot's group is unchanged; only a plan whose admission needed
    // the partner in the group (follow, formation, save_mana) also ends when the partner leaves it, so a stay or a
    // focus heal from a partner outside the group holds. Never a master or leader rule.
    static bool PartnerInGroup(Player* partner, uint64_t groupGuid)
    {
        return partner && groupGuid && GroupGuid(partner) == groupGuid;
    }

    static PlayerbotBridge::FormationShape Shape(std::optional<ChannelShape> shape)
    {
        if (!shape)
            return PlayerbotBridge::FormationShape::None;
        switch (*shape)
        {
            case ChannelShape::Close:
                return PlayerbotBridge::FormationShape::Close;
            case ChannelShape::Spread:
                return PlayerbotBridge::FormationShape::Spread;
            case ChannelShape::Line:
                return PlayerbotBridge::FormationShape::Line;
        }
        return PlayerbotBridge::FormationShape::None;  // Playerbots refuses a formation without a shape.
    }

    // DS.14: playerbots' reason for a Rejected order, in the bot's own words. None (no reason given) is worded
    // cannot_now by the channel.
    static CommandRefusal OrderRefusal(PlayerbotBridge::Refusal reason)
    {
        switch (reason)
        {
            case PlayerbotBridge::Refusal::Dead:
                return CommandRefusal::Dead;
            case PlayerbotBridge::Refusal::Battleground:
                return CommandRefusal::Battleground;
            case PlayerbotBridge::Refusal::Travelling:
                return CommandRefusal::Travelling;
            case PlayerbotBridge::Refusal::NotWithYou:
                return CommandRefusal::NotWithYou;
            case PlayerbotBridge::Refusal::TooFar:
                return CommandRefusal::TooFar;
            case PlayerbotBridge::Refusal::NotInGroup:
                return CommandRefusal::NotInGroup;
            case PlayerbotBridge::Refusal::Unreachable:
                return CommandRefusal::Unreachable;
            case PlayerbotBridge::Refusal::TargetNotVisible:
                return CommandRefusal::TargetNotVisible;
            case PlayerbotBridge::Refusal::NoTarget:
                return CommandRefusal::NoTarget;
            case PlayerbotBridge::Refusal::CannotNow:
                return CommandRefusal::CannotNow;
            case PlayerbotBridge::Refusal::NotPlanner:
                return CommandRefusal::NotPlanner;
            case PlayerbotBridge::Refusal::GoalHeld:
                return CommandRefusal::GoalHeld;
            case PlayerbotBridge::Refusal::UnknownPlace:
                return CommandRefusal::UnknownPlace;
            case PlayerbotBridge::Refusal::NoRoute:
                return CommandRefusal::NoRoute;
            case PlayerbotBridge::Refusal::NoSuchQuest:
                return CommandRefusal::NoSuchQuest;
            case PlayerbotBridge::Refusal::NothingHere:
                return CommandRefusal::NothingHere;
            case PlayerbotBridge::Refusal::AlreadyThere:
                return CommandRefusal::AlreadyThere;
            case PlayerbotBridge::Refusal::UnknownName:
                return CommandRefusal::UnknownName;
            case PlayerbotBridge::Refusal::None:
                break;
        }
        return CommandRefusal::None;
    }

    // J5e: `why` receives the reason of every refusal; the channel words a Rejected without one as cannot_now.
    static EffectResult Effect(EffectPhase const& phase, WorldTicket const& ticket, DispatchGuard const& guard,
                               ChannelCommand const& command, std::function<bool()> const& authorize,
                               CommandRefusal& why)
    {
        // No game read off the joined world thread or for a verb without an accepted effect.
        if (!EffectSupported(command.intent) || !phase.join.Current())
        {
            why = EffectSupported(command.intent) ? CommandRefusal::Unavailable : CommandRefusal::NotSupported;
            return EffectResult::Rejected;
        }
        // Bot and partner come from the registered scope, never from the wire; Receive matched the issuer. DS.14: the
        // partner needs only to be online here; playerbots checks what each verb needs of it (follow: the bot's map).
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(ticket.scope.botGuid));
        Player* partner = ObjectAccessor::FindConnectedPlayer(ObjectGuid(ticket.scope.controllerGuid));
        if (!bot || !bot->IsInWorld() || !partner)
        {
            why = CommandRefusal::Unavailable;
            return EffectResult::Rejected;
        }

        float const distance = g_SayDistance;  // The whisper/party radius BridgeCapture uses.
        IntentContext facts;
        facts.controllerGuid = ticket.scope.controllerGuid;
        facts.botGuid = ticket.scope.botGuid;
        facts.expectedGroupGuid = guard.groupGuid;
        facts.controllerGroupGuid = GroupGuid(partner);
        facts.botGroupGuid = GroupGuid(bot);
        if (command.intent == ChannelIntent::AttackOwnTarget)
            facts.ownTargetGuid = bot->GetTarget().GetRawValue();
        // A position becomes a fact only where playerbots finds a short complete path from the bot to a spot near it
        // (one read-only path query; the partner plays no part). MapIntent then requires the wire position to equal it
        // exactly, and playerbots checks it again after authorization.
        if (auto const* candidate =
                command.intent == ChannelIntent::Position ? std::get_if<ChannelPosition>(&command.params) : nullptr)
        {
            PlayerbotBridge::Order probe;
            probe.Operation = PlayerbotBridge::Intent::Position;
            probe.Bot = ticket.scope.botGuid;
            probe.Controller = ticket.scope.controllerGuid;
            probe.VisibilityDistance = distance;
            probe.MapId = candidate->mapId;
            probe.X = candidate->x;
            probe.Y = candidate->y;
            probe.Z = candidate->z;
            if (PlayerbotBridge::PositionReachable(phase.join, probe))
                facts.pathableNearbyPosition = *candidate;
        }
        if (uint64_t const candidate = EffectTargetGuid(command, facts.ownTargetGuid))
        {
            Unit* unit = ObjectAccessor::GetUnit(*bot, ObjectGuid(candidate));
            // DS.14: the bot acts on its own perception; the partner need not see the target.
            if (VisibleUnit(bot, unit, distance))
                facts.eligibleVisibleTargetGuid = candidate;
        }
        auto const effect = BuildEffectOrder(command, facts, &why);
        if (!effect)
            return EffectResult::Rejected;

        PlayerbotBridge::Order order;
        switch (effect->intent)
        {
            case ChannelIntent::AttackOwnTarget:
                order.Operation = PlayerbotBridge::Intent::AttackOwnTarget;
                break;
            case ChannelIntent::Mark:
                order.Operation = PlayerbotBridge::Intent::Mark;
                break;
            case ChannelIntent::Follow:
                order.Operation = PlayerbotBridge::Intent::Follow;
                break;
            case ChannelIntent::Stay:
                order.Operation = PlayerbotBridge::Intent::Stay;
                break;
            case ChannelIntent::Flee:  // Flees from the bot's own current target, resolved by playerbots.
                order.Operation = PlayerbotBridge::Intent::Flee;
                break;
            case ChannelIntent::Position:
                order.Operation = PlayerbotBridge::Intent::Position;
                break;
            case ChannelIntent::Formation:
                order.Operation = PlayerbotBridge::Intent::Formation;
                order.Shape = Shape(effect->shape);
                break;
            case ChannelIntent::SaveMana:
                order.Operation = PlayerbotBridge::Intent::SaveMana;
                order.Enabled = effect->enabled;
                break;
            case ChannelIntent::FocusHealing:
                order.Operation = PlayerbotBridge::Intent::FocusHealing;
                break;
            case ChannelIntent::GoTo:  // WT18b: the goal verbs; the planner checks the place, quest or spot.
                order.Operation = PlayerbotBridge::Intent::GoTo;
                order.PlaceName = effect->place;
                break;
            case ChannelIntent::Quest:
                order.Operation = PlayerbotBridge::Intent::Quest;
                order.QuestId = effect->questId;
                break;
            case ChannelIntent::Errands:
                order.Operation = PlayerbotBridge::Intent::Errands;
                break;
            case ChannelIntent::SetHearth:
                order.Operation = PlayerbotBridge::Intent::SetHearth;
                break;
            case ChannelIntent::GrindHere:
                order.Operation = PlayerbotBridge::Intent::GrindHere;
                break;
            case ChannelIntent::GatherHere:
                order.Operation = PlayerbotBridge::Intent::GatherHere;
                break;
            case ChannelIntent::MeetAt:
                order.Operation = PlayerbotBridge::Intent::MeetAt;
                order.PlaceName = effect->place;
                break;
            case ChannelIntent::PausePlan:
                order.Operation = PlayerbotBridge::Intent::PausePlan;
                break;
            case ChannelIntent::ResumePlan:
                order.Operation = PlayerbotBridge::Intent::ResumePlan;
                break;
            default:
                why = CommandRefusal::NotSupported;
                return EffectResult::Rejected;  // An unmapped verb never reaches playerbots.
        }
        order.Bot = effect->botGuid;
        order.Controller = effect->controllerGuid;
        order.Group = effect->groupGuid;
        order.Target = effect->targetGuid;
        order.Icon = effect->icon;
        order.VisibilityDistance = distance;
        order.MapId = effect->position.mapId;
        order.X = effect->position.x;
        order.Y = effect->position.y;
        order.Z = effect->position.z;
        // J12: a lasting verb needs the ledger's admission and the bot's current state of that part before every
        // attempt, a repeat too (the ledger compares it with its last applied value to see a person's change); no
        // admission or no capture, no mutation. One clock reading serves admission and record.
        auto const kind = PlanKindFor(effect->intent);
        PlanKey const key{effect->botGuid, kind.value_or(PlanKind::Count)};
        PlanOwner const owner{effect->groupGuid, effect->controllerGuid, ticket.generation,
                              IntentNeedsPartnerInGroup(effect->intent)};
        int64_t const nowMs = Now().steadyMs;
        PlayerbotBridge::Baseline before;
        if (kind && Plans().Admit(key, owner, nowMs) == PlanAdmission::Refused)
        {
            why = CommandRefusal::Busy;
            return EffectResult::Rejected;
        }
        if (kind && !PlayerbotBridge::Capture(phase.join, effect->botGuid,
                                              PlayerbotBridge::LastingPart(order.Operation), before))
        {
            why = CommandRefusal::CannotNow;
            return EffectResult::Rejected;
        }
        PlayerbotBridge::Refusal refusal = PlayerbotBridge::Refusal::None;
        auto const outcome = PlayerbotBridge::Execute(phase.join, order, authorize, &refusal);
        // After every attempt that may have written (Applied, or Failed after partial writes), the state now is what
        // the bridge set; Rejected and Unavailable promise no mutation and record nothing. An attempt left unrecorded
        // (the bot left the world inside the call, or the ledger refused; neither can happen today) has no restore,
        // so it is counted for the watchlist line.
        if (kind && (outcome == PlayerbotBridge::Result::Applied || outcome == PlayerbotBridge::Result::Failed))
        {
            PlayerbotBridge::Baseline applied;
            if (!PlayerbotBridge::Capture(phase.join, effect->botGuid, before.Held, applied) ||
                !Plans().Record(key, owner, before, applied, nowMs))
                Plans().NoteUnrecorded();
        }
        // DS.10: a mark that applied or may have applied re-baselines only this bot's stored marks, so the bot does
        // not react to its own mark change; other bots in the group still observe it. Rejected and Unavailable
        // promise no mutation.
        if (effect->intent == ChannelIntent::Mark && outcome != PlayerbotBridge::Result::Rejected &&
            outcome != PlayerbotBridge::Result::Unavailable)
            OllamaChat_BridgeNoteOwnMark(ticket.scope.botGuid);
        switch (outcome)
        {
            case PlayerbotBridge::Result::Rejected:
                why = OrderRefusal(refusal);
                return EffectResult::Rejected;
            case PlayerbotBridge::Result::Unavailable:
                return EffectResult::Unavailable;
            case PlayerbotBridge::Result::Applied:
                return EffectResult::Applied;
            case PlayerbotBridge::Result::Failed:
                break;
        }
        return EffectResult::Failed;  // Failed or unknown after a possible attempt: never applied, never replayed.
    }

    // LC3: a step's outcome in the words of playerbots' table (the catalogue's); a refusal without a reason, an
    // unavailable verb and a failure in the module's (DescribeRefusal), as the old form words them.
    static ChannelStepResult StepResult(PlayerbotBridge::StepOutcome const& step)
    {
        auto const module = [](ChannelOutcome outcome, CommandRefusal refusal)
        {
            auto const words = DescribeRefusal(refusal);
            return ChannelStepResult{outcome, words.code, words.text};
        };
        switch (step.Outcome)
        {
            case PlayerbotBridge::Result::Applied:
                return {ChannelOutcome::Applied, "", ""};
            case PlayerbotBridge::Result::Failed:
                return module(ChannelOutcome::Failed, CommandRefusal::Failed);
            case PlayerbotBridge::Result::Unavailable:
                return module(ChannelOutcome::Rejected, CommandRefusal::Unavailable);
            case PlayerbotBridge::Result::Rejected:
                break;
        }
        if (step.Why == PlayerbotBridge::Refusal::None)
            return module(ChannelOutcome::Rejected, CommandRefusal::CannotNow);
        auto const& words = PlayerbotBridge::RefusalWords(step.Why);
        return {ChannelOutcome::Rejected, words.Word, words.Sentence};
    }

    // LC3: a stepped command as one playerbots order of its steps: one Execute and so one authorize, with playerbots'
    // gate on every step (the group, a named or given unit or quest seen or held, the row's check; a position's path).
    // The module's MapIntent facts are not read again here. It keeps the scope (authorize), the captured group and the
    // plan ledger: every lasting step is admitted and its part captured before Execute, and each one that ran and may
    // have written is recorded after it. Two steps of one lasting part are refused before anything runs: the second's
    // baseline would be the first one's work. `ran` receives one worded entry per step that ran.
    static EffectResult Stepped(EffectPhase const& phase, WorldTicket const& ticket, DispatchGuard const& guard,
                                ChannelCommand const& command, std::function<bool()> const& authorize,
                                CommandRefusal& why, std::vector<ChannelStepResult>& ran)
    {
        if (!phase.join.Current())
        {
            why = CommandRefusal::Unavailable;
            return EffectResult::Rejected;
        }
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(ticket.scope.botGuid));
        Player* partner = ObjectAccessor::FindConnectedPlayer(ObjectGuid(ticket.scope.controllerGuid));
        if (!bot || !bot->IsInWorld() || !partner)
        {
            why = CommandRefusal::Unavailable;
            return EffectResult::Rejected;
        }
        struct Lasting
        {
            std::size_t step = 0;
            PlanKey key;
            PlanOwner owner;
            PlayerbotBridge::Part part = PlayerbotBridge::Part::None;
            PlayerbotBridge::Baseline before;
        };
        std::vector<PlayerbotBridge::Order> orders;
        std::vector<Lasting> lasting;
        for (std::size_t index = 0; index < command.steps.size(); ++index)
        {
            ChannelStep const& step = command.steps[index];
            PlayerbotBridge::Order order;
            // The row's playerbots Intent value (handler.cpp asserts the 22 old verbs' values are equal).
            order.Operation = static_cast<PlayerbotBridge::Intent>(step.intent);
            order.Bot = ticket.scope.botGuid;
            order.Controller = ticket.scope.controllerGuid;
            order.Group = guard.groupGuid;
            order.VisibilityDistance = g_SayDistance;  // The whisper/party radius BridgeCapture uses.
            order.Target = step.targetGuid;
            order.TargetName = step.targetName;
            order.QuestId = step.questId;
            order.QuestName = step.questName;
            order.PlaceName = step.place;
            if (step.position)
            {
                order.MapId = step.position->mapId;
                order.X = step.position->x;
                order.Y = step.position->y;
                order.Z = step.position->z;
            }
            order.Icon = step.icon.value_or(0);
            // OrderField::Shape: the index of the choice plus one (None without a shape).
            order.Shape = step.shape ? static_cast<PlayerbotBridge::FormationShape>(*step.shape + 1)
                                     : PlayerbotBridge::FormationShape::None;
            order.Enabled = step.enabled.value_or(false);
            order.ItemName = step.itemName;  // LC16c: OrderField ItemName, SpellName, Choice (its index), Count.
            order.SpellName = step.spellName;
            order.Choice = step.choice.value_or(0);
            order.Count = step.count;
            auto const part = PlayerbotBridge::LastingPart(order.Operation);
            auto const kind = PlanKindFor(step.intent);
            orders.push_back(std::move(order));
            if (part == PlayerbotBridge::Part::None && !kind)
                continue;
            // A lasting part needs a ledger kind to be restored, and one step per part.
            if (part == PlayerbotBridge::Part::None || !kind ||
                std::any_of(lasting.begin(), lasting.end(),
                            [&](Lasting const& entry) { return entry.key.kind == *kind; }))
            {
                why = CommandRefusal::CannotNow;
                return EffectResult::Rejected;
            }
            lasting.push_back({index,
                               {ticket.scope.botGuid, *kind},
                               {guard.groupGuid, ticket.scope.controllerGuid, ticket.generation,
                                IntentNeedsPartnerInGroup(step.intent)},
                               part,
                               {}});
        }
        // J12 per lasting step: every admission first, then every capture; no admission or no capture, no mutation.
        // One clock reading serves admission and record.
        int64_t const nowMs = Now().steadyMs;
        for (Lasting const& entry : lasting)
            if (Plans().Admit(entry.key, entry.owner, nowMs) == PlanAdmission::Refused)
            {
                why = CommandRefusal::Busy;
                return EffectResult::Rejected;
            }
        for (Lasting& entry : lasting)
            if (!PlayerbotBridge::Capture(phase.join, ticket.scope.botGuid, entry.part, entry.before))
            {
                why = CommandRefusal::CannotNow;
                return EffectResult::Rejected;
            }
        std::vector<PlayerbotBridge::StepOutcome> outcomes;
        auto const result =
            PlayerbotBridge::Execute(phase.join, std::span<PlayerbotBridge::Order const>(orders), authorize, &outcomes);
        auto const wrote = [&outcomes](std::size_t index)
        {
            return index < outcomes.size() && (outcomes[index].Outcome == PlayerbotBridge::Result::Applied ||
                                               outcomes[index].Outcome == PlayerbotBridge::Result::Failed);
        };
        // A lasting step that ran and may have written is recorded with the part as the order left it; a step that
        // did not run, or was refused, records nothing.
        for (Lasting const& entry : lasting)
        {
            if (!wrote(entry.step))
                continue;
            PlayerbotBridge::Baseline applied;
            if (!PlayerbotBridge::Capture(phase.join, ticket.scope.botGuid, entry.before.Held, applied) ||
                !Plans().Record(entry.key, entry.owner, entry.before, applied, nowMs))
                Plans().NoteUnrecorded();
        }
        for (std::size_t index = 0; index < outcomes.size(); ++index)
        {
            // DS.10: a mark that applied or may have applied re-baselines this bot's stored marks.
            if (command.steps[index].intent == ChannelIntent::Mark && wrote(index))
                OllamaChat_BridgeNoteOwnMark(ticket.scope.botGuid);
            ran.push_back(StepResult(outcomes[index]));
        }
        switch (result)
        {
            case PlayerbotBridge::Result::Rejected:
                why = outcomes.empty() ? CommandRefusal::None : OrderRefusal(outcomes.back().Why);
                return EffectResult::Rejected;
            case PlayerbotBridge::Result::Unavailable:
                why = CommandRefusal::Unavailable;
                return EffectResult::Unavailable;
            case PlayerbotBridge::Result::Applied:
                return EffectResult::Applied;
            case PlayerbotBridge::Result::Failed:
                break;
        }
        return EffectResult::Failed;  // Failed or unknown after a possible attempt: never applied, never replayed.
    }

    CommandRefusal _refusal = CommandRefusal::None;
    std::vector<ChannelStepResult> _steps;
};
}  // namespace
}  // namespace OllamaBridge

OllamaBridge::CommandExecutor& OllamaChat_BridgeCommandExecutor()
{
    static OllamaBridge::BridgeCommandExecutor executor;
    return executor;
}

void OllamaChat_BridgePlansOpen(uint64_t generation) { OllamaBridge::BridgeCommandExecutor::Plans().Open(generation); }

void OllamaChat_BridgePlansRetire() { OllamaBridge::BridgeCommandExecutor::Plans().RetireAll(); }

void OllamaChat_BridgePlansUpdate(OllamaBridge::EffectPhase const& phase)
{
    OllamaBridge::BridgeCommandExecutor::UpdatePlans(phase, OllamaBridge::Now().steadyMs);
}

bool OllamaChat_BridgePlansEmpty() { return OllamaBridge::BridgeCommandExecutor::Plans().Empty(); }

OllamaBridge::PlanCounters OllamaChat_BridgePlanCounters()
{
    return OllamaBridge::BridgeCommandExecutor::Plans().Counters();
}

uint8_t OllamaChat_BridgePlanKinds(uint64_t botGuid)
{
    using OllamaBridge::PlanKind;
    // The bits of OllamaBridge::BotState::plans follow PlanKind.
    static_assert(OllamaBridge::StatePlanMovement == 1u << static_cast<unsigned>(PlanKind::Movement) &&
                  OllamaBridge::StatePlanFormation == 1u << static_cast<unsigned>(PlanKind::Formation) &&
                  OllamaBridge::StatePlanSaveMana == 1u << static_cast<unsigned>(PlanKind::SaveMana) &&
                  OllamaBridge::StatePlanFocusHealing == 1u << static_cast<unsigned>(PlanKind::FocusHealing) &&
                  static_cast<unsigned>(PlanKind::Count) == 4);
    uint8_t kinds = 0;
    for (uint8_t kind = 0; kind < static_cast<uint8_t>(PlanKind::Count); ++kind)
        if (OllamaBridge::BridgeCommandExecutor::Plans().LiveApplied({botGuid, static_cast<PlanKind>(kind)}))
            kinds |= static_cast<uint8_t>(1u << kind);
    return kinds;
}
