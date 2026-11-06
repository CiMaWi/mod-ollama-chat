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
// Typed effects stay closed until literal routing, generated speech and the cumulative J6 review are done.
// Only J6b4 flips this; while it is false the production executor refuses every command without a game read.
constexpr bool kBridgeEffectsReleased = false;

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

// DS.3: only verbs with an accepted typed game effect reach an executor. Every other verb gets the fixed refusal
// here, before any game read; lasting-state verbs stay refused until J12.
inline bool EffectSupported(ChannelIntent intent)
{
    return intent == ChannelIntent::AttackOwnTarget || intent == ChannelIntent::Mark;
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
};

// The one unit an executor resolves for visibility: the bot's current own target for an attack, the payload's
// target for a mark. Zero resolves nothing, so the target fact stays closed.
inline uint64_t EffectTargetGuid(ChannelCommand const& command, uint64_t ownTargetGuid)
{
    if (command.intent == ChannelIntent::AttackOwnTarget)
        return ownTargetGuid;
    auto const* mark = command.intent == ChannelIntent::Mark ? std::get_if<ChannelMark>(&command.params) : nullptr;
    return mark ? mark->guid : 0;
}

// A supported verb mapped from freshly resolved facts, with a nonzero target and captured group; otherwise none.
inline std::optional<EffectOrder> BuildEffectOrder(ChannelCommand const& command, IntentContext const& current)
{
    if (!EffectSupported(command.intent))
        return std::nullopt;
    auto const mapped = MapIntent(command, current);
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
    if (!order.targetGuid || !order.groupGuid)
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
