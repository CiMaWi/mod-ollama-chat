// Offline checks of the production wire decoder and pure intent mapping.
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

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
        {"dungeon_clear", ChannelIntent::DungeonClear, {{"mode", "pause"}}}};
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
                Check(std::holds_alternative<std::monostate>(order->params), "zero arguments");
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
    invalid(fixture.Wire("stay", {{"command", "do flee"}}));
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
    Check(!MapIntent(stay, current), "replacement controller cannot reuse prior authority");
    current = Current();
    current.botGuid = Bot + 1;
    Check(!MapIntent(stay, current), "wrong live bot");
    current = Current();
    current.controllerGroupGuid = 0;
    Check(!MapIntent(stay, current), "controller left the group");
    current = Current();
    current.botGroupGuid = 0;
    Check(!MapIntent(stay, current), "bot left the group");
    current.controllerGroupGuid = current.botGroupGuid = Group + 1;
    Check(!MapIntent(stay, current), "both players moved to a different group");
    current.expectedGroupGuid = current.controllerGroupGuid = current.botGroupGuid = 0;
    Check(!MapIntent(stay, current), "ungrouped request has no authority");
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
    for (double value : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()})
    {
        Check(!ValidIntentParams(ChannelIntent::Position, ChannelPosition{value, 0, 0, 571}), "invalid x");
        Check(!ValidIntentParams(ChannelIntent::Position, ChannelPosition{0, value, 0, 571}), "invalid y");
        Check(!ValidIntentParams(ChannelIntent::Position, ChannelPosition{0, 0, value, 571}), "invalid z");
    }
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
    try
    {
        Mapping();
        WireRefusals();
        WorldRefusals();
        TypedRefusals();
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
