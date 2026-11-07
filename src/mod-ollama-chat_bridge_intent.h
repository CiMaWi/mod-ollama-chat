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
    uint64_t controllerGuid = 0;  // DS.14: the conversation partner (the speaker), not a leader or master.
    uint64_t botGuid = 0;
    uint64_t expectedGroupGuid = 0;  // The bot's group captured with the registered work (0: ungrouped).
    uint64_t controllerGroupGuid = 0;
    uint64_t botGroupGuid = 0;
    uint64_t ownTargetGuid = 0;              // Current own target for AttackOwnTarget, independent of the wire payload.
    uint64_t eligibleVisibleTargetGuid = 0;  // Resolved, visible to the bot and eligible for this intent.
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

// J5e: why an order was refused, so the bot can turn it into its own in-world reply. The wire carries the code and
// the sentence (fixed ASCII, no quotes or backslashes, the bot speaking to its conversation partner).
enum class CommandRefusal : uint8_t
{
    None,
    NotSupported,
    StandingNotSupported,
    TooSoon,
    Unavailable,
    Dead,
    Battleground,
    Travelling,
    NotWithYou,
    TooFar,
    NotInGroup,
    NoTarget,
    TargetNotVisible,
    Unreachable,
    Busy,
    CannotNow,
    Failed
};

struct CommandRefusalWords
{
    char const* code;
    char const* text;
};

inline CommandRefusalWords DescribeRefusal(CommandRefusal refusal)
{
    switch (refusal)
    {
        case CommandRefusal::None:
            return {"", ""};
        case CommandRefusal::NotSupported:
            return {"not_supported", "That is not something I can do on command yet."};
        case CommandRefusal::StandingNotSupported:
            return {"standing_not_supported",
                    "I can follow or stay until you say otherwise, but I cannot take it as a standing order."};
        case CommandRefusal::TooSoon:
            return {"too_soon", "I just took an order; give me a moment."};
        case CommandRefusal::Unavailable:
            return {"unavailable", "I cannot take orders right now."};
        case CommandRefusal::Dead:
            return {"dead", "I am in no state to do anything right now."};
        case CommandRefusal::Battleground:
            return {"battleground", "Not here; I have my own fight to see to."};
        case CommandRefusal::Travelling:
            return {"travelling", "I cannot do that while I am in flight."};
        case CommandRefusal::NotWithYou:
            return {"not_with_you", "We are not travelling together."};
        case CommandRefusal::TooFar:
            return {"too_far", "You are too far away for me to reach."};
        case CommandRefusal::NotInGroup:
            return {"not_in_group", "We are not in the same group."};
        case CommandRefusal::NoTarget:
            return {"no_target", "I have no target to attack."};
        case CommandRefusal::TargetNotVisible:
            return {"target_not_visible", "I cannot see that target from here."};
        case CommandRefusal::Unreachable:
            return {"unreachable", "I cannot get to that spot from here."};
        case CommandRefusal::Busy:
            return {"busy", "I am still finishing an earlier order; ask me again shortly."};
        case CommandRefusal::CannotNow:
            return {"cannot_now", "I cannot do that right now."};
        case CommandRefusal::Failed:
            return {"failed", "I tried, but it did not work."};
    }
    return {"cannot_now", "I cannot do that right now."};
}

// DS.14: group play needs a group. Follow, formation and save_mana also need the partner in the bot's group; a mark
// and a focus heal need only the bot's group (playerbots checks the rest of each verb's rule).
inline bool IntentNeedsGroup(ChannelIntent intent)
{
    return intent == ChannelIntent::Follow || intent == ChannelIntent::Mark || intent == ChannelIntent::Formation ||
           intent == ChannelIntent::SaveMana || intent == ChannelIntent::FocusHealing;
}

inline bool IntentNeedsPartnerInGroup(ChannelIntent intent)
{
    return intent == ChannelIntent::Follow || intent == ChannelIntent::Formation || intent == ChannelIntent::SaveMana;
}

// Call with freshly resolved facts for an already admitted command. Activation, request lifetime, replay and rate
// checks belong to the channel and world executor. The resulting order carries values for this attempt only.
// J5e: `why`, when given, receives the reason of a refusal.
inline std::optional<IntentOrder> MapIntent(ChannelCommand const& command, IntentContext const& current,
                                            CommandRefusal* why = nullptr)
{
    auto const refuse = [why](CommandRefusal reason) -> std::optional<IntentOrder>
    {
        if (why)
            *why = reason;
        return std::nullopt;
    };
    // The partner and the bot are the registered scope's; Receive matched the issuer, so a mismatch is a defect.
    if (!current.controllerGuid || !current.botGuid || current.controllerGuid == current.botGuid ||
        current.controllerGuid != command.issuerGuid || current.botGuid != command.botGuid)
        return refuse(CommandRefusal::CannotNow);
    // DS.14: no leader or master gate. The bot's group must still be the captured one (0: ungrouped then and now).
    if (current.botGroupGuid != current.expectedGroupGuid)
        return refuse(CommandRefusal::CannotNow);
    if (IntentNeedsGroup(command.intent) &&
        (!current.expectedGroupGuid ||
         (IntentNeedsPartnerInGroup(command.intent) && current.controllerGroupGuid != current.expectedGroupGuid)))
        return refuse(command.intent == ChannelIntent::Follow ? CommandRefusal::NotWithYou
                                                              : CommandRefusal::NotInGroup);
    if (!ValidIntentParams(command.intent, command.params))
        return refuse(CommandRefusal::CannotNow);

    if (command.intent == ChannelIntent::AttackOwnTarget)
    {
        if (!current.ownTargetGuid)
            return refuse(CommandRefusal::NoTarget);
        if (current.ownTargetGuid != current.eligibleVisibleTargetGuid)
            return refuse(CommandRefusal::TargetNotVisible);
        return IntentOrder{current.controllerGuid, current.botGuid, command.intent,
                           ChannelTarget{current.ownTargetGuid}};
    }
    if (auto const* target = std::get_if<ChannelTarget>(&command.params))
    {
        if (target->guid != current.eligibleVisibleTargetGuid)
            return refuse(CommandRefusal::TargetNotVisible);
    }
    if (auto const* mark = std::get_if<ChannelMark>(&command.params))
    {
        if (mark->guid != current.eligibleVisibleTargetGuid)
            return refuse(CommandRefusal::TargetNotVisible);
    }
    if (auto const* position = std::get_if<ChannelPosition>(&command.params))
    {
        if (!current.pathableNearbyPosition || *position != *current.pathableNearbyPosition)
            return refuse(CommandRefusal::Unreachable);
    }
    return IntentOrder{current.controllerGuid, current.botGuid, command.intent, command.params};
}
}  // namespace OllamaBridge

#endif
