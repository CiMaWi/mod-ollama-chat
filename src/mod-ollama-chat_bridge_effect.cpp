#include "mod-ollama-chat_bridge_effect.h"

#include "BridgeAbilities.h"
#include "BridgeIntentEffect.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"

// World-only production typed effect adapter. WorldChannel::Update calls it synchronously with the post-map-join
// phase, after the channel's epoch, route, deadline, rate and replay joins. It resolves every game fact afresh by
// GUID, keeps no pointer, phase or callback past the call and hands one typed order to the playerbots boundary,
// which calls authorize once immediately before its single mutation. LC.36: follow, stay, formation, save mana and
// focus heal are actions the bot takes itself; they hold until the bot changes them or playerbots' own group and
// master resets end them, so the module keeps no record of them and restores nothing.
namespace OllamaBridge
{
namespace
{
class BridgeCommandExecutor final : public CommandExecutor
{
public:
    EffectResult Execute(EffectPhase const& phase, WorldTicket const& ticket, DispatchGuard const& guard,
                         ChannelCommand const& command, std::function<bool()> const& authorize) override
    {
        _refusal = CommandRefusal::None;
        _steps.clear();
        return Stepped(phase, ticket, guard, command, authorize, _refusal, _steps);
    }

    CommandRefusal Refusal() const override { return _refusal; }
    std::vector<ChannelStepResult> Steps() const override { return _steps; }

private:
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

    // LC3: a step's outcome in the words of playerbots' table (the catalogue's); a refusal without a reason, an
    // unavailable verb and a failure in the module's (DescribeRefusal).
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
    // The module keeps the scope (authorize) and the captured group; a lasting step runs like any other (LC.36).
    // `ran` receives one worded entry per step that ran.
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
        std::vector<PlayerbotBridge::Order> orders;
        for (ChannelStep const& step : command.steps)
        {
            PlayerbotBridge::Order order;
            order.Operation = step.intent;  // The row's playerbots Intent value.
            order.Bot = ticket.scope.botGuid;
            order.Controller = ticket.scope.controllerGuid;
            order.Group = guard.groupGuid;
            order.Target = step.targetGuid;
            order.TargetName = step.targetName;
            order.QuestId = step.questId;
            order.QuestName = step.questName;
            order.PlaceName = step.place;
            order.Icon = step.icon.value_or(0);
            // OrderField::Shape: the index of the choice plus one (None without a shape).
            order.Shape = step.shape ? static_cast<PlayerbotBridge::FormationShape>(*step.shape + 1)
                                     : PlayerbotBridge::FormationShape::None;
            order.Enabled = step.enabled.value_or(false);
            order.ItemName = step.itemName;  // LC16c: OrderField ItemName, SpellName, Choice (its index), Count.
            order.SpellName = step.spellName;
            order.Choice = step.choice.value_or(0);
            order.Count = step.count;
            orders.push_back(std::move(order));
        }
        std::vector<PlayerbotBridge::StepOutcome> outcomes;
        auto const result =
            PlayerbotBridge::Execute(phase.join, std::span<PlayerbotBridge::Order const>(orders), authorize, &outcomes);
        for (PlayerbotBridge::StepOutcome const& outcome : outcomes)
            ran.push_back(StepResult(outcome));
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
