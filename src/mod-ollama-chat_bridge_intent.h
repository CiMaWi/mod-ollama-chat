#ifndef MOD_OLLAMA_CHAT_BRIDGE_INTENT_H
#define MOD_OLLAMA_CHAT_BRIDGE_INTENT_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "BridgeIntentEffect.h"
#include "BridgeTexts.h"

namespace OllamaBridge
{
// WT18b: a place the planner knows by name (go_to, meet_at); the channel checked it is text, not blank.
constexpr std::size_t ChannelMaxPlaceName = 64;

// LC3: one step of a stepped command, its parameters by the catalogue's names. The act is a row of the installed
// catalogue; a unit and a quest come as an id or a name (exactly one when the act takes it), a place as given, a
// choice as its index among the act's choices. playerbots checks the rest.
// LCB2: intent is the act's playerbots Intent value, its row's code in the installed index.
struct ChannelStep
{
    PlayerbotBridge::Intent intent = PlayerbotBridge::Intent::Stay;
    uint64_t targetGuid = 0;
    std::string targetName;
    uint32_t questId = 0;
    std::string questName;
    std::string place;              // go_to, meet_at: a place's name, "x,y,z,map" or "you", as given
    std::optional<uint8_t> icon;    // mark
    std::optional<uint8_t> shape;   // formation
    std::optional<bool> enabled;    // save_mana
    std::string itemName;           // LC16c: an item, by name
    std::string spellName;          // a spell, by name
    std::optional<uint8_t> choice;  // a choice other than an icon or a shape, as its index
    uint32_t count = 0;             // 1..ChannelMaxCount; 0 = the act takes none
};
constexpr uint32_t ChannelMaxCount = 1000;  // playerbots' and the hub's MAX_COUNT.

struct ChannelCommand
{
    std::string commandId;
    uint64_t issuerGuid = 0;
    uint64_t botGuid = 0;
    std::vector<ChannelStep> steps;  // LC3: 1..maxSteps steps, run as one order.
};

// J5e: why an order was refused, so the bot can turn it into its own in-world reply. The wire carries the code and
// the sentence (fixed ASCII, no quotes or backslashes, the bot speaking to its conversation partner).
enum class CommandRefusal : uint8_t
{
    None,
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
    NotFighting,  // J6e-c watch refusals.
    NotBusy,
    WaitTooShort,
    WaitTooLong,
    PutOff,
    CannotNow,
    Failed,
    NotPlanner,  // WT18b: the planner's refusals of a goal verb.
    GoalHeld,
    UnknownPlace,
    NoRoute,
    NoSuchQuest,
    NothingHere,
    AlreadyThere,
    UnknownName  // LC3: nothing of that name the bot sees or has in its log (a named order).
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
        case CommandRefusal::TooSoon:
            return {"too_soon", BridgeTexts::Refusals::TOO_SOON};
        case CommandRefusal::Unavailable:
            return {"unavailable", BridgeTexts::Refusals::UNAVAILABLE};
        case CommandRefusal::Dead:
            return {"dead", BridgeTexts::Refusals::DEAD};
        case CommandRefusal::Battleground:
            return {"battleground", BridgeTexts::Refusals::BATTLEGROUND};
        case CommandRefusal::Travelling:
            return {"travelling", BridgeTexts::Refusals::TRAVELLING};
        case CommandRefusal::NotWithYou:
            return {"not_with_you", BridgeTexts::Refusals::NOT_WITH_YOU};
        case CommandRefusal::TooFar:
            return {"too_far", BridgeTexts::Refusals::TOO_FAR};
        case CommandRefusal::NotInGroup:
            return {"not_in_group", BridgeTexts::Refusals::NOT_IN_GROUP};
        case CommandRefusal::NoTarget:
            return {"no_target", BridgeTexts::Refusals::NO_TARGET};
        case CommandRefusal::TargetNotVisible:
            return {"target_not_visible", BridgeTexts::Refusals::TARGET_NOT_VISIBLE};
        case CommandRefusal::Unreachable:
            return {"unreachable", BridgeTexts::Refusals::UNREACHABLE};
        case CommandRefusal::NotFighting:
            return {"not_fighting", BridgeTexts::Refusals::NOT_FIGHTING};
        case CommandRefusal::NotBusy:
            return {"not_busy", BridgeTexts::Refusals::NOT_BUSY};
        case CommandRefusal::WaitTooShort:
            return {"wait_too_short", BridgeTexts::Refusals::WAIT_TOO_SHORT};
        case CommandRefusal::WaitTooLong:
            return {"wait_too_long", BridgeTexts::Refusals::WAIT_TOO_LONG};
        case CommandRefusal::PutOff:
            return {"put_off", BridgeTexts::Refusals::PUT_OFF};
        case CommandRefusal::CannotNow:
            return {"cannot_now", BridgeTexts::Refusals::CANNOT_NOW};
        case CommandRefusal::Failed:
            return {"failed", BridgeTexts::Refusals::FAILED};
        case CommandRefusal::NotPlanner:
            return {"not_planner", BridgeTexts::Refusals::NOT_PLANNER};
        case CommandRefusal::GoalHeld:
            return {"goal_held", BridgeTexts::Refusals::GOAL_HELD};
        case CommandRefusal::UnknownPlace:
            return {"unknown_place", BridgeTexts::Refusals::UNKNOWN_PLACE};
        case CommandRefusal::NoRoute:
            return {"no_route", BridgeTexts::Refusals::NO_ROUTE};
        case CommandRefusal::NoSuchQuest:
            return {"no_such_quest", BridgeTexts::Refusals::NO_SUCH_QUEST};
        case CommandRefusal::NothingHere:
            return {"nothing_here", BridgeTexts::Refusals::NOTHING_HERE};
        case CommandRefusal::AlreadyThere:
            return {"already_there", BridgeTexts::Refusals::ALREADY_THERE};
        case CommandRefusal::UnknownName:
            return {"unknown_name", BridgeTexts::Refusals::UNKNOWN_NAME};
    }
    return {"cannot_now", BridgeTexts::Refusals::CANNOT_NOW};
}
}  // namespace OllamaBridge

#endif
