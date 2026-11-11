// Offline checks of the production wire decoder and pure intent mapping.
#include <algorithm>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

#include "bridge-ability-double.h"
#include "mod-ollama-chat_bridge_channel.h"

namespace
{
using namespace OllamaBridge;
using Json = nlohmann::json;

unsigned checks = 0;
constexpr char Bearer[] = "synthetic-intent-selftest-link";
constexpr Clock Now{100000, 200000};
constexpr Deadline Until{101000, 201000};
constexpr uint64_t Controller = 9007199254740993ULL;
constexpr uint64_t Bot = 9007199254740994ULL;
constexpr uint64_t Target = std::numeric_limits<uint64_t>::max();
constexpr uint64_t Group = 15;
constexpr ChannelPosition Destination{17.25, -36.5, 7.0, 571};

void Check(bool value, char const* label)
{
    ++checks;
    if (!value)
        throw std::runtime_error(label);
}

struct Fixture
{
    WorldScope scope{1,       "fixture",       "codex",     "epoch-1",   Bot,        Controller, "intent-test",
                     "party", "visible-party", "context-1", "members-1", "persona-1"};
    ChannelPolicy policy;
    ChannelMessage message;

    Fixture()
    {
        Check(policy.Bind({scope.instance, scope.provider, "", scope.epoch}, Bearer), "bind game epoch");
        Check(policy.ObserveScope(scope, Now), "register scope");
        Check(policy.OpenGeneration(scope, "work", Until, Now) == ChannelError::None, "open generation");
    }

    Json Wire(std::string const& intent, Json const& params, std::string const& id = "order") const
    {
        return {{"version", 1},
                {"instance", scope.instance},
                {"provider", scope.provider},
                {"epoch", scope.epoch},
                {"requestId", "work"},
                {"deadlineUnixMs", Until.unixMs},
                {"type", "command"},
                {"commandId", id},
                {"issuerGuid", std::to_string(Controller)},
                {"botGuid", std::to_string(Bot)},
                {"intent", intent},
                {"params", params}};
    }

    ChannelError Receive(Json const& wire, Clock at = Now)
    {
        return policy.Receive(Bearer, wire.dump(), scope, at, message, "work");
    }

    ChannelCommand Stage(std::string const& intent, Json const& params, std::string const& id = "order")
    {
        Check(Receive(Wire(intent, params, id)) == ChannelError::None, intent.c_str());
        return std::get<ChannelCommand>(message.payload);
    }
};

IntentContext Current() { return {Controller, Bot, Group, Group, Group, Target, Target, Destination}; }

void Mapping()
{
    struct Example
    {
        char const* name;
        ChannelIntent intent;
        Json params;
    };
    auto const empty = Json::object();
    auto const guid = std::to_string(Target);
    Example const examples[] = {
        {"follow", ChannelIntent::Follow, empty},
        {"stay", ChannelIntent::Stay, empty},
        {"flee", ChannelIntent::Flee, empty},
        {"regroup", ChannelIntent::Regroup, empty},
        {"attack_own_target", ChannelIntent::AttackOwnTarget, empty},
        {"pull", ChannelIntent::Pull, {{"targetGuid", guid}}},
        {"mark", ChannelIntent::Mark, {{"targetGuid", guid}, {"icon", 7}}},
        {"focus_healing", ChannelIntent::FocusHealing, {{"targetGuid", guid}}},
        {"assign_role", ChannelIntent::AssignRole, {{"role", "tank"}}},
        {"assign_role", ChannelIntent::AssignRole, {{"role", "healer"}}},
        {"assign_role", ChannelIntent::AssignRole, {{"role", "damage"}}},
        {"save_mana", ChannelIntent::SaveMana, {{"enabled", true}}},
        {"save_mana", ChannelIntent::SaveMana, {{"enabled", false}}},
        {"formation", ChannelIntent::Formation, {{"shape", "close"}}},
        {"formation", ChannelIntent::Formation, {{"shape", "spread"}}},
        {"formation", ChannelIntent::Formation, {{"shape", "line"}}},
        {"position", ChannelIntent::Position, {{"x", 17.25}, {"y", -36.5}, {"z", 7}, {"mapId", 571}}},
        {"dungeon_clear", ChannelIntent::DungeonClear, {{"mode", "on"}}},
        {"dungeon_clear", ChannelIntent::DungeonClear, {{"mode", "off"}}},
        {"dungeon_clear", ChannelIntent::DungeonClear, {{"mode", "pause"}}},
        {"go_to", ChannelIntent::GoTo, {{"place", "Goldshire"}}},
        {"quest", ChannelIntent::Quest, {{"questId", 4294967295ULL}}},
        {"errands", ChannelIntent::Errands, empty},
        {"set_hearth", ChannelIntent::SetHearth, empty},
        {"grind_here", ChannelIntent::GrindHere, empty},
        {"gather_here", ChannelIntent::GatherHere, empty},
        {"meet_at", ChannelIntent::MeetAt, {{"place", "Lion's Pride Inn"}}},
        {"pause_plan", ChannelIntent::PausePlan, empty},
        {"resume_plan", ChannelIntent::ResumePlan, empty}};
    Fixture fixture;
    unsigned id = 0;
    for (auto const& example : examples)
    {
        auto const command = fixture.Stage(example.name, example.params, std::to_string(++id));
        auto const order = MapIntent(command, Current());
        Check(order && order->intent == example.intent && order->controllerGuid == Controller && order->botGuid == Bot,
              example.name);
        switch (example.intent)
        {
            case ChannelIntent::Follow:
            case ChannelIntent::Stay:
            case ChannelIntent::Flee:
            case ChannelIntent::Regroup:
            case ChannelIntent::Errands:
            case ChannelIntent::SetHearth:
            case ChannelIntent::GrindHere:
            case ChannelIntent::GatherHere:
            case ChannelIntent::PausePlan:
            case ChannelIntent::ResumePlan:
                Check(std::holds_alternative<std::monostate>(order->params), "zero arguments");
                break;
            case ChannelIntent::GoTo:
            case ChannelIntent::MeetAt:
                Check(std::get<ChannelPlace>(order->params).name == example.params.at("place").get<std::string>(),
                      "exact place name");
                break;
            case ChannelIntent::Quest:
                Check(std::get<ChannelQuestId>(order->params).id == 4294967295U, "exact quest id");
                break;
            case ChannelIntent::AttackOwnTarget:
            case ChannelIntent::Pull:
            case ChannelIntent::FocusHealing:
                Check(std::get<ChannelTarget>(order->params).guid == Target, "exact resolved uint64 target");
                break;
            case ChannelIntent::Mark:
            {
                auto const mark = std::get<ChannelMark>(order->params);
                Check(mark.guid == Target && mark.icon == 7, "resolved mark target and icon");
                break;
            }
            case ChannelIntent::AssignRole:
            {
                auto const role = example.params.at("role");
                auto const expected = role == "tank"     ? ChannelRole::Tank
                                      : role == "healer" ? ChannelRole::Healer
                                                         : ChannelRole::Damage;
                Check(std::get<ChannelRole>(order->params) == expected, "exact role");
                break;
            }
            case ChannelIntent::SaveMana:
                Check(std::get<bool>(order->params) == example.params.at("enabled").get<bool>(), "exact boolean");
                break;
            case ChannelIntent::Formation:
            {
                auto const shape = example.params.at("shape");
                auto const expected = shape == "close"    ? ChannelShape::Close
                                      : shape == "spread" ? ChannelShape::Spread
                                                          : ChannelShape::Line;
                Check(std::get<ChannelShape>(order->params) == expected, "exact shape");
                break;
            }
            case ChannelIntent::Position:
                Check(std::get<ChannelPosition>(order->params) == Destination, "exact proved position");
                break;
            case ChannelIntent::DungeonClear:
            {
                auto const mode = example.params.at("mode");
                auto const expected = mode == "on"    ? ChannelMode::On
                                      : mode == "off" ? ChannelMode::Off
                                                      : ChannelMode::Pause;
                Check(std::get<ChannelMode>(order->params) == expected, "exact dungeon mode");
                break;
            }
        }
    }
}

void WireRefusals()
{
    Fixture fixture;
    auto invalid = [&](Json const& wire)
    { Check(fixture.Receive(wire) == ChannelError::Invalid, "reject invalid wire without reserving its id"); };
    auto const empty = Json::object();
    for (auto const* text : {"trade", "console", "equip", "talent", "do follow", ".follow", "!stay", "stay;flee",
                             "stay|follow", "follow\nflee", "Follow", " follow", "follow "})
        invalid(fixture.Wire(text, empty));
    auto const nul = std::string("stay\0flee", 9);
    invalid(fixture.Wire(nul, empty));
    invalid(fixture.Wire("attack_own_target", {{"targetGuid", "12"}}));
    invalid(fixture.Wire("stay", Json::array()));
    Check(fixture.Receive(fixture.Wire("stay", {{"command", "do flee"}})) == ChannelError::Standing,
          "J5e: a stay with params is the standing shape, refused as invalid_input without reserving its id");
    invalid(fixture.Wire("pull", {{"targetGuid", "12"}, {"other", true}}));
    for (Json const& guid : {Json(12), Json("0"), Json("01"), Json("-12"), Json("+12"), Json("12;stay"),
                             Json("18446744073709551616"), Json(" 12")})
        invalid(fixture.Wire("pull", {{"targetGuid", guid}}));
    invalid(fixture.Wire("focus_healing", empty));
    for (Json const& icon : {Json(-1), Json(8), Json(1.0), Json("1")})
        invalid(fixture.Wire("mark", {{"targetGuid", "12"}, {"icon", icon}}));
    invalid(fixture.Wire("assign_role", {{"role", "tank;stay"}}));
    invalid(fixture.Wire("formation", {{"shape", "circle"}}));
    invalid(fixture.Wire("dungeon_clear", {{"mode", "on\nstay"}}));
    invalid(fixture.Wire("save_mana", {{"enabled", 1}}));
    invalid(fixture.Wire("save_mana", {{"enabled", "true"}}));
    // WT18b: a goal verb takes exactly its own field; a place is text of 1..64 bytes, a quest id a nonzero uint32.
    for (char const* verb : {"go_to", "meet_at"})
    {
        for (Json const& place : {Json(""), Json(std::string(65, 'p')), Json("   "), Json("Gold\nshire"),
                                  Json("Gold\tshire"), Json(std::string("Gold\x7Fshire")), Json(12), Json::array()})
            invalid(fixture.Wire(verb, {{"place", place}}));
        invalid(fixture.Wire(verb, empty));
        invalid(fixture.Wire(verb, {{"questId", 12}}));
        invalid(fixture.Wire(verb, {{"place", "Goldshire"}, {"questId", 12}}));
        invalid(fixture.Wire(verb, {{"name", "Goldshire"}}));
    }
    for (Json const& quest : {Json(0), Json(-1), Json(4294967296ULL), Json(12.0), Json("12")})
        invalid(fixture.Wire("quest", {{"questId", quest}}));
    invalid(fixture.Wire("quest", empty));
    invalid(fixture.Wire("quest", {{"place", "Goldshire"}}));
    invalid(fixture.Wire("quest", {{"questId", 12}, {"place", "Goldshire"}}));
    for (char const* verb : {"errands", "set_hearth", "grind_here", "gather_here", "pause_plan", "resume_plan"})
    {
        invalid(fixture.Wire(verb, {{"place", "Goldshire"}}));
        invalid(fixture.Wire(verb, {{"questId", 12}}));
        invalid(fixture.Wire(verb, Json::array()));
    }
    for (char const* verb : {"Go_to", "goto", "go to", "quest ", "set_hearth;stay", "pause", "resume"})
        invalid(fixture.Wire(verb, empty));
    for (Json const& map : {Json(-1), Json(4294967296ULL), Json(1.5), Json("571")})
        invalid(fixture.Wire("position", {{"x", 1}, {"y", 2}, {"z", 3}, {"mapId", map}}));
    invalid(fixture.Wire("position", {{"x", "1"}, {"y", 2}, {"z", 3}, {"mapId", 571}}));
    auto wire = fixture.Wire("position", {{"x", 999}, {"y", 2}, {"z", 3}, {"mapId", 571}}).dump();
    wire.replace(wire.find("999"), 3, "1e999");
    Check(fixture.policy.Receive(Bearer, wire, fixture.scope, Now, fixture.message, "work") == ChannelError::Invalid,
          "reject unrepresentable coordinate in JSON");
    auto extra = fixture.Wire("stay", empty);
    extra["text"] = "do flee";
    invalid(extra);
    extra.erase("text");
    extra["issuerGuid"] = std::to_string(Bot);
    invalid(extra);
    Check(fixture.policy.CommandReservations() == 0, "invalid requests consume zero command ids");
    fixture.Stage("stay", empty);
    Check(fixture.Receive(fixture.Wire("stay", empty)) == ChannelError::Duplicate, "duplicate cannot stage again");
    Check(fixture.Receive(fixture.Wire("stay", empty, "expired"), {Until.unixMs, Now.steadyMs}) == ChannelError::Stale,
          "equal wall deadline refuses fresh id");
}

void WorldRefusals()
{
    Fixture fixture;
    auto const stay = fixture.Stage("stay", Json::object(), "stay");
    auto current = Current();
    Check(!MapIntent(stay, {}), "missing live facts");
    current.controllerGuid = 0;
    Check(!MapIntent(stay, current), "lost controller security");
    current.controllerGuid = Controller - 1;
    CommandRefusal why = CommandRefusal::None;
    Check(!MapIntent(stay, current, &why) && why == CommandRefusal::CannotNow,
          "an issuer other than the partner is refused as cannot_now");
    current = Current();
    current.botGuid = Bot + 1;
    Check(!MapIntent(stay, current), "wrong live bot");
    current = Current();
    current.controllerGroupGuid = 0;
    Check(MapIntent(stay, current).has_value(), "DS.14: a partner outside the bot's group may still ask it to stay");
    current = Current();
    current.botGroupGuid = 0;
    why = CommandRefusal::None;
    Check(!MapIntent(stay, current, &why) && why == CommandRefusal::CannotNow, "bot left the captured group");
    current.controllerGroupGuid = current.botGroupGuid = Group + 1;
    why = CommandRefusal::None;
    Check(!MapIntent(stay, current, &why) && why == CommandRefusal::CannotNow,
          "both players moved to a different group");
    current.expectedGroupGuid = current.controllerGroupGuid = current.botGroupGuid = 0;
    Check(MapIntent(stay, current).has_value(), "DS.14: an ungrouped bot may stay on its own");
    current.botGroupGuid = Group;
    why = CommandRefusal::None;
    Check(!MapIntent(stay, current, &why) && why == CommandRefusal::CannotNow,
          "an ungrouped capture is stale once the bot joins a group");
    auto forged = stay;
    forged.issuerGuid = Bot;
    current = Current();
    current.controllerGuid = Bot;
    Check(!MapIntent(forged, current), "bot as self cannot provide authority");

    for (auto const* intent : {"pull", "mark", "focus_healing", "attack_own_target"})
    {
        std::string const name = intent;
        Json params = name == "attack_own_target" ? Json::object() : Json{{"targetGuid", std::to_string(Target)}};
        if (name == "mark")
            params["icon"] = 0;
        auto const command = fixture.Stage(intent, params, name);
        current = Current();
        current.eligibleVisibleTargetGuid = 0;
        Check(!MapIntent(command, current), "missing, unobservable or ineligible target");
        current.eligibleVisibleTargetGuid = Target - 1;
        Check(!MapIntent(command, current), "different resolved target cannot substitute");
    }
    auto const attack = fixture.Stage("attack_own_target", Json::object(), "attack");
    current = Current();
    current.ownTargetGuid = 0;
    Check(!MapIntent(attack, current), "no own target to attack");
    current.ownTargetGuid = Target - 1;
    Check(!MapIntent(attack, current), "old eligibility cannot authorize new own target");

    auto const move = fixture.Stage("position", {{"x", 17.25}, {"y", -36.5}, {"z", 7}, {"mapId", 571}}, "move");
    current = Current();
    current.pathableNearbyPosition.reset();
    Check(!MapIntent(move, current), "missing nearby/pathability proof");
    current.pathableNearbyPosition = Destination;
    current.pathableNearbyPosition->mapId = 0;
    Check(!MapIntent(move, current), "proof for different map");
    current.pathableNearbyPosition = Destination;
    current.pathableNearbyPosition->z += 0.1;
    Check(!MapIntent(move, current), "proof for different position");
}

void TypedRefusals()
{
    auto invalid = [](ChannelIntent intent, ChannelParams params)
    {
        Check(!MapIntent({"order", Controller, Bot, intent, params}, Current()),
              "typed callers cannot bypass enum/variant or value constraints");
    };
    invalid(static_cast<ChannelIntent>(255), {});
    invalid(ChannelIntent::Follow, ChannelTarget{Target});
    invalid(ChannelIntent::AttackOwnTarget, ChannelTarget{Target});
    invalid(ChannelIntent::Pull, ChannelMark{Target, 0});
    invalid(ChannelIntent::FocusHealing, ChannelTarget{0});
    invalid(ChannelIntent::Mark, ChannelMark{Target, 8});
    invalid(ChannelIntent::Mark, ChannelMark{0, 7});
    invalid(ChannelIntent::AssignRole, static_cast<ChannelRole>(255));
    invalid(ChannelIntent::SaveMana, {});
    invalid(ChannelIntent::Formation, static_cast<ChannelShape>(255));
    invalid(ChannelIntent::DungeonClear, static_cast<ChannelMode>(255));
    invalid(ChannelIntent::GoTo, {});
    invalid(ChannelIntent::GoTo, ChannelPlace{});
    invalid(ChannelIntent::GoTo, ChannelPlace{std::string(65, 'p')});
    invalid(ChannelIntent::GoTo, ChannelQuestId{12});
    invalid(ChannelIntent::MeetAt, ChannelPlace{});
    invalid(ChannelIntent::MeetAt, ChannelPlace{std::string(65, 'p')});
    invalid(ChannelIntent::MeetAt, ChannelQuestId{12});
    invalid(ChannelIntent::Quest, ChannelQuestId{0});
    invalid(ChannelIntent::Quest, ChannelPlace{"Goldshire"});
    invalid(ChannelIntent::Quest, {});
    for (auto const verb : {ChannelIntent::Errands, ChannelIntent::SetHearth, ChannelIntent::GrindHere,
                            ChannelIntent::GatherHere, ChannelIntent::PausePlan, ChannelIntent::ResumePlan})
    {
        invalid(verb, ChannelPlace{"Goldshire"});
        invalid(verb, ChannelQuestId{12});
    }
    Check(ValidIntentParams(ChannelIntent::GoTo, ChannelPlace{std::string(64, 'p')}) &&
              ValidIntentParams(ChannelIntent::MeetAt, ChannelPlace{"p"}) &&
              ValidIntentParams(ChannelIntent::Quest, ChannelQuestId{1}),
          "WT18b: a place of 1..64 bytes and a nonzero quest id are valid");
    for (double value : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()})
    {
        Check(!ValidIntentParams(ChannelIntent::Position, ChannelPosition{value, 0, 0, 571}), "invalid x");
        Check(!ValidIntentParams(ChannelIntent::Position, ChannelPosition{0, value, 0, 571}), "invalid y");
        Check(!ValidIntentParams(ChannelIntent::Position, ChannelPosition{0, 0, value, 571}), "invalid z");
    }
}

// DS.14: plausibility per verb, never a leader gate. Group play needs the bot's captured group; follow, formation
// and save_mana also need the partner in it. Everything else is the bot's own act, grouped or not.
void PartnerRules()
{
    struct Verb
    {
        char const* name;
        Json params;
        bool group;    // Needs the bot's group.
        bool partner;  // Needs the partner in that group too.
    };
    auto const empty = Json::object();
    auto const guid = std::to_string(Target);
    Verb const verbs[] = {{"follow", empty, true, true},
                          {"formation", {{"shape", "line"}}, true, true},
                          {"save_mana", {{"enabled", true}}, true, true},
                          {"mark", {{"targetGuid", guid}, {"icon", 7}}, true, false},
                          {"focus_healing", {{"targetGuid", guid}}, true, false},
                          {"stay", empty, false, false},
                          {"flee", empty, false, false},
                          {"attack_own_target", empty, false, false},
                          {"pull", {{"targetGuid", guid}}, false, false},
                          {"position", {{"x", 17.25}, {"y", -36.5}, {"z", 7}, {"mapId", 571}}, false, false},
                          {"go_to", {{"place", "Goldshire"}}, false, false},
                          {"quest", {{"questId", 12}}, false, false},
                          {"errands", empty, false, false},
                          {"set_hearth", empty, false, false},
                          {"grind_here", empty, false, false},
                          {"gather_here", empty, false, false},
                          {"meet_at", {{"place", "Goldshire"}}, false, false},
                          {"pause_plan", empty, false, false},
                          {"resume_plan", empty, false, false}};
    auto const outcome = [](ChannelCommand const& command,
                            IntentContext const& current) -> std::optional<CommandRefusal>
    {
        CommandRefusal why = CommandRefusal::None;
        if (MapIntent(command, current, &why))
            return std::nullopt;
        return why;
    };
    Fixture fixture;
    unsigned id = 0;
    for (Verb const& verb : verbs)
    {
        auto const command = fixture.Stage(verb.name, verb.params, "partner-" + std::to_string(++id));
        CommandRefusal const groupWhy =
            command.intent == ChannelIntent::Follow ? CommandRefusal::NotWithYou : CommandRefusal::NotInGroup;
        Check(!outcome(command, Current()), "grouped with the partner: every verb admitted");
        for (uint64_t const partnerGroup : {uint64_t{0}, Group + 1})
        {
            auto outside = Current();
            outside.controllerGroupGuid = partnerGroup;
            auto const away = outcome(command, outside);
            Check(verb.partner ? away == groupWhy : !away,
                  "partner outside the bot's group: follow not_with_you, formation and save_mana not_in_group, "
                  "the rest admitted");
            auto alone = Current();
            alone.expectedGroupGuid = alone.botGroupGuid = 0;
            alone.controllerGroupGuid = partnerGroup;
            auto const lone = outcome(command, alone);
            Check(verb.group ? lone == groupWhy : !lone,
                  "ungrouped bot: follow not_with_you, other group verbs not_in_group, its own acts admitted");
        }
    }
}

// J5e and DS.13: every refusal names a short code and a plain sentence the bot can say; DS.14 adds five.
void RefusalWords()
{
    std::set<std::string> codes;
    for (unsigned value = 1; value <= static_cast<unsigned>(CommandRefusal::UnknownName); ++value)
    {
        auto const words = DescribeRefusal(static_cast<CommandRefusal>(value));
        std::string const code = words.code;
        std::string const text = words.text;
        Check(!code.empty() && code.size() <= 40 &&
                  code.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") == std::string::npos,
              "a refusal code is a short lower-case word");
        Check(!text.empty() && text.size() <= 200 && text.find_first_of("\"\\") == std::string::npos &&
                  std::all_of(text.begin(), text.end(), [](char c) { return c >= 0x20 && c < 0x7F; }),
              "a refusal sentence is plain ASCII without quotes or backslashes");
        Check(codes.insert(code).second, "every refusal has its own code");
        Check(code != "not_leader", "DS.14: no refusal names a leader");
    }
    auto const none = DescribeRefusal(CommandRefusal::None);
    Check(!*none.code && !*none.text, "no refusal has no words");
    Check(std::string(DescribeRefusal(static_cast<CommandRefusal>(200)).code) == "cannot_now",
          "an unknown refusal is cannot_now");
    auto const said = [](CommandRefusal refusal, char const* code, char const* text)
    {
        auto const words = DescribeRefusal(refusal);
        return std::string(words.code) == code && std::string(words.text) == text;
    };
    Check(said(CommandRefusal::Dead, "dead", "I am in no state to do anything right now."), "dead sentence");
    Check(said(CommandRefusal::Battleground, "battleground", "Not here; I have my own fight to see to."),
          "battleground sentence");
    Check(said(CommandRefusal::Travelling, "travelling", "I cannot do that while I am in flight."),
          "travelling sentence");
    Check(said(CommandRefusal::NotWithYou, "not_with_you", "We are not travelling together."), "not_with_you sentence");
    Check(said(CommandRefusal::TooFar, "too_far", "You are too far away for me to reach."), "too_far sentence");
    // WT18b: the planner's refusals of a goal verb.
    Check(said(CommandRefusal::NotPlanner, "not_planner", "My own plans are not mine to change right now."),
          "not_planner sentence");
    Check(said(CommandRefusal::GoalHeld, "goal_held", "My group has a goal I am holding to."), "goal_held sentence");
    Check(said(CommandRefusal::UnknownPlace, "unknown_place", "I do not know that place."), "unknown_place sentence");
    Check(said(CommandRefusal::NoRoute, "no_route", "I cannot find a way there."), "no_route sentence");
    Check(said(CommandRefusal::NoSuchQuest, "no_such_quest", "That quest is not in my log."), "no_such_quest sentence");
    Check(said(CommandRefusal::NothingHere, "nothing_here", "There is nothing for me to do here."),
          "nothing_here sentence");
    Check(said(CommandRefusal::AlreadyThere, "already_there", "I am already there."), "already_there sentence");
    // LC3: playerbots' sentence for a name it does not know (a named order).
    Check(said(CommandRefusal::UnknownName, "unknown_name", "I do not know what you mean by that."),
          "unknown_name sentence");
    Check(said(CommandRefusal::Failed, "failed", "I tried, but it did not work."), "failed sentence");
}

void StagedCommandCurrentness()
{
    Fixture fixture;
    auto const command = fixture.Stage("stay", Json::object());
    auto const envelope = fixture.message.envelope;
    Check(fixture.policy.Current(envelope, fixture.scope, Now) && MapIntent(command, Current()),
          "current staged order has a mapping");
    Check(!fixture.policy.Current(envelope, fixture.scope, {Now.unixMs, Until.steadyMs}),
          "steady equality invalidates staged order during wall clock rollback");
    auto changed = fixture.scope;
    changed.membershipRevision = "members-2";
    Check(!fixture.policy.Current(envelope, changed, Now), "changed group revision invalidates staged order");
    std::string body;
    Check(fixture.policy.FinishCommand(envelope, command.commandId, ChannelOutcome::Rejected, fixture.scope, Now,
                                       body) == ChannelError::None,
          "current executor refusal produces a result");
    Check(fixture.Receive(fixture.Wire("stay", Json::object())) == ChannelError::Duplicate,
          "refusal does not let wire retry same id");
    fixture.policy.Retire();
    Check(!fixture.policy.Current(envelope, fixture.scope, Now), "retired activation invalidates staged order");
}
}  // namespace

int main()
{
    if (!InstallTestAbilities())  // LC3: the verbs and topics are the installed index's.
    {
        std::cerr << "FAIL: the ability double was refused\n";
        return 1;
    }
    try
    {
        Mapping();
        WireRefusals();
        WorldRefusals();
        TypedRefusals();
        PartnerRules();
        RefusalWords();
        StagedCommandCurrentness();
        std::cout << "PASS " << checks << " checks\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
