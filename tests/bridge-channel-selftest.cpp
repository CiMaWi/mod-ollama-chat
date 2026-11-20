// Offline checks of the actual production channel helper; no game authority or effect executor.
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <tuple>

#include "bridge-ability-double.h"
#include "mod-ollama-chat_bridge_channel.h"

namespace
{
using namespace OllamaBridge;
using Json = nlohmann::json;
unsigned checks = 0;
Clock const now{100000, 200000};
Deadline const deadline{101000, 201000};

void Check(bool value, char const* label)
{
    ++checks;
    if (!value)
        throw std::runtime_error(label);
}

struct Fixture
{
    ChannelPolicy policy;
    WorldScope scope{
        1,       "fixture",       "codex",     "epoch-1",  9007199254740994ULL, 9007199254740993ULL, "conversation",
        "party", "visible-party", "context-1", "members-1"};
    explicit Fixture(std::size_t ids = ChannelMaxRouteIds) : policy(ids)
    {
        Check(policy.Bind({scope.instance, scope.provider, "", scope.epoch}), "trusted selected epoch binding");
    }
    void Open(std::string const& id = "work")
    {
        Check(policy.OpenGeneration(scope, id, deadline, now) == ChannelError::None, "open game generation");
    }
    Json Wire(std::string type, std::string id = "work") const
    {
        return {{"version", 1},
                {"instance", scope.instance},
                {"provider", scope.provider},
                {"epoch", scope.epoch},
                {"requestId", id},
                {"deadlineUnixMs", deadline.unixMs},
                {"type", std::move(type)}};
    }
    Json Query(std::string id = "query") const
    {
        auto json = Wire("query", std::move(id));
        json["controllerGuid"] = std::to_string(scope.controllerGuid);
        json["botGuid"] = std::to_string(scope.botGuid);
        json["query"] = "observable_group";
        return json;
    }
    Json Command(std::string id = "effect", std::string request = "work") const
    {
        auto json = Wire("command", std::move(request));
        json["commandId"] = std::move(id);
        json["issuerGuid"] = std::to_string(scope.controllerGuid);
        json["botGuid"] = std::to_string(scope.botGuid);
        json["steps"] = Json::array({{{"act", "follow"}, {"params", Json::object()}}});
        return json;
    }
    ChannelError Receive(Json const& json, Clock clock = now)
    {
        ChannelMessage result;
        // A query opens under the fixture's generation route (LC.35); other messages name their own route.
        bool const query = json.contains("type") && json.at("type") == "query";
        std::string const route =
            !query && json.at("requestId").is_string() ? json.at("requestId").get<std::string>() : "work";
        return policy.Receive(json.dump(), scope, clock, result, route);
    }
};

void IdentityAndTerminal()
{
    Fixture f;
    f.Open();
    ChannelMessage result;
    // LCB6a: the listener's BearerMatches is the one bearer check; an unbound policy refuses before it parses.
    ChannelPolicy unbound;
    Check(unbound.Receive("{", f.scope, now, result, "work") == ChannelError::Inactive &&
              unbound.Receive(std::string(MaxRequestBytes + 1, 'x'), f.scope, now, result, "work") ==
                  ChannelError::Inactive,
          "an unbound channel parses nothing");
    Check(f.Receive(f.Wire("hello")) == ChannelError::Invalid, "hello cannot register work");
    auto command = f.Command();
    for (auto const* field : {"instance", "provider", "epoch"})
    {
        auto wrong = command;
        wrong[field] = "other";
        Check(f.Receive(wrong) == ChannelError::Stale, "source identity cannot change");
    }
    Check(f.Receive(f.Command("other", "unknown")) == ChannelError::Unknown, "unregistered request cannot command");
    Check(f.Receive(command) == ChannelError::None, "typed proposal stages");
    Check(f.Receive(command) == ChannelError::Duplicate, "effect replay blocked");
    // LCB6a: a generation result or failure is no inbound channel type; refusing one closes nothing.
    for (auto const* type : {"generation.result", "failure"})
    {
        auto stray = f.Wire(type);
        stray["text"] = "I shall wait here.";
        stray["code"] = "unavailable";
        Check(f.Receive(stray) == ChannelError::Invalid, "an inbound generation result or failure is invalid");
    }
    Check(f.Receive(f.Command("after-stray")) == ChannelError::None, "a stray post closes nothing");
    // LC.35: the game ends the route with its ids; whatever follows finds no route.
    f.policy.Drop("work");
    Check(f.Receive(f.Command("late")) == ChannelError::Unknown, "command cannot follow terminal");
    std::string body;
    ChannelEnvelope envelope{"fixture", "codex", "epoch-1", "work", deadline.unixMs};
    // WorldChannel finishes every command in the tick it arrives, so nothing is left to report after the terminal.
    Check(f.policy.FinishCommand(envelope, "effect", ChannelOutcome::Failed, f.scope, now, body) == ChannelError::Stale,
          "a closed route reports no staged proposal");
    Check(body.empty(), "rejected result has no stale output");
    auto nativeResult = f.Wire("command.result");
    nativeResult["commandId"] = "effect";
    nativeResult["outcome"] = "applied";
    Check(f.Receive(nativeResult) == ChannelError::Invalid, "native cannot supply game effect results");

    Fixture failed;
    failed.Open();
    failed.policy.Drop("work");
    auto const old = failed.Command("old");
    Check(failed.Receive(old) == ChannelError::Unknown, "a closed route cannot authorize effects");
    Check(failed.policy.Bind({"fixture", "codex", "", "epoch-2"}), "reconnect uses new epoch");
    Check(failed.Receive(old) == ChannelError::Stale, "the old epoch is retired");
    failed.scope.epoch = "epoch-2";
    Check(failed.policy.Receive(old.dump(), failed.scope, now, result, "work") == ChannelError::Stale,
          "an old command cannot cross reconnect");
    auto renewed = old;
    renewed["epoch"] = "epoch-2";
    Check(failed.policy.Receive(renewed.dump(), failed.scope, now, result, "work") == ChannelError::Unknown,
          "reconnect cannot reopen old work");
    // LC.35: only the previous binding's epoch is remembered (epochs are 128-bit random, never reissued).
    Check(!failed.policy.Bind({"fixture", "codex", "", "epoch-2"}), "retired epoch reuse fails closed");
    Check(failed.Receive(renewed) == ChannelError::Inactive, "bad activation leaves inactive");
}

void ExpiryAndScope()
{
    for (Clock clock : {Clock{101000, 200999}, Clock{100999, 201000}, Clock{90000, 201000}})
    {
        Fixture f;
        f.Open();
        // LC.35: expiry drops the route; the trusted clock's high-water mark keeps its id from reopening.
        Check(f.Receive(f.Command(), clock) == ChannelError::Unknown,
              "expiry equality on either clock including rollback");
        Check(f.policy.RequestReservations() == 0 && f.policy.Pending() == 0, "expiry drops the route");
        Check(f.policy.OpenGeneration(f.scope, "work", deadline, now) == ChannelError::Stale,
              "expired id cannot reopen");
    }
    Fixture f;
    f.Open();
    auto changedDeadline = f.Command();
    changedDeadline["deadlineUnixMs"] = deadline.unixMs + 1;
    Check(f.Receive(changedDeadline) == ChannelError::Stale, "cannot extend deadline");
    changedDeadline["deadlineUnixMs"] = deadline.unixMs - 1;
    Check(f.Receive(changedDeadline) == ChannelError::Stale, "must preserve exact deadline");
    Check(f.Receive(f.Command(), {100999, 200999}) == ChannelError::None, "last live millisecond accepted");
    // LCB6b: the ledger refuses a route of another bot, controller or context; whether the conversation is still
    // current (a revision, a lapse) is WorldPolicy's question, which the channel asks (bridge-world-selftest).
    for (auto const* field : {"bot", "controller", "context id", "context", "membership", "perception", "channel"})
    {
        Fixture revision;
        revision.Open();
        std::string const name = field;
        if (name == "bot")
            revision.scope.botGuid += 1;
        if (name == "context id")
            revision.scope.contextId = "context-2";
        if (std::string(field) == "controller")
            revision.scope.controllerGuid += 1;
        if (std::string(field) == "context")
            revision.scope.contextRevision += "-new";
        if (std::string(field) == "membership")
            revision.scope.membershipRevision += "-new";
        if (std::string(field) == "perception")
            revision.scope.perceptionScope += "-new";
        if (std::string(field) == "channel")
            revision.scope.channel = "direct";
        bool const owner = name == "bot" || name == "controller" || name == "context id";
        Check(revision.Receive(revision.Command()) == (owner ? ChannelError::Stale : ChannelError::None),
              "another route owner is stale; a scope revision is WorldPolicy's");
    }
    Fixture lapsed;
    Deadline const longDeadline{110000, 210000};
    Check(lapsed.policy.OpenGeneration(lapsed.scope, "work", longDeadline, now) == ChannelError::None, "long work");
    auto late = lapsed.Command();
    late["deadlineUnixMs"] = longDeadline.unixMs;
    Check(lapsed.Receive(late, {105000, 205000}) == ChannelError::None,
          "the ledger keeps a route to its own deadline; the conversation's liveness is WorldPolicy's");
    Check(lapsed.policy.OpenGeneration(lapsed.scope, "too-long", {225001, 325001}, {105000, 205000}) ==
              ChannelError::Invalid,
          "120 second maximum");
}

void Schemas()
{
    Fixture f;
    f.Open();
    ChannelMessage result;
    Check(f.policy.Receive("{", f.scope, now, result, "work") == ChannelError::Invalid, "malformed JSON");
    Check(
        f.policy.Receive(std::string(MaxRequestBytes + 1, ' '), f.scope, now, result, "work") == ChannelError::TooLarge,
        "bounded body before parse");
    Check(f.policy.Receive(std::string(MaxRequestBytes, ' '), f.scope, now, result, "work") == ChannelError::Invalid,
          "exact body maximum reaches parser");
    auto duplicate = f.Command().dump();
    duplicate.insert(1, "\"version\":1,");
    Check(f.policy.Receive(duplicate, f.scope, now, result, "work") == ChannelError::Invalid,
          "duplicate key before DOM overwrite");
    duplicate = f.Command().dump();
    auto const position = duplicate.find("\"params\":{}");
    duplicate.replace(position, 11, "\"params\":{\"x\":1,\"x\":2}");
    Check(f.policy.Receive(duplicate, f.scope, now, result, "work") == ChannelError::Invalid, "nested duplicate key");
    Check(f.policy.Receive(std::string(20, '[') + "0" + std::string(20, ']'), f.scope, now, result, "work") ==
              ChannelError::Invalid,
          "parser nesting bound");
    for (auto const& pair : std::vector<std::pair<std::string, Json>>{{"version", 1.0},
                                                                      {"requestId", "bad id"},
                                                                      {"deadlineUnixMs", 101000.0},
                                                                      {"deadlineUnixMs", -1},
                                                                      {"deadlineUnixMs", 18446744073709551615ULL},
                                                                      {"issuerGuid", 9007199254740993ULL},
                                                                      {"botGuid", "09007199254740994"},
                                                                      {"issuerGuid", "18446744073709551616"},
                                                                      {"issuerGuid", "0"},
                                                                      {"commandId", std::string(129, 'a')},
                                                                      {"steps", "follow"},
                                                                      {"steps", Json::array()},
                                                                      {"intent", "follow"},
                                                                      {"params", Json::object()},
                                                                      {"extra", "private"}})
    {
        auto bad = f.Command();
        bad[pair.first] = pair.second;
        Check(f.Receive(bad) == ChannelError::Invalid, "strict exact envelope or payload field");
    }
    // LCB2: one form; a follow or stay with any keyed params (the old standing shape) is invalid like any other.
    for (auto const& pair :
         std::vector<std::pair<std::string, Json>>{{"mark", {{"targetGuid", "01"}, {"icon", "star"}}},
                                                   {"mark", {{"target", "Hogger"}, {"icon", 7}}},
                                                   {"mark", {{"target", "Hogger"}, {"icon", "Skull"}}},
                                                   {"go_to", {{"place", ""}}},
                                                   {"go_to", {{"questId", 1}}},
                                                   {"follow", {{"targetGuid", "1"}}},
                                                   {"follow", {{"standing", true}}},
                                                   {"follow", {{"durationMs", 30000}}}})
    {
        auto bad = f.Command();
        bad["steps"] = Json::array({{{"act", pair.first}, {"params", pair.second}}});
        Check(f.Receive(bad) == ChannelError::Invalid, "strict act parameters");
    }
    Check(f.policy.CommandReservations() == 0, "malformed payloads reserve no effect ids");
    unsigned sequence = 0;
    for (auto const& [act, params, intent] : std::vector<std::tuple<std::string, Json, PlayerbotBridge::Intent>>{
             {"follow", Json::object(), PlayerbotBridge::Intent::Follow},
             {"mark", {{"targetGuid", "18446744073709551615"}, {"icon", "skull"}}, PlayerbotBridge::Intent::Mark},
             {"go_to", {{"place", std::string(64, 'm')}}, PlayerbotBridge::Intent::GoTo}})
    {
        auto good = f.Command("effect-" + std::to_string(++sequence));
        good["steps"] = Json::array({{{"act", act}, {"params", params}}});
        Check(f.policy.Receive(good.dump(), f.scope, now, result, "work") == ChannelError::None,
              "the catalogue's acts decode into typed steps");
        Check(std::get<ChannelCommand>(result.payload).steps.front().intent == intent, "the act's code preserved");
    }
}

void QueriesAndCapacity()
{
    Fixture f;
    f.Open();  // LC.35: queries open under a live generation route.
    ChannelMessage query;
    Check(f.policy.Receive(f.Query().dump(), f.scope, now, query, "work") == ChannelError::None,
          "authenticated reverse query");
    Check(f.Receive(f.Query()) == ChannelError::Duplicate, "query id replay reserved separately from effects");
    Check(f.Receive(f.Command("effect", "query")) == ChannelError::Invalid, "query ids never authorize commands");
    std::string body;
    std::vector<Participant> units{{f.scope.botGuid, "Visible Bot", "", -1, 0}};
    Check(f.policy.FinishQuery(query.envelope, f.scope, units, now, body) == ChannelError::None,
          "filtered owned result shape");
    auto parsed = Json::parse(body);
    Check(parsed.at("units").at(0).size() == 2, "unknown health role target omitted");
    Check(parsed.at("units").at(0).at("guid") == "9007199254740994", "guid preserved beyond double exactness");
    Check(f.policy.FinishQuery(query.envelope, f.scope, units, now, body) == ChannelError::Stale,
          "query terminal once");
    Check(body.empty(), "no stale query output after refusal");
    Check(f.policy.Receive(f.Query("q2").dump(), f.scope, now, query, "work") == ChannelError::None, "second query");
    units.front().healthPercent = 101;
    Check(f.policy.FinishQuery(query.envelope, f.scope, units, now, body) == ChannelError::Invalid, "invalid UI field");
    units.front().healthPercent = 80;
    units.front().role = "healer";
    units.front().visibleTargetGuid = 18446744073709551615ULL;
    Check(f.policy.FinishQuery(query.envelope, f.scope, units, now, body) == ChannelError::None,
          "observed optional fields retained");
    parsed = Json::parse(body);
    Check(parsed.at("units").at(0).size() == 5, "exact declared unit fields");
    Check(f.policy.Receive(f.Query("q3").dump(), f.scope, now, query, "work") == ChannelError::None,
          "query for local failure");
    Check(f.policy.FinishQuery(query.envelope, f.scope, std::vector<Participant>(41, units.front()), now, body) ==
              ChannelError::TooLarge,
          "unit count bounded before encoding");
    Check(f.policy.FinishQuery(query.envelope, f.scope, {units.front(), units.front()}, now, body) ==
              ChannelError::Invalid,
          "duplicate observed unit refused");
    Check(f.policy.FinishFailure(query.envelope, static_cast<ChannelFailure>(255), f.scope, now, body) ==
              ChannelError::Invalid,
          "unknown local failure enum refused");
    Check(f.policy.FinishFailure(query.envelope, ChannelFailure::Unavailable, f.scope, now, body) == ChannelError::None,
          "game-side query failure terminal");
    parsed = Json::parse(body);
    Check(parsed.size() == 8 && parsed.at("type") == "failure" && parsed.at("code") == "unavailable",
          "fixed game failure exact schema");
    Check(f.policy.FinishFailure(query.envelope, ChannelFailure::Cancelled, f.scope, now, body) == ChannelError::Stale,
          "local failure terminal once");
    Check(f.Receive(f.Command("query-after-failure", "q3")) == ChannelError::Closed, "failed query remains closed");
    Fixture slots(4);
    slots.Open();
    Check(slots.Receive(slots.Query()) == ChannelError::None, "LC.38: no ledger-wide pending count refuses a query");
    slots.policy.Drop("work");
    Check(slots.Receive(slots.Query()) == ChannelError::Unknown, "a closed route opens no query");
    slots.Open("work-2");
    Check(slots.policy.RequestReservations() == 1, "a closed route keeps no replay reservation");
    Fixture commands(2);
    commands.Open();
    Check(commands.Receive(commands.Command("a")) == ChannelError::None, "first effect reservation");
    Check(commands.Receive(commands.Command("b")) == ChannelError::None, "last effect reservation");
    Check(commands.Receive(commands.Command("c")) == ChannelError::Capacity,
          "the route's id bound rejects instead of eviction");
    Check(commands.Receive(commands.Command("a")) == ChannelError::Duplicate, "oldest live effect retained");
    // LC.35: 5,000 generations with 3 queries each in one epoch, every one admitted. Each closed route leaves the
    // ledger with its ids, so a late replay of any of them finds no route.
    Fixture full;
    for (std::size_t i = 0; i < 5000; ++i)
    {
        auto const id = "work-" + std::to_string(i);
        Check(full.policy.OpenGeneration(full.scope, id, deadline, now) == ChannelError::None,
              "5000: every generation admitted");
        for (std::size_t k = 0; k < 3; ++k)
        {
            ChannelMessage asked;
            Check(full.policy.Receive(full.Query("q-" + std::to_string(i) + "-" + std::to_string(k)).dump(), full.scope,
                                      now, asked, id) == ChannelError::None,
                  "5000: every query admitted");
        }
        Check(full.Receive(full.Command("c-" + std::to_string(i), id)) == ChannelError::None,
              "5000: every command admitted");
        full.policy.Drop(id);
    }
    Check(
        full.policy.RequestReservations() == 0 && full.policy.CommandReservations() == 0 && full.policy.Pending() == 0,
        "closed routes leave an empty ledger");
    ChannelMessage late;
    Check(full.Receive(full.Command("c-0", "work-0")) == ChannelError::Unknown &&
              full.policy.Receive(full.Query("q-0-0").dump(), full.scope, now, late, "work-0") == ChannelError::Unknown,
          "a late replay of a closed route's command or query is unknown_request");
    Fixture pair;
    pair.Open("route-a");
    pair.Open("route-b");
    ChannelMessage asked;
    Check(pair.policy.Receive(pair.Query("qa").dump(), pair.scope, now, asked, "route-a") == ChannelError::None &&
              pair.policy.Receive(pair.Query("qb").dump(), pair.scope, now, asked, "route-b") == ChannelError::None &&
              pair.Receive(pair.Command("ca", "route-a")) == ChannelError::None &&
              pair.Receive(pair.Command("cb", "route-b")) == ChannelError::None,
          "two routes, each with a query and a command");
    pair.policy.Drop("route-a");
    Check(pair.policy.RequestReservations() == 2 && pair.policy.CommandReservations() == 1,
          "only the closed route's ids leave the ledger");
    Check(
        pair.Receive(pair.Command("cb", "route-b")) == ChannelError::Duplicate &&
            pair.policy.Receive(pair.Query("qb").dump(), pair.scope, now, asked, "route-b") == ChannelError::Duplicate,
        "the open route's ids stay refused as duplicates");
    Check(pair.Receive(pair.Command("ca", "route-a")) == ChannelError::Unknown,
          "a closed route's command id never lands on that route");
    // A dropped route's ids are answered nowhere: not under their own envelope and not under the open route's.
    std::string none;
    auto const envelopeOf = [&](std::string id)
    {
        return ChannelEnvelope{pair.scope.instance, pair.scope.provider, pair.scope.epoch, std::move(id),
                               deadline.unixMs};
    };
    Check(pair.policy.FinishCommand(envelopeOf("route-a"), "ca", ChannelOutcome::Applied, pair.scope, now, none) ==
                  ChannelError::Stale &&
              pair.policy.FinishCommand(envelopeOf("route-b"), "ca", ChannelOutcome::Applied, pair.scope, now, none) ==
                  ChannelError::Unknown &&
              pair.policy.FinishQuery(envelopeOf("qa"), pair.scope, {}, now, none) == ChannelError::Stale &&
              none.empty(),
          "a closed route's command and query are answered on no route");
    Check(pair.policy.FinishCommand(envelopeOf("route-b"), "cb", ChannelOutcome::Applied, pair.scope, now, none) ==
                  ChannelError::None &&
              Json::parse(none).at("requestId") == "route-b",
          "the open route's command still reports once, on its own route");
    Fixture aging;
    aging.Open("old");
    Check(aging.policy.Receive(aging.Query("old-q").dump(), aging.scope, now, asked, "old") == ChannelError::None &&
              aging.Receive(aging.Command("old-c", "old")) == ChannelError::None,
          "expiry: a route with a query and a command");
    Clock const after{deadline.unixMs, deadline.steadyMs};
    aging.policy.Expire(after);
    Check(aging.policy.RequestReservations() == 0 && aging.policy.CommandReservations() == 0,
          "expiry: a route at its deadline leaves the ledger with its ids");
    Check(aging.Receive(aging.Command("old-c", "old"), after) == ChannelError::Unknown,
          "expiry: a late replay finds no route");
    ChannelPolicy invalid(0);
    Check(!invalid.Bind({"fixture", "codex", "", "invalid-limits"}), "zero capacity fails closed");
    ChannelPolicy tooMany(ChannelMaxRouteIds + 1);
    Check(!tooMany.Bind({"fixture", "codex", "", "invalid-limits"}), "cannot expand id bound");

    Fixture effects;
    effects.Open();
    for (std::size_t i = 0; i < ChannelMaxRouteIds; ++i)
        Check(effects.Receive(effects.Command("effect-" + std::to_string(i))) == ChannelError::None,
              "a route holds its full id bound");
    Check(effects.policy.CommandReservations() == ChannelMaxRouteIds, "exact route id maximum");
    Check(effects.Receive(effects.Command("overflow")) == ChannelError::Capacity, "a full route refuses another id");
    Check(effects.Receive(effects.Command("effect-0")) == ChannelError::Duplicate, "first effect never evicted");
    Check(effects.policy.Receive(effects.Query("q-over").dump(), effects.scope, now, asked, "work") ==
              ChannelError::Capacity,
          "queries share the route's id bound");
    // LC.38: the ledger keeps no scope table and no count of bots in talk: routes of 200 bots open.
    Fixture bots;
    for (unsigned i = 1; i <= 200; ++i)
    {
        auto scope = bots.scope;
        scope.botGuid += i;
        scope.contextId = "context-" + std::to_string(i);
        Check(bots.policy.OpenGeneration(scope, "bot-" + std::to_string(i), deadline, now) == ChannelError::None,
              "LC.38: a route per bot, no fixed count");
    }
    Check(bots.policy.RequestReservations() == 200, "200 bots' routes held");
    // LC.35: no epoch history bound; only the previous binding's epoch is refused (epochs are 128-bit random).
    ChannelPolicy epochs;
    for (unsigned i = 0; i < 5000; ++i)
        Check(epochs.Bind({"fixture", "codex", "", "epoch-" + std::to_string(i)}),
              "fresh epochs bind without a history bound");
    Check(!epochs.Bind({"fixture", "codex", "", "epoch-4999"}), "the previous binding's epoch is refused");
    ChannelMessage output;
    Check(epochs.Receive(f.Query().dump(), f.scope, now, output, "work") == ChannelError::Inactive,
          "a refused rebind leaves inactive");
}
}  // namespace

// J5f-b: bot_state queries, one topic each; entries filtered and bounded per topic.
Json StateQuery(Fixture const& f, std::string const& id, Json topic)
{
    auto json = f.Wire("query", id);
    json["controllerGuid"] = std::to_string(f.scope.controllerGuid);
    json["botGuid"] = std::to_string(f.scope.botGuid);
    json["query"] = "bot_state";
    json["topic"] = std::move(topic);
    return json;
}

Json Topic(Fixture& f, std::string const& id, StateTopic topic, std::vector<StateEntry> const& entries)
{
    ChannelMessage message;
    auto wire = StateQuery(f, id, StateTopicName(topic));
    if (topic == StateTopic::Route)
        wire["place"] = "Goldshire";  // WT18b: route names its place.
    Check(f.policy.Receive(wire.dump(), f.scope, now, message, "work") == ChannelError::None,
          "bot_state query admitted");
    auto const& query = std::get<ChannelQuery>(message.payload);
    Check(query.kind == QueryKind::BotState && query.topic == topic &&
              query.place == (topic == StateTopic::Route ? "Goldshire" : ""),
          "typed bot_state topic");
    std::string body;
    Check(f.policy.FinishTopic(message.envelope, f.scope, topic, entries, now, body) == ChannelError::None,
          "topic result encoded");
    Check(body.size() <= MaxResponseBytes, "topic result within the response cap");
    auto json = Json::parse(body);
    Check(json.size() == 9 && json.at("type") == "query.result" && json.at("topic") == StateTopicName(topic) &&
              json.at("entries").is_array() && !json.contains("units"),
          "topic result shape");
    Check(json.at("entries").dump().size() <= ChannelMaxTopicBytes, "topic entries within 8 KB");
    return json.at("entries");
}

bool Keys(Json const& entry, std::set<std::string> const& keys)
{
    std::set<std::string> found;
    for (auto const& item : entry.items())
        found.insert(item.key());
    return found == keys;
}

// LCB1e: a topic's limit is its installed catalogue row's.
std::size_t RowLimit(StateTopic topic)
{
    for (auto const& row : Abilities().rows)
        if (row.topic && row.code == static_cast<uint8_t>(topic))
            return row.limit;
    return 0;
}

void Topics()
{
    Fixture f;
    f.Open();
    // Parse: the fourteen topics, nothing else.
    for (std::string bad : {"bank", "", "Inventory", "talents", "Plan", "quest plan", "questplan", "place"})
        Check(f.Receive(StateQuery(f, "bad-" + bad, bad)) == ChannelError::Invalid, "unknown topic invalid_input");
    Check(f.Receive(StateQuery(f, "bad-number", 1)) == ChannelError::Invalid, "topic must be a string");
    auto missing = StateQuery(f, "missing", "inventory");
    missing.erase("topic");
    Check(f.Receive(missing) == ChannelError::Invalid, "bot_state needs a topic");
    auto extra = f.Wire("query", "extra");
    extra["controllerGuid"] = std::to_string(f.scope.controllerGuid);
    extra["botGuid"] = std::to_string(f.scope.botGuid);
    extra["query"] = "observable_group";
    extra["topic"] = "inventory";
    Check(f.Receive(extra) == ChannelError::Invalid, "observable_group takes no topic");
    auto spare = StateQuery(f, "spare", "spells");
    spare["units"] = Json::array();
    Check(f.Receive(spare) == ChannelError::Invalid, "bot_state exact keys");
    auto other = StateQuery(f, "other-bot", "spells");
    other["botGuid"] = "5";
    Check(f.Receive(other) == ChannelError::Stale, "bot_state only for the route's own bot");
    // WT18b: route, and only route, names its place: text of 1..64 bytes, not blank, no control characters.
    Check(f.Receive(StateQuery(f, "route-bare", "route")) == ChannelError::Invalid, "route needs its place");
    for (Json const& place : {Json(""), Json(std::string(65, 'p')), Json("   "), Json("Gold\nshire"),
                              Json(std::string("Gold\x7Fshire")), Json(12), Json::object()})
    {
        auto bad = StateQuery(f, "route-bad", "route");
        bad["place"] = place;
        Check(f.Receive(bad) == ChannelError::Invalid, "a route place is bounded text");
    }
    for (std::size_t t = 0; t < StateTopicCount; ++t)
        if (static_cast<StateTopic>(t) != StateTopic::Route)
        {
            auto placed = StateQuery(f, "placed-" + std::to_string(t), StateTopicName(static_cast<StateTopic>(t)));
            placed["place"] = "Goldshire";
            Check(f.Receive(placed) == ChannelError::Invalid, "only route takes a place");
        }
    std::string widest;
    for (int i = 0; i < 32; ++i)
        widest += "\xC3\xA9";
    auto longest = StateQuery(f, "route-64", "route");
    longest["place"] = widest;
    ChannelMessage routed;
    Check(f.policy.Receive(longest.dump(), f.scope, now, routed, "work") == ChannelError::None &&
              std::get<ChannelQuery>(routed.payload).place == widest,
          "a place of 64 bytes is carried as given");
    longest = StateQuery(f, "route-66", "route");
    longest["place"] = widest + "\xC3\xA9";
    Check(f.Receive(longest) == ChannelError::Invalid, "a place over 64 bytes is refused");
    Check(f.Receive(StateQuery(f, "state-1", "quests")) == ChannelError::None, "bot_state reserves its id");
    Check(f.Receive(StateQuery(f, "state-1", "quests")) == ChannelError::Duplicate, "bot_state id replay refused");
    Check(f.Receive(f.Command("effect-state", "state-1")) == ChannelError::Invalid,
          "a bot_state query id never authorizes a command");

    // Inventory: 40 entries, quality names, kinds, counts.
    std::vector<StateEntry> items;
    for (int i = 0; i < 50; ++i)
        items.push_back({"Item " + std::to_string(i), i < 19 ? "equipped" : "bag", i % 8, i == 0 ? 0 : 5000});
    items.insert(items.begin(), {{"Bad quality", "bag", 8, 1}, {"Bad kind", "bank", 1, 1}, {"", "bag", 1, 1}});
    auto entries = Topic(f, "inv", StateTopic::Inventory, items);
    Check(entries.size() == 40, "inventory at most 40 entries");
    Check(entries.at(0).at("name") == "Item 0" && Keys(entries.at(0), {"where", "name", "quality", "count"}),
          "inventory fixed keys, bad quality, kind and empty name dropped");
    Check(entries.at(0).at("where") == "equipped" && entries.at(0).at("quality") == "poor" &&
              entries.at(0).at("count") == 1 && entries.at(7).at("quality") == "heirloom" &&
              entries.at(19).at("where") == "bag" && entries.at(1).at("count") == 1000,
          "inventory values bounded and named");

    // Quests: the row's limit of entries, done never above needed.
    std::vector<StateEntry> quests{{"Odd status", "abandoned", 1, 2}};
    for (int i = 0; i < 50; ++i)
        quests.push_back({"Quest " + std::to_string(i),
                          i % 3 == 0   ? "complete"
                          : i % 3 == 1 ? "failed"
                                       : "active",
                          i == 0 ? 99 : -4, i == 0 ? 8 : 20000});
    entries = Topic(f, "quests", StateTopic::Quests, quests);
    Check(entries.size() == RowLimit(StateTopic::Quests) && entries.size() == 40 &&
              Keys(entries.at(0), {"title", "status", "done", "needed"}),
          "quests at most the row's limit with fixed keys");
    Check(entries.at(0).at("title") == "Quest 0" && entries.at(0).at("done") == 8 && entries.at(0).at("needed") == 8 &&
              entries.at(0).at("status") == "complete" && entries.at(1).at("status") == "failed" &&
              entries.at(2).at("status") == "active" && entries.at(1).at("done") == 0 &&
              entries.at(1).at("needed") == 10000,
          "quest progress bounded");

    // Cooldowns: the row's limit of entries, only positive seconds, capped at a week.
    std::vector<StateEntry> cooldowns{{"Ready", "", 0, 0}};
    for (int i = 0; i < 50; ++i)
        cooldowns.push_back({"Spell " + std::to_string(i), "", i == 0 ? 100000000 : 60 - i, 0});
    entries = Topic(f, "cooldowns", StateTopic::Cooldowns, cooldowns);
    Check(entries.size() == RowLimit(StateTopic::Cooldowns) && entries.size() == 40 && Keys(entries.at(0), {"name", "seconds"}) && entries.at(0).at("name") == "Spell 0" &&
              entries.at(0).at("seconds") == 604800,
          "cooldowns at most the row's limit, ready spells dropped, seconds capped");

    // Strategies: whitelist, kinds, repeats dropped.
    entries = Topic(f, "strategies", StateTopic::Strategies,
                    {{"tank", "combat"},
                     {"debug", "combat"},
                     {"follow", "non_combat"},
                     {"follow", "non_combat"},
                     {"heal", "sideways"},
                     {"rtsc", "non_combat"},
                     {"Tank", "combat"}});
    Check(entries.size() == 2 && entries.at(0).at("name") == "tank" && entries.at(0).at("kind") == "combat" &&
              entries.at(1).at("name") == "follow" && Keys(entries.at(1), {"name", "kind"}),
          "strategies through the whitelist, unknown and repeated dropped");

    // Spells: 40 entries, rank bounded.
    std::vector<StateEntry> spells;
    for (int i = 0; i < 50; ++i)
        spells.push_back({"Spell " + std::to_string(i), "", i == 0 ? 150 : i == 1 ? -1 : 3, 0});
    entries = Topic(f, "spells", StateTopic::Spells, spells);
    Check(entries.size() == 40 && Keys(entries.at(0), {"name", "rank"}) && entries.at(0).at("rank") == 99 &&
              entries.at(1).at("rank") == 0 && entries.at(2).at("rank") == 3,
          "spells at most 40, rank bounded");

    Check(std::string(StateTopicName(StateTopic::Plan)) == "plan" &&
              std::string(StateTopicName(StateTopic::Options)) == "options" &&
              std::string(StateTopicName(StateTopic::Route)) == "route" &&
              std::string(StateTopicName(StateTopic::QuestPlan)) == "quest_plan" &&
              std::string(StateTopicName(StateTopic::Bags)) == "bags" &&
              std::string(StateTopicName(StateTopic::Places)) == "places",
          "planner topic names");
    Check(std::string(StateTopicName(StateTopic::History)) == "history" &&
              std::string(StateTopicName(StateTopic::Lately)) == "lately" &&
              std::string(StateTopicName(StateTopic::Known)) == "known",
          "LC7: record topic names");
    entries = Topic(f, "plan", StateTopic::Plan,
                    {{"rpg_travel", "goal", 120, 600},
                     {"planner", "source", -5, 5000000000LL},
                     {"near quests", "reason", 0, 0},
                     {"Kobold Camp Cleanup", "stop", 3, 250},
                     {"bogus", "option", 1, 1},
                     {"none", "", 0, 0},
                     {"", "goal", 1, 1},
                     {"spaced", "Not a word", 1, 1},
                     {"long", std::string(65, 'w'), 1, 1},
                     {"bad sentence", "goal", 2, 2, "Bad\nline"}});
    Check(entries.size() == 6 && Keys(entries.at(0), {"row", "name", "value", "extra"}) &&
              entries.at(0).at("row") == "goal" && entries.at(0).at("name") == "rpg_travel" &&
              entries.at(0).at("value") == 120 && entries.at(0).at("extra") == 600 &&
              entries.at(1).at("row") == "source" && entries.at(1).at("value") == 0 &&
              entries.at(1).at("extra") == 4294967295LL && entries.at(2).at("name") == "near quests" &&
              entries.at(3).at("row") == "stop" && entries.at(3).at("name") == "Kobold Camp Cleanup" &&
              entries.at(4).at("row") == "option" && entries.at(5).at("name") == "bad sentence" &&
              Keys(entries.at(5), {"row", "name", "value", "extra"}),
          "plan rows: the game's row word whichever it is, name, value and extra bounded; no word, a word not of "
          "[a-z_] 1..64 and an empty name dropped; a bad sentence left out");
    // LCB1e: the rows past bags' old three (the mail, the kept items) reach the wire, each with the game's sentence.
    entries = Topic(f, "bags", StateTopic::Bags,
                    {{"free", "free", 6, 16},
                     {"Linen Cloth", "sell", 3, 40},
                     {"mail", "mail_waiting", 1, 0, "Mail waits at the mailbox."},
                     {"Copper Ore", "kept_bank", 2, 0, "Copper Ore, kept in the bank."}});
    Check(entries.size() == 4 && !entries.at(0).contains("text") && entries.at(2).at("row") == "mail_waiting" &&
              entries.at(2).at("text") == "Mail waits at the mailbox." &&
              Keys(entries.at(2), {"row", "name", "text", "value", "extra"}) &&
              entries.at(3).at("row") == "kept_bank" && entries.at(3).at("text") == "Copper Ore, kept in the bank.",
          "bags: the mail and kept rows pass with their sentences; a row without one has none");
    for (StateTopic const topic : {StateTopic::Plan, StateTopic::Options, StateTopic::Route, StateTopic::QuestPlan,
                                   StateTopic::Bags, StateTopic::Places, StateTopic::History, StateTopic::Lately,
                                   StateTopic::Known})
    {
        std::vector<StateEntry> rows;
        for (int i = 0; i < 30; ++i)
            rows.push_back({"word " + std::to_string(i), i % 2 ? "row_odd" : "row_even", i, i, "Words."});
        auto const limited = Topic(f, std::string("limit-") + StateTopicName(topic), topic, rows);
        Check(limited.size() == RowLimit(topic) && limited.size() > 1 && limited.at(1).at("row") == "row_odd" &&
                  limited.at(1).at("text") == "Words.",
              "a planner or record topic keeps the game's row words and sentences and stops at its row's limit");
    }

    // LC7: a record topic's entry carries its sentence in its own field, cut at its own cap; no sentence, no entry.
    entries = Topic(f, "history", StateTopic::History,
                    {{"kill", "event", 3, 0, "You slew Hogger, an elite creature, in Elwynn Forest."},
                     {"Westfall", "zone", 2, 0, "You have spent time in Westfall."},
                     {"kill", "deed", 1, 0, "A row word the module does not know."},
                     {"quest", "event", 1, 0, ""},
                     {"level", "event", 1, 0, "Bad\nline"},
                     {"spell", "event", 0, 0, std::string(300, 's')}});
    Check(entries.size() == 4 && Keys(entries.at(0), {"row", "name", "text", "value", "extra"}) &&
              entries.at(0).at("text") == "You slew Hogger, an elite creature, in Elwynn Forest." &&
              entries.at(0).at("name") == "kill" && entries.at(1).at("row") == "zone" &&
              entries.at(2).at("row") == "deed" && entries.at(3).at("text") == std::string(ChannelMaxTopicText, 's'),
          "history rows: row (any word the game sends), name, text, value, extra; a missing or bad sentence drops the "
          "entry, a long one is cut");
    entries = Topic(f, "known", StateTopic::Known,
                    {{"Aldric", "bot", 0, 2, "Aldric, a human paladin, fought beside you."}, {"Brug", "bot", 1, 1, ""}});
    Check(entries.size() == 1 && entries.at(0).at("name") == "Aldric" && entries.at(0).at("extra") == 2,
          "known rows: the bot by name with the events shared");

    // Names: cut to 64 bytes on a character boundary; control characters and invalid UTF-8 dropped.
    std::string accented;
    for (int i = 0; i < 40; ++i)
        accented += "\xC3\xA9";
    entries = Topic(f, "names", StateTopic::Spells,
                    {{std::string(100, 'x'), "", 1, 0},
                     {"x" + accented, "", 1, 0},
                     {"Bad\nline", "", 1, 0},
                     {"Bad \xFF byte", "", 1, 0},
                     {"   ", "", 1, 0}});
    Check(entries.size() == 2, "names with controls, invalid UTF-8 or no text dropped");
    Check(entries.at(0).at("name") == std::string(64, 'x'), "name cut at 64 bytes");
    Check(entries.at(1).at("name").get<std::string>().size() == 63, "name cut on a character boundary");

    // Worst case per topic stays inside 8 KB without the byte cut.
    for (std::size_t t = 0; t < StateTopicCount; ++t)
    {
        auto const topic = static_cast<StateTopic>(t);
        std::vector<StateEntry> worst;
        for (int i = 0; i < 60; ++i)
            worst.push_back({std::to_string(i) + std::string(64, 'w'),
                             topic == StateTopic::Inventory    ? "equipped"
                             : topic == StateTopic::Quests     ? "complete"
                             : topic == StateTopic::Strategies ? "non_combat"
                                                               : "row_word",
                             topic == StateTopic::Inventory ? 5 : 604800, 10000,
                             std::string(ChannelMaxTopicText, 'w')});
        auto const result = Topic(f, "worst-" + std::to_string(t), topic, worst);
        Check(result.size() == (topic == StateTopic::Strategies ? 0 : RowLimit(topic)), "worst case count");
    }

    // Terminal once, unknown topic value refused, wrong work refused.
    ChannelMessage message;
    Check(f.policy.Receive(StateQuery(f, "once", "spells").dump(), f.scope, now, message, "work") == ChannelError::None,
          "query for terminal check");
    std::string body;
    Check(f.policy.FinishTopic(message.envelope, f.scope, static_cast<StateTopic>(StateTopicCount), {}, now, body) ==
                  ChannelError::Invalid &&
              body.empty(),
          "unknown topic value refused");
    Check(f.policy.FinishTopic(message.envelope, f.scope, StateTopic::Spells, {}, now, body) == ChannelError::None &&
              Json::parse(body).at("entries").empty(),
          "empty topic result");
    Check(f.policy.FinishTopic(message.envelope, f.scope, StateTopic::Spells, {}, now, body) == ChannelError::Stale &&
              body.empty(),
          "topic result terminal once");
    f.Open("gen");
    ChannelEnvelope envelope{f.scope.instance, f.scope.provider, f.scope.epoch, "gen", deadline.unixMs};
    Check(f.policy.FinishTopic(envelope, f.scope, StateTopic::Spells, {}, now, body) == ChannelError::Stale,
          "a generation route never carries a topic result");
}

// J6e-c (DS.14 (c)): a watch on the route's own bot; its id is a command id (one replay reservation).
Json WatchWire(Fixture const& f, std::string const& id, Json condition = "combat_end", Json until = 160000,
               std::string const& request = "work")
{
    auto json = f.Wire("watch", request);
    json["commandId"] = id;
    json["botGuid"] = std::to_string(f.scope.botGuid);
    json["condition"] = std::move(condition);
    json["untilUnixMs"] = std::move(until);
    return json;
}

// LC3: the installed ability index is the channel's only word list; query: catalogue answers with it.
Json CatalogueQuery(Fixture const& f, std::string const& id)
{
    auto json = f.Query(id);
    json["query"] = "catalogue";
    return json;
}

void Catalogue()
{
    auto const bytes = SetAbilityIndex(TestAbilityIndex());
    Check(bytes > 0 && bytes <= ChannelMaxCatalogueBytes && Abilities().rows.size() == 17 && CatalogueInstalled(),
          "the double installs");
    Check(ChannelMaxCatalogueBytes == 49152 && ChannelMaxCatalogueBytes + 4096 < MaxCatalogueResponseBytes &&
              MaxResponseBytes == 32768,
          "the catalogue's budget leaves its answer headroom; the other bodies keep 32 KB");

    // A refused index leaves the channel none: no act, no topic, no catalogue.
    using Change = void (*)(AbilityIndex&);
    Change const refused[] = {
        [](AbilityIndex& index) { index.rows[0].name = "Follow"; },
        [](AbilityIndex& index) { index.rows[1].name = index.rows[0].name; },
        [](AbilityIndex& index) { index.rows[1].code = index.rows[0].code; },
        [](AbilityIndex& index) { index.rows[4].name = index.rows[3].name; },
        [](AbilityIndex& index) { index.rows[4].code = index.rows[3].code; },
        [](AbilityIndex& index) { index.rows[10].params.push_back(index.rows[10].params.front()); },
        [](AbilityIndex& index) { index.rows[3].name = "inventory items"; },
        [](AbilityIndex& index) { index.rows.clear(); },
        [](AbilityIndex& index) { index.version = 0; },
        [](AbilityIndex& index) { index.maxSteps = 0; },
    };
    for (Change change : refused)
    {
        auto index = TestAbilityIndex();
        change(index);
        Check(SetAbilityIndex(std::move(index)) == 0, "a malformed index is refused before it is encoded");
        Fixture f;
        f.Open();
        Check(Abilities().rows.empty() && !CatalogueInstalled() && f.Receive(f.Command()) == ChannelError::Invalid &&
                  f.Receive(StateQuery(f, "refused-topic", "quests")) == ChannelError::Invalid &&
                  !*StateTopicName(StateTopic::Quests) && f.policy.CommandReservations() == 0,
              "a refused index leaves no act, no topic and no topic name");
    }
    // LC3b (LC3a review 1): a catalogue the query cannot send, over its budget or not UTF-8, drops only the
    // catalogue: the acts and topics stand.
    auto const standing = [](char const* label)
    {
        Fixture f;
        f.Open();
        Check(Abilities().rows.size() == 17 && !CatalogueInstalled() &&
                  f.Receive(f.Command("standing")) == ChannelError::None &&
                  f.Receive(StateQuery(f, "standing-topic", "quests")) == ChannelError::None &&
                  std::string(StateTopicName(StateTopic::Quests)) == "quests",
              label);
    };
    auto large = TestAbilityIndex();
    large.rows[0].sentence = std::string(ChannelMaxCatalogueBytes, 'x');
    Check(SetAbilityIndex(std::move(large)) > ChannelMaxCatalogueBytes, "a catalogue over its budget, with its size");
    standing("a catalogue over its budget drops only the catalogue");
    auto broken = TestAbilityIndex();
    broken.rows[0].sentence = "\xC3";
    Check(SetAbilityIndex(std::move(broken)) == 0, "a catalogue that is not UTF-8 has no size");
    standing("a catalogue that is not UTF-8 drops only the catalogue");

    // The words are the index's: a renamed act or topic is taken under its new word only.
    auto renamed = TestAbilityIndex();
    renamed.rows[0].name = "chase";
    renamed.rows[8].name = "plans";
    renamed.rows.erase(renamed.rows.begin() + 4);
    Check(SetAbilityIndex(std::move(renamed)) > 0, "a renamed double installs");
    {
        Fixture f;
        f.Open();
        auto chase = f.Command("renamed");
        Check(f.Receive(chase) == ChannelError::Invalid, "the old word is no act once the index renames it");
        chase["steps"][0]["act"] = "chase";
        ChannelMessage result;
        Check(f.policy.Receive(chase.dump(), f.scope, now, result, "work") == ChannelError::None &&
                  std::get<ChannelCommand>(result.payload).steps.front().intent == PlayerbotBridge::Intent::Follow,
              "the index's word decodes to its row's code");
        Check(f.Receive(StateQuery(f, "old-topic", "plan")) == ChannelError::Invalid &&
                  f.Receive(StateQuery(f, "gone-topic", "quests")) == ChannelError::Invalid &&
                  f.Receive(StateQuery(f, "kept-topic", "inventory")) == ChannelError::None,
              "only a topic the index lists is taken");
        auto const plans = Topic(f, "new-topic", StateTopic::Plan, {{"rpg_travel", "goal", 1, 2}});
        Check(plans.size() == 1 && std::string(StateTopicName(StateTopic::Plan)) == "plans",
              "a topic is answered under the index's word");
    }
    Check(InstallTestAbilities(), "the double installs again");
    {
        Fixture f;
        f.Open();
        Check(f.Receive(StateQuery(f, "act-topic", "follow")) == ChannelError::Invalid, "an act is no bot_state topic");
        auto act = f.Command("topic-act");
        act["steps"][0]["act"] = "inventory";
        Check(f.Receive(act) == ChannelError::Invalid, "a topic is no act");
    }

    // The query: observable_group's keys, the route's own bot, an id of its own.
    Fixture f;
    f.Open();
    auto extra = CatalogueQuery(f, "cat-extra");
    extra["topic"] = "plan";
    Check(f.Receive(extra) == ChannelError::Invalid, "catalogue takes no topic");
    auto missing = CatalogueQuery(f, "cat-missing");
    missing.erase("botGuid");
    Check(f.Receive(missing) == ChannelError::Invalid, "catalogue takes the route's keys");
    auto other = CatalogueQuery(f, "cat-other");
    other["botGuid"] = "12";
    Check(f.Receive(other) == ChannelError::Stale, "catalogue only for the route's own bot");
    auto upper = CatalogueQuery(f, "cat-upper");
    upper["query"] = "Catalogue";
    Check(f.Receive(upper) == ChannelError::Invalid, "an unknown query stays invalid");
    ChannelMessage message;
    Check(f.policy.Receive(CatalogueQuery(f, "cat").dump(), f.scope, now, message, "work") == ChannelError::None &&
              std::get<ChannelQuery>(message.payload).kind == QueryKind::Catalogue,
          "a catalogue query admitted, typed");
    Check(f.Receive(CatalogueQuery(f, "cat")) == ChannelError::Duplicate, "catalogue id replay refused");
    Check(f.Receive(f.Command("effect-catalogue", "cat")) == ChannelError::Invalid,
          "a catalogue query id never authorizes a command");

    // The answer: the installed index field for field, hash as 16 lowercase hex digits, within its budget.
    std::string body;
    Check(f.policy.FinishCatalogue(message.envelope, f.scope, now, body) == ChannelError::None, "catalogue answered");
    auto const json = Json::parse(body);
    Check(json.size() == 8 && json.at("type") == "query.result" && json.at("version") == 1 &&
              json.at("requestId") == "cat",
          "a query.result envelope that holds the catalogue");
    auto const& catalogue = json.at("catalogue");
    Check(catalogue.size() == 5 && catalogue.at("hash") == "0a1b2c3d4e5f6071" && catalogue.at("version") == 1 &&
              catalogue.at("maxSteps") == 3 &&
              catalogue.at("abilities") == Json::array({"catalogue", "names", "steps", "facts"}) &&
              catalogue.at("rows").size() == 17,
          "hash, version, abilities, the most steps and every row");
    Check(catalogue.at("rows").at(1) == Json::parse(R"({"kind":"act","name":"mark",
              "sentence":"Put a raid icon on a unit you can see.",
              "params":[{"name":"target","kind":"unit","sentence":"The unit to mark, by name.","choices":[]},
                        {"name":"icon","kind":"choice","sentence":"The raid icon to put on it.",
                         "choices":["star","circle","diamond","triangle","moon","square","cross","skull"]}],
              "refusals":[{"word":"battleground","sentence":"You are in a battleground, where this cannot be done."},
                          {"word":"travelling","sentence":"You are in flight; this cannot be done until you land."},
                          {"word":"dead","sentence":"You are dead."},
                          {"word":"not_in_group","sentence":"The one who asked is not in your group."},
                          {"word":"unknown_name","sentence":"You know nothing by that name."}],
              "effect":"none","done":"at_once","state":"marking {target}","event":"BridgeAct","limit":0})"),
          "an act row, field for field");
    Check(catalogue.at("rows").at(10) == Json::parse(R"({"kind":"topic","name":"route","sentence":"Your route.",
              "params":[{"name":"place","kind":"place","sentence":"A known place's name.","choices":[]}],
              "refusals":[],"effect":"none","done":"at_once","state":"","event":"","limit":12})"),
          "a topic row, field for field");
    Check(catalogue.at("rows").at(0).at("name") == "follow" && catalogue.at("rows").at(2).at("name") == "go_to" &&
              catalogue.at("rows").at(3).at("name") == "inventory" &&
              catalogue.at("rows").at(13).at("name") == "places",
          "the rows in the index's order");
    Check(body.size() < ChannelMaxCatalogueBytes, "the answer is below its budget");
    Check(f.policy.FinishCatalogue(message.envelope, f.scope, now, body) == ChannelError::Stale && body.empty(),
          "a catalogue is answered once");
    f.Open("gen-catalogue");
    ChannelEnvelope generation{f.scope.instance, f.scope.provider, f.scope.epoch, "gen-catalogue", deadline.unixMs};
    Check(f.policy.FinishCatalogue(generation, f.scope, now, body) == ChannelError::Stale && body.empty(),
          "a generation route never carries a catalogue");

    // Without an index the query is still admitted, and no catalogue is answered.
    SetAbilityIndex({});
    Fixture none;
    none.Open();
    ChannelMessage asked;
    std::string empty;
    Check(none.policy.Receive(CatalogueQuery(none, "cat-none").dump(), none.scope, now, asked, "work") ==
                  ChannelError::None &&
              none.policy.FinishCatalogue(asked.envelope, none.scope, now, empty) == ChannelError::Invalid &&
              empty.empty(),
          "no catalogue without an index");
    Check(InstallTestAbilities(), "the double installs once more");
}

void Watches()
{
    Fixture f;
    f.Open();
    std::pair<char const*, WatchCondition> const conditions[] = {{"combat_end", WatchCondition::CombatEnd},
                                                                 {"activity_done", WatchCondition::ActivityDone},
                                                                 {"time", WatchCondition::Time},
                                                                 {"none", WatchCondition::None}};
    for (auto const& [name, condition] : conditions)
    {
        ChannelMessage message;
        std::string const id = std::string("w-") + name;
        Check(f.policy.Receive(WatchWire(f, id, name).dump(), f.scope, now, message, "work") == ChannelError::None,
              "watch: each condition admitted");
        auto const* watch = std::get_if<ChannelWatch>(&message.payload);
        Check(watch && watch->commandId == id && watch->botGuid == f.scope.botGuid && watch->condition == condition &&
                  watch->untilUnixMs == 160000,
              "watch: typed payload");
    }
    for (auto const* key : {"commandId", "botGuid", "condition", "untilUnixMs"})
    {
        auto missing = WatchWire(f, std::string("missing-") + key);
        missing.erase(key);
        Check(f.Receive(missing) == ChannelError::Invalid, "watch: every key required");
    }
    for (auto const* key : {"issuerGuid", "intent", "params", "text"})
    {
        auto extra = WatchWire(f, std::string("extra-") + key);
        extra[key] = "x";
        Check(f.Receive(extra) == ChannelError::Invalid, "watch: no other key");
    }
    for (Json const& bad : {Json("combat"), Json(""), Json("Combat_End"), Json("their_answer"), Json(1), Json(nullptr)})
        Check(f.Receive(WatchWire(f, "bad-condition", bad)) == ChannelError::Invalid,
              "watch: the condition one of four names");
    for (Json const& bad : {Json(-1), Json(1.5), Json("160000"), Json(9007199254740992ULL), Json(nullptr), Json(true)})
        Check(f.Receive(WatchWire(f, "bad-until", "time", bad)) == ChannelError::Invalid,
              "watch: until a safe non-negative integer");
    Check(f.Receive(WatchWire(f, "safe-until", "time", Json(9007199254740991ULL))) == ChannelError::None,
          "watch: until up to the largest safe integer");
    for (std::string const& bad :
         {std::string(), std::string("-lead"), std::string("has space"), std::string(129, 'a')})
        Check(f.Receive(WatchWire(f, bad)) == ChannelError::Invalid, "watch: the id an identifier");
    auto other = WatchWire(f, "other-bot");
    other["botGuid"] = "5";
    Check(f.Receive(other) == ChannelError::Invalid, "watch: only the route's own bot");

    Check(f.Receive(WatchWire(f, "w-combat_end")) == ChannelError::Duplicate, "watch: id replay refused");
    Check(f.Receive(f.Command("cmd-1")) == ChannelError::None &&
              f.Receive(WatchWire(f, "cmd-1")) == ChannelError::Duplicate,
          "watch: a command id cannot be reused by a watch");
    Check(f.Receive(WatchWire(f, "w-shared")) == ChannelError::None &&
              f.Receive(f.Command("w-shared")) == ChannelError::Duplicate,
          "watch: a watch id cannot be reused by a command");
    ChannelMessage query;
    Check(f.policy.Receive(f.Query("q-watch").dump(), f.scope, now, query, "work") == ChannelError::None,
          "watch: a query route");
    Check(f.Receive(WatchWire(f, "on-query", "time", 160000, "q-watch")) == ChannelError::Invalid,
          "watch: never on a query route");
    std::string body;

    ChannelEnvelope const envelope{f.scope.instance, f.scope.provider, f.scope.epoch, "work", deadline.unixMs};
    Check(f.policy.FinishWatch(envelope, "w-combat_end", WatchOutcome::Armed, f.scope, now, body) == ChannelError::None,
          "watch: armed result");
    auto json = Json::parse(body);
    Check(json.size() == 9 && json.at("type") == "watch.result" && json.at("requestId") == "work" &&
              json.at("commandId") == "w-combat_end" && json.at("outcome") == "armed",
          "watch: armed result exact keys");
    Check(f.policy.FinishWatch(envelope, "w-combat_end", WatchOutcome::Armed, f.scope, now, body) ==
                  ChannelError::Duplicate &&
              body.empty(),
          "watch: one result per id");
    Check(f.policy.FinishWatch(envelope, "w-none", WatchOutcome::Cleared, f.scope, now, body) == ChannelError::None &&
              Json::parse(body).size() == 9 && Json::parse(body).at("outcome") == "cleared",
          "watch: cleared result exact keys");
    Check(
        f.policy.FinishWatch(envelope, "never-seen", WatchOutcome::Armed, f.scope, now, body) == ChannelError::Unknown,
        "watch: an unknown id has no result");
    struct Words
    {
        CommandRefusal refusal;
        char const* code;
        char const* text;
    };
    Words const words[] = {{CommandRefusal::NotFighting, "not_fighting", "You are not fighting anything."},
                           {CommandRefusal::NotBusy, "not_busy", "You are not busy with anything."},
                           {CommandRefusal::WaitTooShort, "wait_too_short", "That wait is too short to be worth it."},
                           {CommandRefusal::WaitTooLong, "wait_too_long", "That wait is too long."},
                           {CommandRefusal::PutOff, "put_off", "You have already put this off as long as you can."},
                           {CommandRefusal::CannotNow, "cannot_now", nullptr}};
    for (auto const& w : words)
    {
        std::string const id = std::string("r-") + w.code;
        Check(f.Receive(WatchWire(f, id)) == ChannelError::None, "watch: rejected fixture");
        Check(f.policy.FinishWatch(envelope, id, WatchOutcome::Rejected, f.scope, now, body) == ChannelError::Invalid &&
                  body.empty(),
              "watch: rejected needs a refusal");
        Check(f.policy.FinishWatch(envelope, id, WatchOutcome::Armed, f.scope, now, body, w.refusal) ==
                  ChannelError::Invalid,
              "watch: a refusal only with rejected");
        Check(f.policy.FinishWatch(envelope, id, WatchOutcome::Rejected, f.scope, now, body, w.refusal) ==
                  ChannelError::None,
              "watch: rejected result");
        json = Json::parse(body);
        Check(json.size() == 11 && json.at("outcome") == "rejected" && json.at("reason") == w.code &&
                  (w.text ? json.at("reasonText") == w.text : !json.at("reasonText").get<std::string>().empty()),
              "watch: rejected carries reason and in-world reasonText");
    }
    f.policy.Drop("work");
    Check(f.Receive(WatchWire(f, "late")) == ChannelError::Unknown, "watch: none after the route closed");
}

// LC3b: a stepped command, 1..maxSteps acts of the installed catalogue, each with exactly its parameters.
Json StepWire(Fixture const& f, std::string const& id, Json steps)
{
    auto json = f.Command(id);
    json["steps"] = std::move(steps);
    return json;
}

Json Step(char const* act, Json params = Json::object()) { return {{"act", act}, {"params", std::move(params)}}; }

// The steps of one stepped command admitted on a fresh route, as its only reservation.
std::vector<ChannelStep> Decoded(Json steps, char const* label)
{
    Fixture f;
    f.Open();
    ChannelMessage message;
    Check(f.policy.Receive(StepWire(f, "stepped", std::move(steps)).dump(), f.scope, now, message, "work") ==
                  ChannelError::None &&
              f.policy.CommandReservations() == 1,
          label);
    return std::get<ChannelCommand>(message.payload).steps;
}

void Stepped()
{
    Check(SetAbilityIndex(TestStepAbilityIndex()) > 0 && Abilities().rows.size() == 24, "the stepped double installs");
    auto const three =
        Decoded(Json::array({Step("mark", {{"target", "Hogger"}, {"icon", "skull"}}),
                             Step("go_to", {{"place", "Goldshire"}}), Step("meet_at", {{"place", "you"}})}),
                "three steps decode");
    Check(three.size() == 3 && three[0].intent == PlayerbotBridge::Intent::Mark && three[0].targetName == "Hogger" &&
              !three[0].targetGuid && three[0].icon == 7 && three[1].intent == PlayerbotBridge::Intent::GoTo &&
              three[1].place == "Goldshire" && three[2].intent == PlayerbotBridge::Intent::MeetAt &&
              three[2].place == "you",
          "each step's act and parameters, in order; a name and a place as given, a choice as its index");
    auto const ids =
        Decoded(Json::array({Step("focus_healing", {{"targetGuid", "44"}}), Step("quest", {{"questId", 42}}),
                             Step("mark", {{"targetGuid", "45"}, {"icon", "star"}})}),
                "ids decode");
    Check(ids[0].intent == PlayerbotBridge::Intent::FocusHealing && ids[0].targetGuid == 44 &&
              ids[0].targetName.empty() && ids[1].intent == PlayerbotBridge::Intent::Quest && ids[1].questId == 42 &&
              ids[1].questName.empty() && ids[2].targetGuid == 45 && ids[2].icon == 0,
          "a unit and a quest by id");
    auto const named =
        Decoded(Json::array({Step("quest", {{"quest", "The Collector"}}),
                             Step("attack_own_target", {{"target", std::string(ChannelMaxPlaceName, 'a')}})}),
                "names decode");
    Check(named[0].questName == "The Collector" && !named[0].questId &&
              named[1].intent == PlayerbotBridge::Intent::AttackOwnTarget &&
              named[1].targetName.size() == ChannelMaxPlaceName,
          "a quest by title, a unit by a name of the longest length");
    auto const choices = Decoded(
        Json::array({Step("formation", {{"shape", "line"}}), Step("save_mana", {{"enabled", false}}), Step("stay")}),
        "choices and flags decode");
    Check(choices[0].shape == 2 && choices[1].enabled == false && choices[2].intent == PlayerbotBridge::Intent::Stay &&
              !choices[2].icon && !choices[2].shape && !choices[2].enabled,
          "a shape as its choice's index, a flag as given, an act without parameters");
    {
        Fixture f;
        f.Open();
        auto const wire = StepWire(f, "replayed", Json::array({Step("stay"), Step("stay")}));
        Check(f.Receive(wire) == ChannelError::None && f.Receive(wire) == ChannelError::Duplicate &&
                  f.policy.CommandReservations() == 1,
              "one reservation per stepped order; a replay is a duplicate");
    }
    std::string const longName(ChannelMaxPlaceName + 1, 'a');
    struct Bad
    {
        Json steps;
        char const* label;
    };
    Bad const bad[] = {
        {Json::object(), "steps are an array"},
        {Json::array(), "no step"},
        {Json::array({Step("stay"), Step("stay"), Step("stay"), Step("stay")}), "more steps than the catalogue's most"},
        {Json::array({Step("chase")}), "an unknown act"},
        {Json::array({Step("inventory")}), "a topic is no act"},
        {Json::array({Step("regroup")}), "a verb without a row is no act"},
        {Json::array({Step("Stay")}), "an act is its exact word"},
        {Json::array({Json{{"act", 3}, {"params", Json::object()}}}), "an act is a word"},
        {Json::array({Json{{"act", "stay"}}}), "a step without params"},
        {Json::array({Json{{"act", "stay"}, {"params", Json::object()}, {"why", 1}}}), "a step key the wire lacks"},
        {Json::array({Json{{"act", "stay"}, {"params", Json::array()}}}), "params are an object"},
        {Json::array({Step("stay", {{"target", "Hogger"}})}), "a parameter the act does not take"},
        {Json::array({Step("go_to", {{"place", "Goldshire"}, {"icon", "star"}})}), "an extra parameter"},
        {Json::array({Step("mark", {{"target", "Hogger"}, {"targetGuid", "44"}, {"icon", "star"}})}),
         "a unit by name and by id"},
        {Json::array({Step("mark", {{"icon", "star"}})}), "a unit act without its unit"},
        {Json::array({Step("mark", {{"target", "Hogger"}})}), "a missing choice"},
        {Json::array({Step("mark", {{"targetGuid", "abc"}, {"icon", "star"}})}), "a bad guid"},
        {Json::array({Step("mark", {{"target", "Hogger"}, {"icon", 3}})}), "an icon is a choice word, not a number"},
        {Json::array({Step("mark", {{"target", "Hogger"}, {"icon", "Skull"}})}), "a choice is its exact word"},
        {Json::array({Step("formation", {{"shape", "circle"}})}), "a shape not among the choices"},
        {Json::array({Step("quest", {{"quest", "The Collector"}, {"questId", 42}})}), "a quest by title and by id"},
        {Json::array({Step("quest")}), "a quest act without its quest"},
        {Json::array({Step("quest", {{"questId", 0}})}), "quest id zero"},
        {Json::array({Step("quest", {{"questId", 4294967296LL}})}), "a quest id past 32 bits"},
        {Json::array({Step("quest", {{"questId", "42"}})}), "a quest id is a number"},
        {Json::array({Step("focus_healing", {{"target", longName}})}), "a name over 64 bytes"},
        {Json::array({Step("focus_healing", {{"target", ""}})}), "an empty name"},
        {Json::array({Step("focus_healing", {{"target", "Hog\tger"}})}), "a control character in a name"},
        {Json::array({Step("quest", {{"quest", " "}})}), "a blank title"},
        {Json::array({Step("go_to", {{"place", 7}})}), "a place is text"},
        {Json::array({Step("save_mana", {{"enabled", "yes"}})}), "a flag is a boolean"},
        {Json::array({Step("save_mana", {{"enabled", 1}})}), "a flag is no number"},
        {Json::array({Step("position", {{"place", "1,2,3,0"}})}), "position is no act (LC17e)"},
        {Json::array({Step("stay"), Step("chase")}), "one bad step refuses the order"},
    };
    for (std::size_t at = 0; at < std::size(bad); ++at)
    {
        Fixture f;
        f.Open();
        Check(f.Receive(StepWire(f, "bad-" + std::to_string(at), bad[at].steps)) == ChannelError::Invalid &&
                  f.policy.CommandReservations() == 0,
              bad[at].label);
    }
    {
        Fixture f;
        f.Open();
        auto both = StepWire(f, "both", Json::array({Step("stay")}));
        both["intent"] = "stay";
        auto withParams = StepWire(f, "with-params", Json::array({Step("stay")}));
        withParams["params"] = Json::object();
        auto noBot = StepWire(f, "no-bot", Json::array({Step("stay")}));
        noBot.erase("botGuid");
        auto oldNamed = f.Command("old-named");
        oldNamed.erase("steps");
        oldNamed["intent"] = "mark";
        oldNamed["params"] = {{"target", "Hogger"}, {"icon", 3}};
        Check(f.Receive(both) == ChannelError::Invalid && f.Receive(withParams) == ChannelError::Invalid &&
                  f.Receive(noBot) == ChannelError::Invalid && f.Receive(oldNamed) == ChannelError::Invalid &&
                  f.policy.CommandReservations() == 0,
              "steps beside intent or params, without the scope's keys, or a body of the old form: invalid");
    }
    auto two = TestStepAbilityIndex();
    two.maxSteps = 2;
    Check(SetAbilityIndex(std::move(two)) > 0, "a two-step double installs");
    {
        Fixture f;
        f.Open();
        Check(f.Receive(StepWire(f, "two", Json::array({Step("stay"), Step("stay")}))) == ChannelError::None &&
                  f.Receive(StepWire(f, "three", Json::array({Step("stay"), Step("stay"), Step("stay")}))) ==
                      ChannelError::Invalid,
              "the most steps are the installed catalogue's");
    }
    Check(InstallTestAbilities(), "the plain double installs again");
    {
        Fixture f;
        f.Open();
        Check(f.Receive(StepWire(f, "rowless", Json::array({Step("stay")}))) == ChannelError::Invalid &&
                  f.Receive(StepWire(f, "rowed", Json::array({Step("follow")}))) == ChannelError::None,
              "an act is a row of the installed catalogue");
    }
}

// LC3b: a stepped command's result keeps the old shape and adds one entry per submitted step.
struct Finished
{
    ChannelError error = ChannelError::None;
    Json json;
};

// Admits the command on a fresh route and finishes it once, then checks a second finish is refused.
Finished Finish(Json const& steps, ChannelOutcome outcome, std::vector<ChannelStepResult> const& ran,
                CommandRefusal refusal)
{
    Fixture f;
    f.Open();
    ChannelMessage message;
    auto const wire = StepWire(f, "done", steps);
    Check(f.policy.Receive(wire.dump(), f.scope, now, message, "work") == ChannelError::None, "the command admitted");
    auto const& command = std::get<ChannelCommand>(message.payload);
    std::string body;
    auto const error =
        f.policy.FinishCommand(message.envelope, "done", outcome, f.scope, now, body, refusal, command.steps, ran);
    Check((error == ChannelError::None) != body.empty(), "a body exactly when answered");
    Finished result{error, body.empty() ? Json() : Json::parse(body)};
    if (error == ChannelError::None)
        Check(f.policy.FinishCommand(message.envelope, "done", outcome, f.scope, now, body, refusal, command.steps,
                                     ran) == ChannelError::Duplicate &&
                  body.empty(),
              "a stepped result is answered once");
    return result;
}

void SteppedResults()
{
    Check(SetAbilityIndex(TestStepAbilityIndex()) > 0, "the stepped double installs");
    ChannelStepResult const ok{ChannelOutcome::Applied, "", ""};
    ChannelStepResult const place{ChannelOutcome::Rejected, "unknown_place", "You do not know that place."};
    Json const three = Json::array({Step("mark", {{"target", "Hogger"}, {"icon", "skull"}}),
                                    Step("go_to", {{"place", "Goldshire"}}), Step("stay")});
    auto const applied = Finish(three, ChannelOutcome::Applied, {ok, ok, ok}, CommandRefusal::None);
    Check(applied.error == ChannelError::None && applied.json.at("outcome") == "applied" &&
              !applied.json.contains("reason") && applied.json.size() == 10 &&
              applied.json.at("steps") == Json::parse(R"([{"act":"mark","outcome":"applied"},
                  {"act":"go_to","outcome":"applied"},{"act":"stay","outcome":"applied"}])"),
          "every step applied: the order applied, each step listed in order");
    auto const second = Finish(three, ChannelOutcome::Rejected, {ok, place}, CommandRefusal::CannotNow);
    Check(second.error == ChannelError::None && second.json.at("outcome") == "rejected" &&
              second.json.at("reason") == "unknown_place" &&
              second.json.at("reasonText") == "You do not know that place." &&
              second.json.at("steps") == Json::parse(R"([{"act":"mark","outcome":"applied"},
                  {"act":"go_to","outcome":"rejected","reason":"unknown_place",
                   "reasonText":"You do not know that place."},{"act":"stay","outcome":"skipped"}])"),
          "a refused step ends the order with its words; the steps after it are skipped");
    auto const failedWords = DescribeRefusal(CommandRefusal::Failed);
    auto const failed = Finish(three, ChannelOutcome::Failed, {{ChannelOutcome::Failed, "", ""}}, CommandRefusal::None);
    Check(failed.json.at("outcome") == "failed" && failed.json.at("reason") == failedWords.code &&
              failed.json.at("steps").at(0) == Json{{"act", "mark"},
                                                    {"outcome", "failed"},
                                                    {"reason", failedWords.code},
                                                    {"reasonText", failedWords.text}} &&
              failed.json.at("steps").at(1) == Json{{"act", "go_to"}, {"outcome", "skipped"}},
          "an unworded failure reads failed");
    auto const cannot = DescribeRefusal(CommandRefusal::CannotNow);
    auto const unworded =
        Finish(three, ChannelOutcome::Rejected, {ok, {ChannelOutcome::Rejected, "", ""}}, CommandRefusal::None);
    Check(unworded.json.at("reason") == cannot.code && unworded.json.at("steps").at(1).at("reason") == cannot.code &&
              unworded.json.at("steps").at(1).at("reasonText") == cannot.text,
          "an unworded refusal reads cannot_now");
    auto const early = Finish(three, ChannelOutcome::Rejected, {}, CommandRefusal::TooSoon);
    Check(early.json.at("outcome") == "rejected" && early.json.at("reason") == "too_soon" &&
              early.json.at("steps") == Json::parse(R"([{"act":"mark","outcome":"skipped"},
                  {"act":"go_to","outcome":"skipped"},{"act":"stay","outcome":"skipped"}])"),
          "an order refused before any step ran: every step skipped, the order's own reason");
    struct Guard
    {
        ChannelOutcome outcome;
        std::vector<ChannelStepResult> ran;
        char const* label;
    };
    Guard const guards[] = {
        {ChannelOutcome::Applied, {ok, ok}, "applied with a step that never ran"},
        {ChannelOutcome::Applied, {ok, ok, ok, ok}, "more steps ran than were sent"},
        {ChannelOutcome::Rejected, {place, ok}, "a step ran after a refused one"},
        {ChannelOutcome::Rejected, {ok, ok, ok}, "refused though every step applied"},
        {ChannelOutcome::Applied, {ok, place}, "applied though a step was refused"},
    };
    for (Guard const& guard : guards)
    {
        auto const refused = Finish(three, guard.outcome, guard.ran, CommandRefusal::None);
        Check(refused.error == ChannelError::Invalid && refused.json.is_null(), guard.label);
    }
    Check(InstallTestAbilities(), "the plain double installs again");
}

// ---- LC16c: a row the module has never heard of ---------------------------------------------------------------------
void OpenRows()
{
    // Malformed rows first: an open act or topic keeps every row rule.
    using Change = void (*)(AbilityIndex&);
    Change const refused[] = {
        [](AbilityIndex& index) { index.rows[3].name = "mark"; },
        [](AbilityIndex& index) { index.rows[3].code = index.rows[1].code; },
        [](AbilityIndex& index) { index.rows[3].params[3].field = static_cast<AbilityField>(11); },
        [](AbilityIndex& index)
        {
            auto twin = index.rows[3];
            twin.name = "cast_again";
            index.rows.insert(index.rows.begin() + 4, std::move(twin));
        },
        [](AbilityIndex& index) { index.rows.back().code = index.rows[4].code; },
        [](AbilityIndex& index) { index.rows.back().params.push_back(index.rows.back().params.front()); },
        [](AbilityIndex& index) { index.rows.back().name = "deeds of"; },
    };
    for (Change change : refused)
    {
        auto index = TestOpenAbilityIndex();
        change(index);
        Check(SetAbilityIndex(std::move(index)) == 0 && Abilities().rows.empty(),
              "an open row named or coded like another act, with an unknown field, a twin code, a duplicate topic, "
              "two topic parameters or a bad word refuses the index");
    }
    auto high = TestOpenAbilityIndex();
    high.rows[3].code = 255;
    high.rows.back().code = 200;
    Check(SetAbilityIndex(std::move(high)) > 0, "any act or topic value");
    Check(SetAbilityIndex(TestOpenAbilityIndex()) > 0 && Abilities().rows.size() == 19 && CatalogueInstalled(),
          "the open double installs: an act and a topic the module has no code for");

    // A stepped order: the act's value from the index, the four new fields by kind.
    Json const cast =
        Step("cast_on", {{"item", "Hearthstone"}, {"spell", "Frost Nova"}, {"times", 1000}, {"manner", "loudly"}});
    auto const open =
        Decoded(Json::array({cast, Step("mark", {{"target", "Hogger"}, {"icon", "skull"}})}), "an open act decodes");
    Check(open.size() == 2 && static_cast<uint8_t>(open[0].intent) == TestOpenAct &&
              open[0].itemName == "Hearthstone" && open[0].spellName == "Frost Nova" && open[0].count == 1000 &&
              open[0].choice == 1 && !open[0].icon && !open[0].shape && !open[0].enabled && open[0].place.empty() &&
              !open[0].targetGuid && open[0].targetName.empty() && open[1].intent == PlayerbotBridge::Intent::Mark &&
              open[1].icon == 7 && !open[1].choice && !open[1].count && open[1].itemName.empty(),
          "an item and a spell as named, a count as given, a choice as its index; the act beside it unchanged");
    auto const least = Decoded(Json::array({Step("cast_on", {{"item", std::string(ChannelMaxPlaceName, 'i')},
                                                             {"spell", "Blink"},
                                                             {"times", 1},
                                                             {"manner", "quietly"}})}),
                               "the least count and the longest name decode");
    Check(least[0].count == 1 && least[0].choice == 0 && least[0].itemName.size() == ChannelMaxPlaceName,
          "a count of one, the first choice, a name of 64 bytes");
    auto const with = [&cast](char const* key, Json value)
    {
        auto step = cast;
        step["params"][key] = std::move(value);
        return Json::array({step});
    };
    auto const without = [&cast](char const* key)
    {
        auto step = cast;
        step["params"].erase(key);
        return Json::array({step});
    };
    std::string const longName(ChannelMaxPlaceName + 1, 'a');
    struct Bad
    {
        Json steps;
        char const* label;
    };
    Bad const bad[] = {
        {without("item"), "an open act without its item"},
        {without("spell"), "without its spell"},
        {without("times"), "without its count"},
        {without("manner"), "without its choice"},
        {with("item", 7), "an item is text"},
        {with("item", ""), "an empty item"},
        {with("item", longName), "an item name over 64 bytes"},
        {with("item", "Hearth\nstone"), "a control character in an item"},
        {with("spell", " "), "a blank spell"},
        {with("spell", Json::array({"Blink"})), "a spell is text"},
        {with("times", 0), "a count of zero"},
        {with("times", ChannelMaxCount + 1), "a count past 1000"},
        {with("times", -1), "a negative count"},
        {with("times", 2.5), "a fractional count"},
        {with("times", "3"), "a count is a number"},
        {with("manner", "Loudly"), "a choice is its exact word"},
        {with("manner", 1), "a choice is a word, not its index"},
        {with("manner", "softly"), "a choice not among the act's"},
        {with("itemId", 6948), "an item takes no id"},
        {with("spellGuid", "44"), "a spell takes no guid"},
        {Json::array({Step("cast_on")}), "an open act without parameters"},
        {Json::array({Step("deeds_of", {{"name", "Hogger"}})}), "an open topic is no act"},
    };
    for (std::size_t at = 0; at < std::size(bad); ++at)
    {
        Fixture f;
        f.Open();
        Check(f.Receive(StepWire(f, "open-bad-" + std::to_string(at), bad[at].steps)) == ChannelError::Invalid &&
                  f.policy.CommandReservations() == 0,
              bad[at].label);
    }
    {
        Fixture f;
        f.Open();
        auto old = f.Command("old-open");
        old.erase("steps");
        old["intent"] = "cast_on";
        old["params"] = cast.at("params");
        auto bare = f.Command("old-open-bare");
        bare.erase("steps");
        bare["intent"] = "cast_on";
        bare["params"] = Json::object();
        auto topic = f.Command("old-open-topic");
        topic.erase("steps");
        topic["intent"] = "deeds_of";
        topic["params"] = Json::object();
        Check(f.Receive(old) == ChannelError::Invalid && f.Receive(bare) == ChannelError::Invalid &&
                  f.Receive(topic) == ChannelError::Invalid && f.policy.CommandReservations() == 0,
              "LCB2: a body of the old form is invalid for an open act or topic too");
        Check(f.Receive(StepWire(f, "open-once", Json::array({cast}))) == ChannelError::None &&
                  f.policy.CommandReservations() == 1,
              "one reservation for an open act's order");
    }
    auto const refusedStep =
        Finish(Json::array({cast, Step("mark", {{"target", "Hogger"}, {"icon", "skull"}})}), ChannelOutcome::Rejected,
               {{ChannelOutcome::Rejected, "no_such_item", "I carry nothing like that."}}, CommandRefusal::CannotNow);
    Check(refusedStep.error == ChannelError::None && refusedStep.json.at("reason") == "no_such_item" &&
              refusedStep.json.at("steps") == Json::parse(R"([{"act":"cast_on","outcome":"rejected",
                  "reason":"no_such_item","reasonText":"I carry nothing like that."},{"act":"mark","outcome":"skipped"}])"),
          "an open act's step is named by its row and refused in playerbots' words");

    // A topic past the fourteen: its argument under the row's parameter name, its entries in the lines form.
    Check(RowLimit(static_cast<StateTopic>(TestOpenTopic)) == 6 && !RowLimit(static_cast<StateTopic>(TestOpenTopic + 1)),
          "an open topic's limit is its row's; a topic the index lacks has none");
    {
        Fixture f;
        f.Open();
        auto const asking = [&f](char const* id, char const* key, Json value)
        {
            auto json = StateQuery(f, id, "deeds_of");
            if (key)
                json[key] = std::move(value);
            return json;
        };
        auto both = asking("deeds-both", "name", "Hogger");
        both["place"] = "Goldshire";
        auto routeNamed = StateQuery(f, "route-named", "route");
        routeNamed["name"] = "Goldshire";
        Check(f.Receive(asking("deeds-bare", nullptr, {})) == ChannelError::Invalid &&
                  f.Receive(asking("deeds-place", "place", "Hogger")) == ChannelError::Invalid &&
                  f.Receive(both) == ChannelError::Invalid &&
                  f.Receive(asking("deeds-blank", "name", " ")) == ChannelError::Invalid &&
                  f.Receive(asking("deeds-number", "name", 44)) == ChannelError::Invalid &&
                  f.Receive(asking("deeds-control", "name", "Hog\tger")) == ChannelError::Invalid &&
                  f.Receive(asking("deeds-long", "name", longName)) == ChannelError::Invalid &&
                  f.Receive(routeNamed) == ChannelError::Invalid,
              "an open topic takes exactly its argument, under its own name and Place's rules");
        ChannelMessage message;
        Check(f.policy.Receive(asking("deeds", "name", "Hogger").dump(), f.scope, now, message, "work") ==
                  ChannelError::None,
              "an open topic's query admitted");
        auto const& query = std::get<ChannelQuery>(message.payload);
        Check(query.kind == QueryKind::BotState && static_cast<uint8_t>(query.topic) == TestOpenTopic &&
                  query.place == "Hogger",
              "the open topic and its argument");
        std::string const longText(ChannelMaxTopicText + 40, 't');
        std::vector<StateEntry> entries = {
            {"Hogger", "deeds_of", 3, 4, "Hogger slew a wolf."},
            {"Hogger", "deed", -5, 5000000000LL, longText},
            {"Hogger", "Bad Word", 1, 1, "Dropped."},
            {"Hogger", "deed", 1, 1, ""},
            {"", "deed", 1, 1, "No name."},
        };
        for (int line = 0; line < 9; ++line)
            entries.push_back({"Hogger", "deed", line, 0, "Line " + std::to_string(line)});
        entries[5].side = "alliance";  // LC18g: Line 0 carries a place's side, Line 1 a word that is none
        entries[6].side = "Bogus";
        std::string body;
        Check(f.policy.FinishTopic(message.envelope, f.scope, query.topic, entries, now, body) == ChannelError::None,
              "an open topic answered");
        auto const json = Json::parse(body);
        auto const& list = json.at("entries");
        Check(json.at("topic") == "deeds_of" && list.size() == 6 &&
                  list.at(0) == Json{{"row", "deeds_of"},
                                     {"name", "Hogger"},
                                     {"text", "Hogger slew a wolf."},
                                     {"value", 3},
                                     {"extra", 4}} &&
                  list.at(2).at("text") == "Line 0" && list.at(5).at("text") == "Line 3",
              "the lines form, unusable entries dropped, cut at the row's limit");
        Check(list.at(2).at("side") == "alliance" && !list.at(3).contains("side") && !list.at(0).contains("side"),
              "LC18g: a place's side passes when it is one of the three words; none or another word sends no key");
        Check(list.at(1).at("row") == "deed" &&
                  list.at(1).at("text").get<std::string>().size() == ChannelMaxTopicText &&
                  list.at(1).at("value") == 0 && list.at(1).at("extra") == UINT32_MAX,
              "any wire word as the row; the text cut to 200 bytes, value and extra clamped");
        ChannelMessage other;
        Check(f.policy.Receive(asking("deeds-other", "name", "Hogger").dump(), f.scope, now, other, "work") ==
                      ChannelError::None &&
                  f.policy.FinishTopic(other.envelope, f.scope, static_cast<StateTopic>(TestOpenTopic + 1), entries,
                                       now, body) == ChannelError::Invalid,
              "a topic the index lacks is never answered");
    }

    // The catalogue answer: past the old 32 KB reply, within the catalogue's own bound.
    {
        auto big = TestOpenAbilityIndex();
        big.rows[0].sentence = std::string(40000, 'x');
        auto const size = SetAbilityIndex(std::move(big));
        Fixture f;
        f.Open();
        ChannelMessage message;
        std::string body;
        Check(size > MaxResponseBytes && size <= ChannelMaxCatalogueBytes &&
                  f.policy.Receive(CatalogueQuery(f, "cat-big").dump(), f.scope, now, message, "work") ==
                      ChannelError::None &&
                  f.policy.FinishCatalogue(message.envelope, f.scope, now, body) == ChannelError::None &&
                  body.size() > MaxResponseBytes && body.size() <= MaxCatalogueResponseBytes,
              "a catalogue past the old 32 KB reply is answered whole");
    }
    Check(InstallTestAbilities(), "the plain double installs again");
}

int main()
{
    if (!InstallTestAbilities())  // LC3: the acts and topics are the installed index's.
    {
        std::cerr << "FAIL: the ability double was refused\n";
        return 1;
    }
    try
    {
        IdentityAndTerminal();
        ExpiryAndScope();
        Schemas();
        QueriesAndCapacity();
        Topics();
        Watches();
        Catalogue();
        Stepped();
        SteppedResults();
        OpenRows();
        std::cout << "PASS " << checks << " production channel checks\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL after " << checks << ": " << error.what() << '\n';
        return 1;
    }
}
