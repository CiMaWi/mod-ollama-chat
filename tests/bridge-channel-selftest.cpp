// Offline checks of the actual production channel helper; no game authority or effect executor.
#include <iostream>
#include <nlohmann/json.hpp>
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
        return policy.Receive(bearer, json.dump(), scope, clock, result);
    }
};

void IdentityAndTerminal()
{
    Fixture f;
    f.Open();
    ChannelMessage result;
    Check(f.policy.Receive("wrong", "{", f.scope, now, result) == ChannelError::Unauthorized, "auth precedes parse");
    Check(f.policy.Receive("wrong", std::string(MaxRequestBytes + 1, 'x'), f.scope, now, result) ==
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
    Check(failed.policy.Receive(bearer + "-new", failure.dump(), failed.scope, now, result) == ChannelError::Stale,
          "old terminal cannot cross reconnect");
    failure["epoch"] = "epoch-2";
    Check(failed.policy.Receive(bearer + "-new", failure.dump(), failed.scope, now, result) == ChannelError::Unknown,
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
    Check(f.policy.Receive(bearer, "{", f.scope, now, result) == ChannelError::Invalid, "malformed JSON");
    Check(
        f.policy.Receive(bearer, std::string(MaxRequestBytes + 1, ' '), f.scope, now, result) == ChannelError::TooLarge,
        "bounded body before parse");
    Check(f.policy.Receive(bearer, std::string(MaxRequestBytes, ' '), f.scope, now, result) == ChannelError::Invalid,
          "exact body maximum reaches parser");
    auto duplicate = f.Command().dump();
    duplicate.insert(1, "\"version\":1,");
    Check(f.policy.Receive(bearer, duplicate, f.scope, now, result) == ChannelError::Invalid,
          "duplicate key before DOM overwrite");
    duplicate = f.Command().dump();
    auto const position = duplicate.find("\"params\":{}");
    duplicate.replace(position, 11, "\"params\":{\"x\":1,\"x\":2}");
    Check(f.policy.Receive(bearer, duplicate, f.scope, now, result) == ChannelError::Invalid, "nested duplicate key");
    Check(f.policy.Receive(bearer, std::string(20, '[') + "0" + std::string(20, ']'), f.scope, now, result) ==
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
         std::vector<std::pair<std::string, Json>>{{"stay", {{"targetGuid", "1"}}},
                                                   {"pull", {{"targetGuid", "01"}}},
                                                   {"mark", {{"targetGuid", "1"}, {"icon", 8}}},
                                                   {"mark", {{"targetGuid", "1"}, {"icon", 1.0}}},
                                                   {"assign_role", {{"role", "leader"}}},
                                                   {"save_mana", {{"enabled", 1}}},
                                                   {"formation", {{"shape", "circle"}}},
                                                   {"dungeon_clear", {{"mode", "restart"}}},
                                                   {"position", {{"x", 1}, {"y", 2}, {"z", 3}, {"mapId", -1}}},
                                                   {"position", {{"x", "1"}, {"y", 2}, {"z", 3}, {"mapId", 0}}}})
    {
        auto bad = f.Command();
        bad["intent"] = pair.first;
        bad["params"] = pair.second;
        Check(f.Receive(bad) == ChannelError::Invalid, "strict intent parameters");
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
                                                   {"dungeon_clear", {{"mode", "pause"}}}})
    {
        auto good = f.Command("effect-" + std::to_string(++sequence));
        good["intent"] = pair.first;
        good["params"] = pair.second;
        Check(f.policy.Receive(bearer, good.dump(), f.scope, now, result) == ChannelError::None,
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
    Check(f.policy.Receive(bearer, f.Query().dump(), f.scope, now, query) == ChannelError::None,
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
    Check(f.policy.Receive(bearer, f.Query("q2").dump(), f.scope, now, query) == ChannelError::None, "second query");
    units.front().healthPercent = 101;
    Check(f.policy.FinishQuery(query.envelope, f.scope, units, now, body) == ChannelError::Invalid, "invalid UI field");
    units.front().healthPercent = 80;
    units.front().role = "healer";
    units.front().visibleTargetGuid = 18446744073709551615ULL;
    Check(f.policy.FinishQuery(query.envelope, f.scope, units, now, body) == ChannelError::None,
          "observed optional fields retained");
    parsed = Json::parse(body);
    Check(parsed.at("units").at(0).size() == 5, "exact declared unit fields");
    Check(f.policy.Receive(bearer, f.Query("q3").dump(), f.scope, now, query) == ChannelError::None,
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
    Check(epochs.Receive(bearer, f.Query().dump(), f.scope, now, output) == ChannelError::Unauthorized,
          "epoch exhaustion leaves inactive");
}
}  // namespace

int main()
{
    try
    {
        IdentityAndTerminal();
        ExpiryAndScope();
        Schemas();
        QueriesEventsAndCapacity();
        std::cout << "PASS " << checks << " production channel checks\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL after " << checks << ": " << error.what() << '\n';
        return 1;
    }
}
