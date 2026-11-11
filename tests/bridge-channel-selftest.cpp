// Offline checks of the actual production channel helper; no game authority or effect executor.
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

#include "mod-ollama-chat_bridge_channel.h"

namespace
{
using namespace OllamaBridge;
using Json = nlohmann::json;
unsigned checks = 0;
Clock const now{100000, 200000};
std::string const bearer = "synthetic-local-game-link-bearer";
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
        1,       "fixture",       "codex",     "epoch-1",   9007199254740994ULL, 9007199254740993ULL, "conversation",
        "party", "visible-party", "context-1", "members-1", "persona-1"};
    explicit Fixture(std::size_t ids = ChannelMaxIds, std::size_t pending = ChannelMaxPending) : policy(ids, pending)
    {
        Check(policy.Bind({scope.instance, scope.provider, "", scope.epoch}, bearer), "trusted selected epoch binding");
        Check(policy.ObserveScope(scope, now), "trusted scope registration");
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
        json["intent"] = "stay";
        json["params"] = Json::object();
        return json;
    }
    ChannelError Receive(Json const& json, Clock clock = now)
    {
        ChannelMessage result;
        std::string const route = json.at("requestId").is_string() ? json.at("requestId").get<std::string>() : "work";
        return policy.Receive(bearer, json.dump(), scope, clock, result, route);
    }
};

void IdentityAndTerminal()
{
    Fixture f;
    f.Open();
    ChannelMessage result;
    Check(f.policy.Receive("wrong", "{", f.scope, now, result, "work") == ChannelError::Unauthorized,
          "auth precedes parse");
    Check(f.policy.Receive("wrong", std::string(MaxRequestBytes + 1, 'x'), f.scope, now, result, "work") ==
              ChannelError::Unauthorized,
          "auth precedes body size error");
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
    auto terminal = f.Wire("generation.result");
    terminal["text"] = "I shall wait here.";
    Check(f.Receive(terminal) == ChannelError::None, "one terminal result");
    Check(f.Receive(terminal) == ChannelError::Closed, "duplicate terminal blocked");
    Check(f.Receive(f.Command("late")) == ChannelError::Closed, "command cannot follow terminal");
    auto failure = f.Wire("failure");
    failure["code"] = "unavailable";
    Check(f.Receive(failure) == ChannelError::Closed, "failure cannot replace terminal");
    std::string body;
    ChannelEnvelope envelope{"fixture", "codex", "epoch-1", "work", deadline.unixMs};
    Check(f.policy.FinishCommand(envelope, "effect", ChannelOutcome::Failed, f.scope, now, body) == ChannelError::None,
          "failed staged proposal reports once even after parent terminal");
    Check(Json::parse(body).at("outcome") == "failed", "typed fixed outcome");
    Check(f.policy.FinishCommand(envelope, "effect", ChannelOutcome::Applied, f.scope, now, body) ==
              ChannelError::Duplicate,
          "failed command id never retries");
    Check(body.empty(), "rejected result has no stale output");
    auto nativeResult = f.Wire("command.result");
    nativeResult["commandId"] = "effect";
    nativeResult["outcome"] = "applied";
    Check(f.Receive(nativeResult) == ChannelError::Invalid, "native cannot supply game effect results");

    Fixture failed;
    failed.Open();
    failure = failed.Wire("failure");
    failure["code"] = "timeout";
    Check(failed.Receive(failure) == ChannelError::None, "failure terminal accepted");
    Check(failed.Receive(failed.Command()) == ChannelError::Closed, "failure cannot authorize effects");
    Check(failed.policy.Bind({"fixture", "codex", "", "epoch-2"}, bearer + "-new"), "reconnect uses new epoch");
    Check(!failed.policy.ObserveScope(failed.scope, now), "reconnect rejects old scope");
    Check(failed.Receive(failure) == ChannelError::Unauthorized, "old bearer retired");
    failed.scope.epoch = "epoch-2";
    Check(failed.policy.ObserveScope(failed.scope, now), "fresh epoch scope");
    Check(failed.policy.Receive(bearer + "-new", failure.dump(), failed.scope, now, result, "work") ==
              ChannelError::Stale,
          "old terminal cannot cross reconnect");
    failure["epoch"] = "epoch-2";
    Check(failed.policy.Receive(bearer + "-new", failure.dump(), failed.scope, now, result, "work") ==
              ChannelError::Unknown,
          "reconnect cannot reopen old work");
    Check(!failed.policy.Bind({"fixture", "codex", "", "epoch-1"}, bearer), "retired epoch reuse fails closed");
    Check(!failed.policy.ObserveScope(failed.scope, now), "bad activation leaves inactive");
}

void ExpiryAndScope()
{
    for (Clock clock : {Clock{101000, 200999}, Clock{100999, 201000}, Clock{90000, 201000}})
    {
        Fixture f;
        f.Open();
        Check(f.Receive(f.Command(), clock) == ChannelError::Stale,
              "expiry equality on either clock including rollback");
        Check(f.policy.RequestReservations() == 1 && f.policy.Pending() == 0, "expiry retains replay id");
        Check(f.policy.OpenGeneration(f.scope, "work", deadline, now) == ChannelError::Duplicate,
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
    f.policy.RemoveScope(f.scope.botGuid, f.scope.contextId);
    Check(f.Receive(f.Command("removed")) == ChannelError::Stale, "removed context rejected");
    Check(f.policy.ObserveScope(f.scope, now), "same scope can be observed again");
    Check(f.Receive(f.Command("revive")) == ChannelError::Stale, "reobservation cannot revive old request");
    for (auto const* field : {"controller", "context", "membership", "persona", "perception", "channel"})
    {
        Fixture revision;
        revision.Open();
        if (std::string(field) == "controller")
            revision.scope.controllerGuid += 1;
        if (std::string(field) == "context")
            revision.scope.contextRevision += "-new";
        if (std::string(field) == "membership")
            revision.scope.membershipRevision += "-new";
        if (std::string(field) == "persona")
            revision.scope.personaRevision += "-new";
        if (std::string(field) == "perception")
            revision.scope.perceptionScope += "-new";
        if (std::string(field) == "channel")
            revision.scope.channel = "direct";
        Check(revision.policy.ObserveScope(revision.scope, now), "trusted scope revision");
        Check(revision.Receive(revision.Command()) == ChannelError::Stale, "revision invalidates old work");
    }
    Fixture lapsed;
    Deadline const longDeadline{110000, 210000};
    Check(lapsed.policy.OpenGeneration(lapsed.scope, "work", longDeadline, now) == ChannelError::None, "long work");
    Check(lapsed.policy.ObserveScope(lapsed.scope, {105000, 205000}), "refresh after scope expiry");
    auto late = lapsed.Command();
    late["deadlineUnixMs"] = longDeadline.unixMs;
    Check(lapsed.Receive(late, {105000, 205000}) == ChannelError::Stale, "scope expiry cannot revive by refresh");
    Check(lapsed.policy.OpenGeneration(lapsed.scope, "too-long", {225001, 325001}, {105000, 205000}) ==
              ChannelError::Invalid,
          "120 second maximum");
}

void Schemas()
{
    Fixture f;
    f.Open();
    ChannelMessage result;
    Check(f.policy.Receive(bearer, "{", f.scope, now, result, "work") == ChannelError::Invalid, "malformed JSON");
    Check(f.policy.Receive(bearer, std::string(MaxRequestBytes + 1, ' '), f.scope, now, result, "work") ==
              ChannelError::TooLarge,
          "bounded body before parse");
    Check(f.policy.Receive(bearer, std::string(MaxRequestBytes, ' '), f.scope, now, result, "work") ==
              ChannelError::Invalid,
          "exact body maximum reaches parser");
    auto duplicate = f.Command().dump();
    duplicate.insert(1, "\"version\":1,");
    Check(f.policy.Receive(bearer, duplicate, f.scope, now, result, "work") == ChannelError::Invalid,
          "duplicate key before DOM overwrite");
    duplicate = f.Command().dump();
    auto const position = duplicate.find("\"params\":{}");
    duplicate.replace(position, 11, "\"params\":{\"x\":1,\"x\":2}");
    Check(f.policy.Receive(bearer, duplicate, f.scope, now, result, "work") == ChannelError::Invalid,
          "nested duplicate key");
    Check(f.policy.Receive(bearer, std::string(20, '[') + "0" + std::string(20, ']'), f.scope, now, result, "work") ==
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
                                                                      {"intent", "stay;delete"},
                                                                      {"params", Json::array()},
                                                                      {"extra", "private"}})
    {
        auto bad = f.Command();
        bad[pair.first] = pair.second;
        Check(f.Receive(bad) == ChannelError::Invalid, "strict exact envelope or payload field");
    }
    for (auto const& pair :
         std::vector<std::pair<std::string, Json>>{{"pull", {{"targetGuid", "01"}}},
                                                   {"mark", {{"targetGuid", "1"}, {"icon", 8}}},
                                                   {"mark", {{"targetGuid", "1"}, {"icon", 1.0}}},
                                                   {"assign_role", {{"role", "leader"}}},
                                                   {"save_mana", {{"enabled", 1}}},
                                                   {"formation", {{"shape", "circle"}}},
                                                   {"dungeon_clear", {{"mode", "restart"}}},
                                                   {"go_to", {{"place", ""}}},
                                                   {"meet_at", {{"questId", 1}}},
                                                   {"quest", {{"questId", 0}}},
                                                   {"quest", {{"place", "Goldshire"}}},
                                                   {"errands", {{"place", "Goldshire"}}},
                                                   {"position", {{"x", 1}, {"y", 2}, {"z", 3}, {"mapId", -1}}},
                                                   {"position", {{"x", "1"}, {"y", 2}, {"z", 3}, {"mapId", 0}}}})
    {
        auto bad = f.Command();
        bad["intent"] = pair.first;
        bad["params"] = pair.second;
        Check(f.Receive(bad) == ChannelError::Invalid, "strict intent parameters");
    }
    // J5e: follow or stay with any keyed params is a standing shape: still 400 invalid_input, but with its reason.
    for (auto const& pair : std::vector<std::pair<std::string, Json>>{
             {"stay", {{"targetGuid", "1"}}}, {"follow", {{"standing", true}}}, {"stay", {{"durationMs", 30000}}}})
    {
        auto bad = f.Command();
        bad["intent"] = pair.first;
        bad["params"] = pair.second;
        Check(f.Receive(bad) == ChannelError::Standing, "J5e: follow or stay with params is a standing shape");
    }
    Check(f.policy.CommandReservations() == 0, "malformed payloads reserve no effect ids");
    unsigned sequence = 0;
    for (auto const& pair :
         std::vector<std::pair<std::string, Json>>{{"follow", Json::object()},
                                                   {"stay", Json::object()},
                                                   {"flee", Json::object()},
                                                   {"regroup", Json::object()},
                                                   {"attack_own_target", Json::object()},
                                                   {"pull", {{"targetGuid", "18446744073709551615"}}},
                                                   {"mark", {{"targetGuid", "1"}, {"icon", 7}}},
                                                   {"focus_healing", {{"targetGuid", "2"}}},
                                                   {"assign_role", {{"role", "healer"}}},
                                                   {"save_mana", {{"enabled", false}}},
                                                   {"formation", {{"shape", "spread"}}},
                                                   {"position", {{"x", 1.5}, {"y", -2}, {"z", 3}, {"mapId", 0}}},
                                                   {"dungeon_clear", {{"mode", "pause"}}},
                                                   {"go_to", {{"place", "Goldshire"}}},
                                                   {"quest", {{"questId", 4294967295ULL}}},
                                                   {"errands", Json::object()},
                                                   {"set_hearth", Json::object()},
                                                   {"grind_here", Json::object()},
                                                   {"gather_here", Json::object()},
                                                   {"meet_at", {{"place", std::string(64, 'm')}}},
                                                   {"pause_plan", Json::object()},
                                                   {"resume_plan", Json::object()}})
    {
        auto good = f.Command("effect-" + std::to_string(++sequence));
        good["intent"] = pair.first;
        good["params"] = pair.second;
        Check(f.policy.Receive(bearer, good.dump(), f.scope, now, result, "work") == ChannelError::None,
              "whitelist decodes into typed values");
        Check(static_cast<unsigned>(std::get<ChannelCommand>(result.payload).intent) == sequence - 1,
              "typed intent preserved");
    }
    auto terminal = f.Wire("generation.result");
    for (std::string text : {std::string(4097, 'x'), std::string("\0", 1), std::string("  \n")})
    {
        terminal["text"] = text;
        Check(f.Receive(terminal) == ChannelError::Invalid, "bounded usable text");
    }
    terminal["text"] = std::string(4096, 'x');
    Check(f.Receive(terminal) == ChannelError::None, "exact text byte maximum");
}

void QueriesEventsAndCapacity()
{
    Fixture f;
    ChannelMessage query;
    Check(f.policy.Receive(bearer, f.Query().dump(), f.scope, now, query, "work") == ChannelError::None,
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
    Check(f.policy.Receive(bearer, f.Query("q2").dump(), f.scope, now, query, "work") == ChannelError::None,
          "second query");
    units.front().healthPercent = 101;
    Check(f.policy.FinishQuery(query.envelope, f.scope, units, now, body) == ChannelError::Invalid, "invalid UI field");
    units.front().healthPercent = 80;
    units.front().role = "healer";
    units.front().visibleTargetGuid = 18446744073709551615ULL;
    Check(f.policy.FinishQuery(query.envelope, f.scope, units, now, body) == ChannelError::None,
          "observed optional fields retained");
    parsed = Json::parse(body);
    Check(parsed.at("units").at(0).size() == 5, "exact declared unit fields");
    Check(f.policy.Receive(bearer, f.Query("q3").dump(), f.scope, now, query, "work") == ChannelError::None,
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
    Check(f.policy.OpenEvents(f.scope, "events", deadline, std::vector<ChannelObservedMark>(32, {1, 7}), now, body) ==
              ChannelError::None,
          "maximum event batch");
    parsed = Json::parse(body);
    Check(parsed.at("events").size() == 32 && parsed.at("events").at(0).size() == 3, "only observed mark schema");
    Check(f.Receive(f.Wire("events.result", "events")) == ChannelError::None, "event acknowledgement terminal");
    Check(f.Receive(f.Wire("events.result", "events")) == ChannelError::Closed, "event terminal replay rejected");
    Check(f.policy.OpenEvents(f.scope, "overevents", deadline, std::vector<ChannelObservedMark>(33, {1, 0}), now,
                              body) == ChannelError::Invalid,
          "oversize event batch");
    Check(f.policy.OpenEvents(f.scope, "badmark", deadline, {{1, 8}}, now, body) == ChannelError::Invalid,
          "mark icon bound");
    Fixture slots(4, 1);
    slots.Open();
    Check(slots.Receive(slots.Query()) == ChannelError::Capacity, "pending work fails closed");
    auto failure = slots.Wire("failure");
    failure["code"] = "cancelled";
    Check(slots.Receive(failure) == ChannelError::None, "terminal releases only logical pending work");
    Check(slots.Receive(slots.Query()) == ChannelError::None, "capacity reuse with fresh request id");
    Check(slots.policy.RequestReservations() == 2, "terminal replay reservation retained");
    Fixture commands(2);
    commands.Open();
    Check(commands.Receive(commands.Command("a")) == ChannelError::None, "first effect reservation");
    Check(commands.Receive(commands.Command("b")) == ChannelError::None, "last effect reservation");
    Check(commands.Receive(commands.Command("c")) == ChannelError::Capacity,
          "effect capacity rejects instead of eviction");
    Check(commands.Receive(commands.Command("a")) == ChannelError::Duplicate, "oldest live effect retained");
    Fixture full;
    for (std::size_t i = 0; i < ChannelMaxIds; ++i)
    {
        auto const id = "work-" + std::to_string(i);
        full.Open(id);
        auto fail = full.Wire("failure", id);
        fail["code"] = "unavailable";
        Check(full.Receive(fail) == ChannelError::None, "retained request terminal");
    }
    Check(full.policy.RequestReservations() == 4096, "exact epoch request maximum");
    Check(full.policy.OpenGeneration(full.scope, "overflow", deadline, now) == ChannelError::Capacity,
          "request capacity fails closed");
    Check(full.policy.OpenGeneration(full.scope, "work-0", deadline, now) == ChannelError::Duplicate,
          "oldest terminal id retained");
    ChannelPolicy invalid(0);
    Check(!invalid.Bind({"fixture", "codex", "", "invalid-limits"}, bearer), "zero capacity fails closed");
    ChannelPolicy tooMany(ChannelMaxIds + 1);
    Check(!tooMany.Bind({"fixture", "codex", "", "invalid-limits"}, bearer), "cannot expand id bound");
    ChannelPolicy tooPending(1, ChannelMaxPending + 1);
    Check(!tooPending.Bind({"fixture", "codex", "", "invalid-limits"}, bearer), "cannot expand pending bound");

    Fixture effects;
    effects.Open();
    for (std::size_t i = 0; i < ChannelMaxIds; ++i)
        Check(effects.Receive(effects.Command("effect-" + std::to_string(i))) == ChannelError::None,
              "reserve full effect namespace");
    Check(effects.policy.CommandReservations() == 4096, "exact effect maximum");
    Check(effects.Receive(effects.Command("overflow")) == ChannelError::Capacity, "full effect namespace refuses");
    Check(effects.Receive(effects.Command("effect-0")) == ChannelError::Duplicate, "first effect never evicted");
    Fixture scopes;
    for (unsigned i = 1; i < ChannelMaxScopes; ++i)
    {
        auto scope = scopes.scope;
        scope.contextId = "context-" + std::to_string(i);
        Check(scopes.policy.ObserveScope(scope, now), "scope reservation up to maximum");
    }
    auto extra = scopes.scope;
    extra.contextId = "overflow";
    Check(!scopes.policy.ObserveScope(extra, now), "scope capacity fails closed");
    scopes.policy.RemoveScope(scopes.scope.botGuid, scopes.scope.contextId);
    Check(scopes.policy.ObserveScope(extra, now), "removed scope frees bounded scope slot");
    ChannelPolicy epochs;
    for (unsigned i = 0; i < ChannelMaxIds; ++i)
        Check(epochs.Bind({"fixture", "codex", "", "epoch-" + std::to_string(i)}, bearer),
              "bounded fresh epoch history");
    Check(!epochs.Bind({"fixture", "codex", "", "epoch-overflow"}, bearer), "epoch history capacity fails closed");
    ChannelMessage output;
    Check(epochs.Receive(bearer, f.Query().dump(), f.scope, now, output, "work") == ChannelError::Unauthorized,
          "epoch exhaustion leaves inactive");
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
    Check(f.policy.Receive(bearer, wire.dump(), f.scope, now, message, "work") == ChannelError::None,
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

void Topics()
{
    Fixture f;
    // Parse: the eleven topics, nothing else.
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
    Check(f.policy.Receive(bearer, longest.dump(), f.scope, now, routed, "work") == ChannelError::None &&
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

    // Quests: 25 entries, done never above needed.
    std::vector<StateEntry> quests{{"Odd status", "abandoned", 1, 2}};
    for (int i = 0; i < 30; ++i)
        quests.push_back({"Quest " + std::to_string(i),
                          i % 3 == 0   ? "complete"
                          : i % 3 == 1 ? "failed"
                                       : "active",
                          i == 0 ? 99 : -4, i == 0 ? 8 : 20000});
    entries = Topic(f, "quests", StateTopic::Quests, quests);
    Check(entries.size() == 25 && Keys(entries.at(0), {"title", "status", "done", "needed"}),
          "quests at most 25 with fixed keys");
    Check(entries.at(0).at("title") == "Quest 0" && entries.at(0).at("done") == 8 && entries.at(0).at("needed") == 8 &&
              entries.at(0).at("status") == "complete" && entries.at(1).at("status") == "failed" &&
              entries.at(2).at("status") == "active" && entries.at(1).at("done") == 0 &&
              entries.at(1).at("needed") == 10000,
          "quest progress bounded");

    // Cooldowns: 20 entries, only positive seconds, capped at a week.
    std::vector<StateEntry> cooldowns{{"Ready", "", 0, 0}};
    for (int i = 0; i < 30; ++i)
        cooldowns.push_back({"Spell " + std::to_string(i), "", i == 0 ? 100000000 : 30 - i, 0});
    entries = Topic(f, "cooldowns", StateTopic::Cooldowns, cooldowns);
    Check(entries.size() == 20 && Keys(entries.at(0), {"name", "seconds"}) && entries.at(0).at("name") == "Spell 0" &&
              entries.at(0).at("seconds") == 604800,
          "cooldowns at most 20, ready spells dropped, seconds capped");

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

    // WT18b: the planner's topics share one row shape; the row word comes from StateTopicRow, other rows dropped.
    struct RowWords
    {
        StateTopic topic;
        std::vector<std::string> words;
    };
    RowWords const tables[] = {{StateTopic::Plan, {"goal", "source", "reason", "stop"}},
                               {StateTopic::Options, {"option", "current"}},
                               {StateTopic::Route, {"total", "leg", "failed"}},
                               {StateTopic::QuestPlan, {"take", "task", "turn_in", "carried", "deferred"}},
                               {StateTopic::Bags, {"free", "sell", "bag"}},
                               {StateTopic::Places, {"hearth", "flight_here", "flight_elsewhere"}}};
    for (auto const& table : tables)
        for (std::size_t code = 0; code < 8; ++code)
            Check(std::string(StateTopicRow(table.topic, static_cast<uint8_t>(code))) ==
                      (code < table.words.size() ? table.words[code] : std::string()),
                  "each planner row code has its word, an unknown code none");
    for (std::size_t t = 0; t <= static_cast<std::size_t>(StateTopic::Spells); ++t)
        Check(!*StateTopicRow(static_cast<StateTopic>(t), 0) && !*StateTopicRow(static_cast<StateTopic>(t), 1),
              "older topics have no row words");
    Check(!*StateTopicRow(static_cast<StateTopic>(StateTopicCount), 0), "an unknown topic has no row words");
    Check(std::string(StateTopicName(StateTopic::Plan)) == "plan" &&
              std::string(StateTopicName(StateTopic::Options)) == "options" &&
              std::string(StateTopicName(StateTopic::Route)) == "route" &&
              std::string(StateTopicName(StateTopic::QuestPlan)) == "quest_plan" &&
              std::string(StateTopicName(StateTopic::Bags)) == "bags" &&
              std::string(StateTopicName(StateTopic::Places)) == "places",
          "planner topic names");
    Check(StateTopicLimit(StateTopic::Plan) == 8 && StateTopicLimit(StateTopic::Options) == 5 &&
              StateTopicLimit(StateTopic::Route) == 12 && StateTopicLimit(StateTopic::QuestPlan) == 12 &&
              StateTopicLimit(StateTopic::Bags) == 6 && StateTopicLimit(StateTopic::Places) == 24,
          "planner topic limits as playerbots keeps them");
    entries = Topic(f, "plan", StateTopic::Plan,
                    {{"rpg_travel", "goal", 120, 600},
                     {"planner", "source", -5, 5000000000LL},
                     {"near quests", "reason", 0, 0},
                     {"Kobold Camp Cleanup", "stop", 3, 250},
                     {"bogus", "option", 1, 1},
                     {"none", "", 0, 0},
                     {"", "goal", 1, 1}});
    Check(entries.size() == 4 && Keys(entries.at(0), {"row", "name", "value", "extra"}) &&
              entries.at(0).at("row") == "goal" && entries.at(0).at("name") == "rpg_travel" &&
              entries.at(0).at("value") == 120 && entries.at(0).at("extra") == 600 &&
              entries.at(1).at("row") == "source" && entries.at(1).at("value") == 0 &&
              entries.at(1).at("extra") == 4294967295LL && entries.at(2).at("name") == "near quests" &&
              entries.at(3).at("row") == "stop" && entries.at(3).at("name") == "Kobold Camp Cleanup",
          "plan rows: row word, name, value and extra bounded; another topic's row word and an empty name dropped");
    for (auto const& table : tables)
    {
        std::vector<StateEntry> rows;
        for (int i = 0; i < 30; ++i)
            rows.push_back({"word " + std::to_string(i), table.words.at(i % table.words.size()), i, i});
        auto const limited = Topic(f, std::string("limit-") + StateTopicName(table.topic), table.topic, rows);
        Check(limited.size() == StateTopicLimit(table.topic) &&
                  limited.at(1).at("row") == table.words.at(1 % table.words.size()),
              "a planner topic keeps its row words and stops at its limit");
    }

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
                                                               : StateTopicRow(topic, 0),
                             topic == StateTopic::Inventory ? 5 : 604800, 10000});
        auto const result = Topic(f, "worst-" + std::to_string(t), topic, worst);
        Check(result.size() == (topic == StateTopic::Strategies ? 0 : StateTopicLimit(topic)), "worst case count");
    }

    // Terminal once, unknown topic value refused, wrong work refused.
    ChannelMessage message;
    Check(f.policy.Receive(bearer, StateQuery(f, "once", "spells").dump(), f.scope, now, message, "work") ==
              ChannelError::None,
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
        Check(f.policy.Receive(bearer, WatchWire(f, id, name).dump(), f.scope, now, message, "work") ==
                  ChannelError::None,
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
    Check(f.policy.Receive(bearer, f.Query("q-watch").dump(), f.scope, now, query, "work") == ChannelError::None,
          "watch: a query route");
    Check(f.Receive(WatchWire(f, "on-query", "time", 160000, "q-watch")) == ChannelError::Invalid,
          "watch: never on a query route");
    std::string body;
    Check(f.policy.OpenEvents(f.scope, "ev-watch", deadline, {{1, 0}}, now, body) == ChannelError::None,
          "watch: an events route");
    Check(f.Receive(WatchWire(f, "on-events", "time", 160000, "ev-watch")) == ChannelError::Invalid,
          "watch: never on an events route");

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
    Words const words[] = {{CommandRefusal::NotFighting, "not_fighting", "I am not fighting anything right now."},
                           {CommandRefusal::NotBusy, "not_busy", "I am not busy with anything."},
                           {CommandRefusal::WaitTooShort, "wait_too_short", "That is hardly worth waiting for."},
                           {CommandRefusal::WaitTooLong, "wait_too_long", "I cannot wait that long."},
                           {CommandRefusal::PutOff, "put_off", "I have put this off long enough already."},
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
    auto terminal = f.Wire("generation.result");
    terminal["text"] = "I will come once this is done.";
    Check(f.Receive(terminal) == ChannelError::None && f.Receive(WatchWire(f, "late")) == ChannelError::Closed,
          "watch: none after the terminal result");
}

int main()
{
    try
    {
        IdentityAndTerminal();
        ExpiryAndScope();
        Schemas();
        QueriesEventsAndCapacity();
        Topics();
        Watches();
        std::cout << "PASS " << checks << " production channel checks\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL after " << checks << ": " << error.what() << '\n';
        return 1;
    }
}
