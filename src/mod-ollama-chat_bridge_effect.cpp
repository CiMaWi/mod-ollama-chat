#include "mod-ollama-chat_bridge_effect.h"

#include "BridgeIntentEffect.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"

// World-only production typed effect adapter. WorldChannel::Update calls it synchronously with the post-map-join
// phase, after the channel's epoch, route, deadline, rate and replay joins. It resolves every game fact afresh by
// GUID, keeps no pointer, phase or callback past the call and hands one typed order to the playerbots boundary,
// which calls authorize once immediately before its single mutation.
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
        // Release gate (flipped only by J6b4): refuse before any game read, authorization or playerbots call.
        if constexpr (!kBridgeEffectsReleased)
            return EffectResult::Rejected;
        else
            return Effect(phase, ticket, guard, command, authorize);
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

    static EffectResult Effect(EffectPhase const& phase, WorldTicket const& ticket, DispatchGuard const& guard,
                               ChannelCommand const& command, std::function<bool()> const& authorize)
    {
        // No game read off the joined world thread or for a verb without an accepted effect.
        if (!EffectSupported(command.intent) || !phase.join.Current())
            return EffectResult::Rejected;
        // Bot and controller come from the registered scope, never from the wire; Receive matched the issuer.
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(ticket.scope.botGuid));
        Player* controller = ObjectAccessor::FindConnectedPlayer(ObjectGuid(ticket.scope.controllerGuid));
        if (!bot || !bot->IsInWorld() || !controller || !controller->IsInWorld())
            return EffectResult::Rejected;

        float const distance = g_SayDistance;  // The whisper/party radius BridgeCapture uses.
        IntentContext facts;
        facts.controllerGuid = ticket.scope.controllerGuid;
        facts.botGuid = ticket.scope.botGuid;
        facts.expectedGroupGuid = guard.groupGuid;
        facts.controllerGroupGuid = GroupGuid(controller);
        facts.botGroupGuid = GroupGuid(bot);
        if (command.intent == ChannelIntent::AttackOwnTarget)
            facts.ownTargetGuid = bot->GetTarget().GetRawValue();
        // pathableNearbyPosition stays closed: no delivered verb carries a position.
        if (uint64_t const candidate = EffectTargetGuid(command, facts.ownTargetGuid))
        {
            Unit* unit = ObjectAccessor::GetUnit(*bot, ObjectGuid(candidate));
            if (VisibleUnit(bot, unit, distance) && VisibleUnit(controller, unit, distance))
                facts.eligibleVisibleTargetGuid = candidate;
        }
        auto const effect = BuildEffectOrder(command, facts);
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
            default:
                return EffectResult::Rejected;  // An unmapped verb never reaches playerbots.
        }
        order.Bot = effect->botGuid;
        order.Controller = effect->controllerGuid;
        order.Group = effect->groupGuid;
        order.Target = effect->targetGuid;
        order.Icon = effect->icon;
        order.VisibilityDistance = distance;
        switch (PlayerbotBridge::Execute(phase.join, order, authorize))
        {
            case PlayerbotBridge::Result::Rejected:
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
};
}  // namespace
}  // namespace OllamaBridge

OllamaBridge::CommandExecutor& OllamaChat_BridgeCommandExecutor()
{
    static OllamaBridge::BridgeCommandExecutor executor;
    return executor;
}
