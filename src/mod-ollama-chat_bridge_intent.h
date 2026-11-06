#ifndef MOD_OLLAMA_CHAT_BRIDGE_INTENT_H
#define MOD_OLLAMA_CHAT_BRIDGE_INTENT_H

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace OllamaBridge
{
enum class ChannelIntent : uint8_t
{
    Follow,
    Stay,
    Flee,
    Regroup,
    AttackOwnTarget,
    Pull,
    Mark,
    FocusHealing,
    AssignRole,
    SaveMana,
    Formation,
    Position,
    DungeonClear
};

enum class ChannelRole : uint8_t
{
    Tank,
    Healer,
    Damage
};
enum class ChannelShape : uint8_t
{
    Close,
    Spread,
    Line
};
enum class ChannelMode : uint8_t
{
    On,
    Off,
    Pause
};

struct ChannelTarget
{
    uint64_t guid = 0;
};
struct ChannelMark
{
    uint64_t guid = 0;
    uint8_t icon = 0;
};
struct ChannelPosition
{
    double x = 0;
    double y = 0;
    double z = 0;
    uint32_t mapId = 0;

    bool operator==(ChannelPosition const&) const = default;
};
using ChannelParams = std::variant<std::monostate, ChannelTarget, ChannelMark, ChannelRole, bool, ChannelShape,
                                   ChannelPosition, ChannelMode>;

struct ChannelCommand
{
    std::string commandId;
    uint64_t issuerGuid = 0;
    uint64_t botGuid = 0;
    ChannelIntent intent = ChannelIntent::Stay;
    ChannelParams params;
};

// Only the game adapter supplies these facts. A missing fact leaves the associated check closed.
struct IntentContext
{
    uint64_t controllerGuid = 0;  // Current human controller after the game's control/security check.
    uint64_t botGuid = 0;
    uint64_t expectedGroupGuid = 0;  // Group captured with the registered work.
    uint64_t controllerGroupGuid = 0;
    uint64_t botGroupGuid = 0;
    uint64_t ownTargetGuid = 0;              // Current own target for AttackOwnTarget, independent of the wire payload.
    uint64_t eligibleVisibleTargetGuid = 0;  // Resolved, controller-observable and eligible for this intent.
    std::optional<ChannelPosition> pathableNearbyPosition;  // Exact position checked in the current map and phase.
};

struct IntentOrder
{
    uint64_t controllerGuid;
    uint64_t botGuid;
    ChannelIntent intent;
    ChannelParams params;  // AttackOwnTarget carries a resolved ChannelTarget here.
};

inline bool ValidIntentParams(ChannelIntent intent, ChannelParams const& params)
{
    switch (intent)
    {
        case ChannelIntent::Follow:
        case ChannelIntent::Stay:
        case ChannelIntent::Flee:
        case ChannelIntent::Regroup:
        case ChannelIntent::AttackOwnTarget:
            return std::holds_alternative<std::monostate>(params);
        case ChannelIntent::Pull:
        case ChannelIntent::FocusHealing:
        {
            auto const* target = std::get_if<ChannelTarget>(&params);
            return target && target->guid != 0;
        }
        case ChannelIntent::Mark:
        {
            auto const* mark = std::get_if<ChannelMark>(&params);
            return mark && mark->guid != 0 && mark->icon <= 7;
        }
        case ChannelIntent::AssignRole:
        {
            auto const* role = std::get_if<ChannelRole>(&params);
            return role && (*role == ChannelRole::Tank || *role == ChannelRole::Healer || *role == ChannelRole::Damage);
        }
        case ChannelIntent::SaveMana:
            return std::holds_alternative<bool>(params);
        case ChannelIntent::Formation:
        {
            auto const* shape = std::get_if<ChannelShape>(&params);
            return shape &&
                   (*shape == ChannelShape::Close || *shape == ChannelShape::Spread || *shape == ChannelShape::Line);
        }
        case ChannelIntent::Position:
        {
            auto const* position = std::get_if<ChannelPosition>(&params);
            return position && std::isfinite(position->x) && std::isfinite(position->y) && std::isfinite(position->z);
        }
        case ChannelIntent::DungeonClear:
        {
            auto const* mode = std::get_if<ChannelMode>(&params);
            return mode && (*mode == ChannelMode::On || *mode == ChannelMode::Off || *mode == ChannelMode::Pause);
        }
    }
    return false;
}

// Call with freshly resolved facts for an already admitted command. Activation, request lifetime, replay and rate
// checks belong to the channel and world executor. The resulting order carries values for this attempt only.
inline std::optional<IntentOrder> MapIntent(ChannelCommand const& command, IntentContext const& current)
{
    if (!current.controllerGuid || !current.botGuid || current.controllerGuid == current.botGuid ||
        current.controllerGuid != command.issuerGuid || current.botGuid != command.botGuid ||
        !current.expectedGroupGuid || current.controllerGroupGuid != current.expectedGroupGuid ||
        current.botGroupGuid != current.expectedGroupGuid || !ValidIntentParams(command.intent, command.params))
        return std::nullopt;

    if (command.intent == ChannelIntent::AttackOwnTarget)
    {
        if (!current.ownTargetGuid || current.ownTargetGuid != current.eligibleVisibleTargetGuid)
            return std::nullopt;
        return IntentOrder{current.controllerGuid, current.botGuid, command.intent,
                           ChannelTarget{current.ownTargetGuid}};
    }
    if (auto const* target = std::get_if<ChannelTarget>(&command.params))
    {
        if (target->guid != current.eligibleVisibleTargetGuid)
            return std::nullopt;
    }
    if (auto const* mark = std::get_if<ChannelMark>(&command.params))
    {
        if (mark->guid != current.eligibleVisibleTargetGuid)
            return std::nullopt;
    }
    if (auto const* position = std::get_if<ChannelPosition>(&command.params))
    {
        if (!current.pathableNearbyPosition || *position != *current.pathableNearbyPosition)
            return std::nullopt;
    }
    return IntentOrder{current.controllerGuid, current.botGuid, command.intent, command.params};
}
}  // namespace OllamaBridge

#endif
