// Offline checks of the production wire decoder: one command form (LCB2), its act codes and refusal words.
#include <algorithm>
#include <iostream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>

#include "bridge-ability-double.h"
#include "mod-ollama-chat_bridge_channel.h"

namespace
{
using namespace OllamaBridge;
using PlayerbotBridge::Intent;
using Json = nlohmann::json;

unsigned checks = 0;
constexpr char Bearer[] = "synthetic-intent-selftest-link";
constexpr Clock Now{100000, 200000};
constexpr Deadline Until{101000, 201000};
constexpr uint64_t Controller = 9007199254740993ULL;
constexpr uint64_t Bot = 9007199254740994ULL;

void Check(bool value, char const* label)
{
    ++checks;
    if (!value)
        throw std::runtime_error(label);
}

Json Step(std::string const& act, Json const& params = Json::object()) { return {{"act", act}, {"params", params}}; }

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

    Json Envelope(std::string const& id) const
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
                {"botGuid", std::to_string(Bot)}};
    }

    Json Wire(Json const& steps, std::string const& id = "order") const
    {
        auto wire = Envelope(id);
        wire["steps"] = steps;
        return wire;
    }

    // The old form: one verb and its params, no steps.
    Json OldWire(std::string const& intent, Json const& params, std::string const& id = "order") const
    {
        auto wire = Envelope(id);
        wire["intent"] = intent;
        wire["params"] = params;
        return wire;
    }

    ChannelError Receive(Json const& wire, Clock at = Now)
    {
        return policy.Receive(Bearer, wire.dump(), scope, at, message, "work");
    }

    ChannelCommand Stage(Json const& steps, std::string const& id = "order")
    {
        Check(Receive(Wire(steps, id)) == ChannelError::None, id.c_str());
        return std::get<ChannelCommand>(message.payload);
    }
};

// LCB2: the old form is gone. Every old verb with its params, and a body that mixes both forms, is invalid and
// reserves no command id.
void OldFormRefused()
{
    Fixture fixture;
    auto invalid = [&](Json const& wire, char const* label)
    { Check(fixture.Receive(wire) == ChannelError::Invalid, label); };
    auto const empty = Json::object();
    std::pair<char const*, Json> const old[] = {{"follow", empty},
                                                {"stay", empty},
                                                {"flee", empty},
                                                {"regroup", empty},
                                                {"attack_own_target", {{"targetGuid", "12"}}},
                                                {"pull", {{"targetGuid", "12"}}},
                                                {"mark", {{"targetGuid", "12"}, {"icon", 7}}},
                                                {"focus_healing", {{"targetGuid", "12"}}},
                                                {"assign_role", {{"role", "tank"}}},
                                                {"save_mana", {{"enabled", true}}},
                                                {"formation", {{"shape", "line"}}},
                                                {"position", {{"x", 1}, {"y", 2}, {"z", 3}, {"mapId", 571}}},
                                                {"dungeon_clear", {{"mode", "on"}}},
                                                {"go_to", {{"place", "Goldshire"}}},
                                                {"quest", {{"questId", 12}}},
                                                {"errands", empty},
                                                {"set_hearth", empty},
                                                {"grind_here", empty},
                                                {"gather_here", empty},
                                                {"meet_at", {{"place", "Goldshire"}}},
                                                {"pause_plan", empty},
                                                {"resume_plan", empty}};
    for (auto const& [verb, params] : old)
        invalid(fixture.OldWire(verb, params), verb);
    invalid(fixture.OldWire("stay", {{"command", "do flee"}}), "the old standing shape is invalid too");
    auto mixed = fixture.Wire(Json::array({Step("stay")}));
    mixed["intent"] = "stay";
    invalid(mixed, "a stepped body with an intent is invalid");
    mixed.erase("intent");
    mixed["params"] = empty;
    invalid(mixed, "a stepped body with params is invalid");
    Check(fixture.policy.CommandReservations() == 0, "an old-form body reserves no command id");
    fixture.Stage(Json::array({Step("stay")}));
    Check(fixture.policy.CommandReservations() == 1, "the stepped form stages after the refusals");
}

// The stepped wire's own refusals: an unknown or mis-spelt act, a topic, a wrong parameter, an extra key. Each is
// invalid and reserves no id; a duplicate and an expired id are told apart.
void WireRefusals()
{
    Fixture fixture;
    auto invalid = [&](Json const& steps)
    {
        Check(fixture.Receive(fixture.Wire(steps)) == ChannelError::Invalid,
              "reject invalid wire without reserving its id");
    };
    for (auto const* act : {"trade", "console", "do follow", ".follow", "!stay", "stay;flee", "Follow", " follow",
                            "follow ", "flee", "inventory", "goto"})
        invalid(Json::array({Step(act)}));
    invalid(Json::array({Step(std::string("stay\0flee", 9))}));
    invalid(Json::array());
    invalid(Json::array({Step("stay"), Step("stay"), Step("stay"), Step("stay")}));  // maxSteps is 3.
    invalid(Json::object());
    invalid(Json::array({{{"act", "stay"}}}));
    invalid(Json::array({{{"act", "stay"}, {"params", Json::array()}}}));
    invalid(Json::array({{{"act", "stay"}, {"params", Json::object()}, {"intent", "stay"}}}));
    invalid(Json::array({Step("stay", {{"command", "do flee"}})}));
    invalid(Json::array({Step("attack_own_target", {{"target", "Hogger"}, {"targetGuid", "12"}})}));
    for (Json const& guid :
         {Json(12), Json("0"), Json("01"), Json("-12"), Json("12;stay"), Json("18446744073709551616"), Json(" 12")})
        invalid(Json::array({Step("attack_own_target", {{"targetGuid", guid}})}));
    invalid(Json::array({Step("focus_healing")}));
    for (Json const& icon : {Json(-1), Json(8), Json("8"), Json("Skull")})
        invalid(Json::array({Step("mark", {{"target", "Hogger"}, {"icon", icon}})}));
    invalid(Json::array({Step("formation", {{"shape", "circle"}})}));
    invalid(Json::array({Step("save_mana", {{"enabled", 1}})}));
    invalid(Json::array({Step("save_mana", {{"enabled", "true"}})}));
    for (Json const& place : {Json(""), Json(std::string(65, 'p')), Json("   "), Json("Gold\nshire"),
                              Json("Gold\tshire"), Json(std::string("Gold\x7Fshire")), Json(12), Json::array()})
        invalid(Json::array({Step("go_to", {{"place", place}})}));
    invalid(Json::array({Step("go_to")}));
    invalid(Json::array({Step("go_to", {{"place", "Goldshire"}, {"questId", 12}})}));
    for (Json const& quest : {Json(0), Json(-1), Json(4294967296ULL), Json(12.0), Json("12")})
        invalid(Json::array({Step("quest", {{"questId", quest}})}));
    invalid(Json::array({Step("quest", {{"place", "Goldshire"}})}));
    for (Json const& spot : {Json("1,2,3"), Json("1,2,3,571,0"), Json("1,2,3,-1"), Json("1,2,nan,571"),
                             Json("1,2,3,4294967296"), Json("x,2,3,571"), Json(571)})
        invalid(Json::array({Step("position", {{"place", spot}})}));
    auto extra = fixture.Wire(Json::array({Step("stay")}));
    extra["text"] = "do flee";
    Check(fixture.Receive(extra) == ChannelError::Invalid, "an extra key is invalid");
    extra.erase("text");
    extra["issuerGuid"] = std::to_string(Bot);
    Check(fixture.Receive(extra) == ChannelError::Invalid, "the bot cannot issue its own order");
    Check(fixture.policy.CommandReservations() == 0, "invalid requests consume zero command ids");
    fixture.Stage(Json::array({Step("stay")}));
    Check(fixture.Receive(fixture.Wire(Json::array({Step("stay")}))) == ChannelError::Duplicate,
          "duplicate cannot stage again");
    // LC.35: at its wall deadline the route leaves the ledger; a fresh id then finds no route.
    Check(fixture.Receive(fixture.Wire(Json::array({Step("stay")}), "expired"), {Until.unixMs, Now.steadyMs}) ==
              ChannelError::Unknown,
          "equal wall deadline refuses fresh id");
}

// LCB2: a step's act is its row's code, as playerbots' Intent. Each act of the double stages with the Intent its name
// stands for; a decoder that keyed a step by anything but its row (a wrong code, a position) fails here.
void ActCodes()
{
    std::map<std::string, std::pair<Intent, Json>> const acts = {
        {"follow", {Intent::Follow, Json::object()}},
        {"mark", {Intent::Mark, {{"target", "Hogger"}, {"icon", "skull"}}}},
        {"go_to", {Intent::GoTo, {{"place", "Goldshire"}}}},
        {"stay", {Intent::Stay, Json::object()}},
        {"quest", {Intent::Quest, {{"questId", 12}}}},
        {"position", {Intent::Position, {{"place", "17.25,-36.5,7,571"}}}},
        {"formation", {Intent::Formation, {{"shape", "line"}}}},
        {"save_mana", {Intent::SaveMana, {{"enabled", false}}}},
        {"focus_healing", {Intent::FocusHealing, {{"targetGuid", "12"}}}},
        {"attack_own_target", {Intent::AttackOwnTarget, {{"target", "Hogger"}}}}};
    std::size_t seen = 0;
    for (auto const& row : Abilities().rows)
    {
        if (row.topic)
            continue;
        auto const found = acts.find(row.name);
        Check(found != acts.end(), "every act of the double has an expected Intent");
        Check(static_cast<uint8_t>(found->second.first) == row.code, "the double's row code is its act's Intent");
        Fixture fixture;
        auto const command = fixture.Stage(Json::array({Step(row.name, found->second.second)}), row.name);
        Check(command.steps.size() == 1 && command.steps.front().intent == found->second.first,
              "a staged step carries its act's Intent");
        ++seen;
    }
    Check(seen == acts.size(), "every expected act is in the double");
    Fixture fixture;
    auto const command =
        fixture.Stage(Json::array({Step("formation", {{"shape", "spread"}}),
                                   Step("mark", {{"targetGuid", "12"}, {"icon", "star"}}), Step("stay")}),
                      "three");
    Check(command.steps.size() == 3 && command.steps[0].intent == Intent::Formation && command.steps[0].shape == 1 &&
              command.steps[1].intent == Intent::Mark && command.steps[1].targetGuid == 12 &&
              command.steps[1].icon == 0 && command.steps[2].intent == Intent::Stay,
          "a three-step order keeps each step's act and parameters in order");
    Check(command.issuerGuid == Controller && command.botGuid == Bot && command.commandId == "three",
          "the order keeps its issuer, bot and id");
}

// DS.14: follow, formation and save_mana need the partner in the bot's group; no other act does.
void PartnerRules()
{
    for (unsigned value = 0; value <= static_cast<unsigned>(Intent::UseItem); ++value)
    {
        auto const intent = static_cast<Intent>(value);
        bool const expected = intent == Intent::Follow || intent == Intent::Formation || intent == Intent::SaveMana;
        Check(IntentNeedsPartnerInGroup(intent) == expected, "partner-in-group rule per act");
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
        Check(code != "not_supported" && code != "standing_not_supported", "LCB2: the old form's refusals are gone");
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
    Check(said(CommandRefusal::Dead, "dead", "You are dead."), "dead sentence");
    Check(said(CommandRefusal::Battleground, "battleground", "You are in a battleground, where this cannot be done."),
          "battleground sentence");
    Check(said(CommandRefusal::Travelling, "travelling", "You are in flight; this cannot be done until you land."),
          "travelling sentence");
    Check(said(CommandRefusal::NotWithYou, "not_with_you", "The one who asked is not the one you follow."),
          "not_with_you sentence");
    Check(said(CommandRefusal::TooFar, "too_far", "The one who asked is too far away."), "too_far sentence");
    // WT18b: the planner's refusals of a goal verb.
    Check(said(CommandRefusal::NotPlanner, "not_planner", "Your plans are not yours to change right now."),
          "not_planner sentence");
    Check(said(CommandRefusal::GoalHeld, "goal_held", "Your group holds to a goal, and you with it."),
          "goal_held sentence");
    Check(said(CommandRefusal::UnknownPlace, "unknown_place", "You do not know that place."), "unknown_place sentence");
    Check(said(CommandRefusal::NoRoute, "no_route", "You know no way there."), "no_route sentence");
    Check(said(CommandRefusal::NoSuchQuest, "no_such_quest", "That quest is not in your quest log."),
          "no_such_quest sentence");
    Check(said(CommandRefusal::NothingHere, "nothing_here", "There is nothing for you to do here."),
          "nothing_here sentence");
    Check(said(CommandRefusal::AlreadyThere, "already_there", "You are already there."), "already_there sentence");
    // LC3: playerbots' sentence for a name it does not know (a named order).
    Check(said(CommandRefusal::UnknownName, "unknown_name", "You know nothing by that name."),
          "unknown_name sentence");
    Check(said(CommandRefusal::Failed, "failed", "You tried, but it did not work."), "failed sentence");
}

void StagedCommandCurrentness()
{
    Fixture fixture;
    auto const command = fixture.Stage(Json::array({Step("stay")}));
    auto const envelope = fixture.message.envelope;
    Check(fixture.policy.Current(envelope, fixture.scope, Now) && command.steps.size() == 1,
          "current staged order has its step");
    Check(!fixture.policy.Current(envelope, fixture.scope, {Now.unixMs, Until.steadyMs}),
          "steady equality invalidates staged order during wall clock rollback");
    auto changed = fixture.scope;
    changed.membershipRevision = "members-2";
    Check(!fixture.policy.Current(envelope, changed, Now), "changed group revision invalidates staged order");
    std::string body;
    Check(fixture.policy.FinishCommand(envelope, command.commandId, ChannelOutcome::Rejected, fixture.scope, Now,
                                       body) == ChannelError::None,
          "current executor refusal produces a result");
    Check(fixture.Receive(fixture.Wire(Json::array({Step("stay")}))) == ChannelError::Duplicate,
          "refusal does not let wire retry same id");
    fixture.policy.Retire();
    Check(!fixture.policy.Current(envelope, fixture.scope, Now), "retired activation invalidates staged order");
}
}  // namespace

int main()
{
    OllamaBridge::SetAbilityIndex(TestStepAbilityIndex());  // LC3: the acts and topics are the installed index's.
    if (OllamaBridge::Abilities().rows.empty())
    {
        std::cerr << "FAIL: the ability double was refused\n";
        return 1;
    }
    try
    {
        OldFormRefused();
        WireRefusals();
        ActCodes();
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
