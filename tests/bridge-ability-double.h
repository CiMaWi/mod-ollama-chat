// LC3: a double of the ability index the game adapter installs from mod-playerbots' table (handler.cpp's
// BridgeAbilityIndex): the old wire's 22 verbs in ChannelIntent order, three acts with every part filled and the
// fourteen topics in StateTopic order. A standalone selftest installs it before it parses a command or a topic.
#ifndef MOD_OLLAMA_CHAT_TESTS_BRIDGE_ABILITY_DOUBLE_H
#define MOD_OLLAMA_CHAT_TESTS_BRIDGE_ABILITY_DOUBLE_H

#include <iterator>

#include "mod-ollama-chat_bridge_channel.h"

inline OllamaBridge::AbilityIndex TestAbilityIndex()
{
    using namespace OllamaBridge;
    AbilityIndex index;
    index.hash = 0x0a1b2c3d4e5f6071ULL;  // A leading zero digit: the wire keeps all sixteen.
    index.version = 1;
    index.maxSteps = 3;
    char const* const verbs[] = {"follow",     "stay",       "flee",          "regroup",     "attack_own_target",
                                 "pull",       "mark",       "focus_healing", "assign_role", "save_mana",
                                 "formation",  "position",   "dungeon_clear", "go_to",       "quest",
                                 "errands",    "set_hearth", "grind_here",    "gather_here", "meet_at",
                                 "pause_plan", "resume_plan"};
    for (std::size_t value = 0; value < std::size(verbs); ++value)
        index.verbs.push_back({verbs[value], static_cast<ChannelIntent>(value)});
    std::vector<AbilityRefusal> const common = {{"battleground", "Not here; I have my own fight to see to."},
                                                {"travelling", "I cannot do that while I am in flight."},
                                                {"dead", "I am in no state to do anything right now."}};
    auto const with = [&common](std::vector<AbilityRefusal> own)
    {
        own.insert(own.begin(), common.begin(), common.end());
        return own;
    };
    index.rows.push_back({.topic = false,
                          .code = static_cast<uint8_t>(ChannelIntent::Follow),
                          .name = "follow",
                          .sentence = "Follow the person you are talking to.",
                          .params = {},
                          .refusals = with({{"not_with_you", "We are not travelling together."},
                                            {"too_far", "You are too far away for me to reach."}}),
                          .effect = "movement",
                          .done = "holds",
                          .state = "following you",
                          .event = "BridgeAct",
                          .limit = 0});
    index.rows.push_back({.topic = false,
                          .code = static_cast<uint8_t>(ChannelIntent::Mark),
                          .name = "mark",
                          .sentence = "Put a raid icon on a unit you can see.",
                          .params = {{"target", "unit", "The unit to mark, by name.", {}, AbilityField::Target},
                                     {"icon",
                                      "choice",
                                      "The raid icon to put on it.",
                                      {"star", "circle", "diamond", "triangle", "moon", "square", "cross", "skull"},
                                      AbilityField::Icon}},
                          .refusals = with({{"not_in_group", "We are not in the same group."},
                                            {"unknown_name", "I do not know what you mean by that."}}),
                          .effect = "none",
                          .done = "at_once",
                          .state = "marking {target}",
                          .event = "BridgeAct",
                          .limit = 0});
    index.rows.push_back({.topic = false,
                          .code = static_cast<uint8_t>(ChannelIntent::GoTo),
                          .name = "go_to",
                          .sentence = "Travel to a place you know.",
                          .params = {{"place", "place", "A known place's name.", {}, AbilityField::PlaceName}},
                          .refusals = with({{"unknown_place", "I do not know that place."}}),
                          .effect = "none",
                          .done = "goal_ends",
                          .state = "travelling to {place}",
                          .event = "BridgeAct",
                          .limit = 0});
    struct Topic
    {
        char const* name;
        std::size_t limit;
    };
    Topic const topics[] = {{"inventory", 40},  {"quests", 40}, {"cooldowns", 40}, {"strategies", 40},
                            {"spells", 40},     {"plan", 8},    {"options", 5},    {"route", 12},
                            {"quest_plan", 12}, {"bags", 6},    {"places", 24}, {"history", 24},
                            {"lately", 12},     {"known", 12}};
    for (std::size_t value = 0; value < std::size(topics); ++value)
    {
        AbilityRow row{.topic = true,
                       .code = static_cast<uint8_t>(value),
                       .name = topics[value].name,
                       .sentence = std::string("Your ") + topics[value].name + ".",
                       .params = {},
                       .refusals = {},
                       .effect = "none",
                       .done = "at_once",
                       .state = "",
                       .event = "",
                       .limit = topics[value].limit};
        if (static_cast<StateTopic>(value) == StateTopic::Route)
            row.params.push_back({"place", "place", "A known place's name.", {}, AbilityField::PlaceName});
        index.rows.push_back(std::move(row));
    }
    return index;
}

// LC3b: the double with the acts a stepped command needs for every parameter form, inserted after go_to (rows 3-9;
// the topics follow from row 10): stay, quest, position, formation, save_mana, focus_healing, attack_own_target.
// TestAbilityIndex stays as it is: the selftests pin its seventeen rows by position.
inline OllamaBridge::AbilityIndex TestStepAbilityIndex()
{
    using namespace OllamaBridge;
    auto index = TestAbilityIndex();
    auto const act = [](ChannelIntent intent, char const* name, std::vector<AbilityParam> params)
    {
        return AbilityRow{.topic = false,
                          .code = static_cast<uint8_t>(intent),
                          .name = name,
                          .sentence = std::string("Do ") + name + ".",
                          .params = std::move(params),
                          .refusals = {},
                          .effect = "none",
                          .done = "at_once",
                          .state = "",
                          .event = "BridgeAct",
                          .limit = 0};
    };
    AbilityRow const acts[] = {
        act(ChannelIntent::Stay, "stay", {}),
        act(ChannelIntent::Quest, "quest", {{"quest", "quest", "The quest, by its title.", {}, AbilityField::QuestId}}),
        act(ChannelIntent::Position, "position", {{"place", "place", "Where to stand.", {}, AbilityField::Position}}),
        act(ChannelIntent::Formation, "formation",
            {{"shape", "choice", "The formation.", {"close", "spread", "line"}, AbilityField::Shape}}),
        act(ChannelIntent::SaveMana, "save_mana", {{"enabled", "flag", "On or off.", {}, AbilityField::Enabled}}),
        act(ChannelIntent::FocusHealing, "focus_healing",
            {{"target", "unit", "Whom to heal first.", {}, AbilityField::Target}}),
        act(ChannelIntent::AttackOwnTarget, "attack_own_target",
            {{"target", "unit", "What to attack.", {}, AbilityField::Target}}),
    };
    index.rows.insert(index.rows.begin() + 3, std::begin(acts), std::end(acts));
    return index;
}

// Installs the double; false when the channel refused it.
inline bool InstallTestAbilities()
{
    OllamaBridge::SetAbilityIndex(TestAbilityIndex());
    return !OllamaBridge::Abilities().rows.empty();
}

#endif
