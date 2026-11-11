#ifndef MOD_OLLAMA_CHAT_BRIDGE_EFFECT_H
#define MOD_OLLAMA_CHAT_BRIDGE_EFFECT_H

#include <algorithm>
#include <functional>
#include <optional>
#include <unordered_map>
#include <variant>

#include "mod-ollama-chat_bridge_channel.h"

// Pure typed-command seam: no game, playerbots or transport types. The world channel decides verb support,
// rate, epoch, route, deadline and authority; a world-only executor supplies the one game effect.
namespace OllamaBridge
{
// Typed effects are on since J6b4 (DS.5); the bridge channel enable is the only switch.
constexpr bool kBridgeEffectsReleased = true;

struct WorldTicket;
struct DispatchGuard;
// Opaque post-map-join world phase. Only the world tick constructs one; nothing stores it past the call.
struct EffectPhase;

enum class EffectResult : uint8_t
{
    Rejected,     // No mutation: refused before or at the final currentness check.
    Unavailable,  // No mutation: the game side has no safe effect for this verb.
    Applied,
    Failed  // Attempted after authorization; may follow partial writes and never grants a retry.
};

// DS.3: only verbs with an accepted typed game effect reach an executor. Every other verb (regroup, pull,
// assign_role, dungeon_clear: no safe playerbots effect, DS.9) gets the fixed refusal here, before any game read.
// Attack, mark, flee and position are one-time; follow, stay, formation, save_mana and focus_healing are lasting
// plans (J12): the executor captures the bot's state first and the plan ledger restores it later. WT18b: the goal
// verbs hand the planner a goal; it carries the goal out, so they have no lasting part and no restore.
inline bool EffectSupported(ChannelIntent intent)
{
    return intent == ChannelIntent::AttackOwnTarget || intent == ChannelIntent::Mark ||
           intent == ChannelIntent::Follow || intent == ChannelIntent::Stay || intent == ChannelIntent::Flee ||
           intent == ChannelIntent::Position || intent == ChannelIntent::Formation ||
           intent == ChannelIntent::SaveMana || intent == ChannelIntent::FocusHealing ||
           intent == ChannelIntent::GoTo || intent == ChannelIntent::Quest || intent == ChannelIntent::Errands ||
           intent == ChannelIntent::SetHearth || intent == ChannelIntent::GrindHere ||
           intent == ChannelIntent::GatherHere || intent == ChannelIntent::MeetAt ||
           intent == ChannelIntent::PausePlan || intent == ChannelIntent::ResumePlan;
}

// Rejected and Unavailable promise no mutation. Only an attempt the channel authorized can be applied; any other
// claimed attempt is reported as failed, never as applied or as a pre-effect refusal.
inline ChannelOutcome EffectOutcome(EffectResult result, bool authorized)
{
    if (result == EffectResult::Rejected || result == EffectResult::Unavailable)
        return ChannelOutcome::Rejected;
    return authorized && result == EffectResult::Applied ? ChannelOutcome::Applied : ChannelOutcome::Failed;
}

// Plain typed order for the game boundary, built only from MapIntent over fresh facts and the captured group.
struct EffectOrder
{
    ChannelIntent intent = ChannelIntent::Stay;
    uint64_t botGuid = 0;
    uint64_t controllerGuid = 0;
    uint64_t groupGuid = 0;
    uint64_t targetGuid = 0;
    uint8_t icon = 0;
    ChannelPosition position;           // Position only: the destination MapIntent matched against the pathable fact.
    std::optional<ChannelShape> shape;  // Formation only.
    bool enabled = false;               // SaveMana only.
    std::string place;                  // GoTo and MeetAt only: the place name.
    uint32_t questId = 0;               // Quest only.
};

// The one unit an executor resolves for visibility: the bot's current own target for an attack, the payload's
// target for a mark or a focus heal. Zero resolves nothing, so the target fact stays closed.
inline uint64_t EffectTargetGuid(ChannelCommand const& command, uint64_t ownTargetGuid)
{
    if (command.intent == ChannelIntent::AttackOwnTarget)
        return ownTargetGuid;
    if (command.intent == ChannelIntent::FocusHealing)
    {
        auto const* target = std::get_if<ChannelTarget>(&command.params);
        return target ? target->guid : 0;
    }
    auto const* mark = command.intent == ChannelIntent::Mark ? std::get_if<ChannelMark>(&command.params) : nullptr;
    return mark ? mark->guid : 0;
}

// A supported verb mapped from freshly resolved facts with the captured group (0: an ungrouped bot; MapIntent says
// which verbs need one); otherwise none. An attack, a mark or a focus heal needs a nonzero target, a target-less verb
// must carry none; a position carries its checked destination, a formation its shape and save_mana its flag.
inline std::optional<EffectOrder> BuildEffectOrder(ChannelCommand const& command, IntentContext const& current,
                                                   CommandRefusal* why = nullptr)
{
    if (!EffectSupported(command.intent))
    {
        if (why)
            *why = CommandRefusal::NotSupported;
        return std::nullopt;
    }
    auto const mapped = MapIntent(command, current, why);
    if (!mapped)
        return std::nullopt;
    EffectOrder order;
    order.intent = mapped->intent;
    order.botGuid = mapped->botGuid;
    order.controllerGuid = mapped->controllerGuid;
    order.groupGuid = current.expectedGroupGuid;
    if (auto const* target = std::get_if<ChannelTarget>(&mapped->params))
        order.targetGuid = target->guid;
    else if (auto const* mark = std::get_if<ChannelMark>(&mapped->params))
    {
        order.targetGuid = mark->guid;
        order.icon = mark->icon;
    }
    else if (auto const* position = std::get_if<ChannelPosition>(&mapped->params))
        order.position = *position;
    else if (auto const* shape = std::get_if<ChannelShape>(&mapped->params))
        order.shape = *shape;
    else if (auto const* enabled = std::get_if<bool>(&mapped->params))
        order.enabled = *enabled;
    else if (auto const* place = std::get_if<ChannelPlace>(&mapped->params))
        order.place = place->name;
    else if (auto const* quest = std::get_if<ChannelQuestId>(&mapped->params))
        order.questId = quest->id;
    bool const targeted = order.intent == ChannelIntent::AttackOwnTarget || order.intent == ChannelIntent::Mark ||
                          order.intent == ChannelIntent::FocusHealing;
    if (targeted ? !order.targetGuid : order.targetGuid != 0)
        return std::nullopt;
    return order;
}

// World-only and synchronous. Execute calls authorize once immediately before any mutation and mutates nothing
// when it returns false; the channel grants at most one authorization per command. It retains no phase, ticket,
// guard, command, callback or game pointer, and never calls back into the channel.
class CommandExecutor
{
public:
    virtual ~CommandExecutor() = default;
    virtual EffectResult Execute(EffectPhase const& phase, WorldTicket const& ticket, DispatchGuard const& guard,
                                 ChannelCommand const& command, std::function<bool()> const& authorize) = 0;
    // J5e: the reason of the last Execute's refusal (None when unknown); read right after Execute, world thread.
    virtual CommandRefusal Refusal() const { return CommandRefusal::None; }
    // LC3: the last Execute's steps that ran, in order and worded (empty for the old form or when none ran).
    virtual std::vector<ChannelStepResult> Steps() const { return {}; }
};

// Bounded per-bot gate on the game-owned lane, independent of request and command ids and of channel epochs.
// A steady high-water mark keeps clock rollback from reopening an interval. Entries whose interval has passed
// are evicted only when the table is full, so a scope or epoch change never reopens a bot's interval early.
class CommandRate
{
public:
    static constexpr std::size_t MaxBots = 64;
    static constexpr int64_t IntervalMs = 2000;

    bool Admit(uint64_t botGuid, Clock now)
    {
        if (!botGuid || now.steadyMs < 0)
            return false;
        _high = std::max(_high, now.steadyMs);
        auto const found = _admitted.find(botGuid);
        if (found != _admitted.end())
        {
            if (_high - found->second < IntervalMs)
                return false;
            found->second = _high;
            return true;
        }
        if (_admitted.size() >= MaxBots)
            std::erase_if(_admitted, [&](auto const& entry) { return _high - entry.second >= IntervalMs; });
        if (_admitted.size() >= MaxBots)
            return false;
        _admitted.emplace(botGuid, _high);
        return true;
    }
    std::size_t Size() const { return _admitted.size(); }

private:
    int64_t _high = 0;
    std::unordered_map<uint64_t, int64_t> _admitted;
};

// World-thread counters for the smoke-test watchlist. No ids or payload text are recorded.
struct CommandStats
{
    uint64_t proposed = 0;    // Admitted command ids; the replay ledger consumed each one exactly once.
    uint64_t duplicates = 0;  // Replayed command ids refused at admission, before any executor.
    uint64_t refused = 0;     // Internal outcome rejected: fixed refusal, rate, lost authority or executor refusal.
    uint64_t attempts = 0;    // Executor invocations.
    uint64_t applied = 0;
    uint64_t failed = 0;
    uint64_t withheld = 0;  // Decided outcomes whose receipt was not published; never a retry.

    bool operator==(CommandStats const&) const = default;
};
}  // namespace OllamaBridge

#endif
