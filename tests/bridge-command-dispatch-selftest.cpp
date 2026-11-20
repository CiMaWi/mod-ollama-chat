// Actual WorldChannel, WorldPolicy and ChannelPolicy with an instrumented typed effect seam; no game or PB code.
#include <algorithm>
#include <iostream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "bridge-ability-double.h"
#include "mod-ollama-chat_bridge_world.h"

namespace OllamaBridge
{
struct EffectPhase
{
};
}  // namespace OllamaBridge

namespace
{
using namespace OllamaBridge;
using PlayerbotBridge::Intent;
using Json = nlohmann::json;
unsigned checks = 0;
void Check(bool value, char const* label)
{
    ++checks;
    if (!value)
        throw std::runtime_error(label);
}

EffectPhase const phase{};

// One game effect, in the order the PB boundary uses it: ask authorize once for the whole order, mutate only when
// granted (playerbots checks each step against fresh facts).
struct Probe final : CommandExecutor
{
    std::optional<EffectResult> early;            // Refusal before the final check (PB Rejected/Unavailable).
    EffectResult result = EffectResult::Applied;  // Reported after the final check granted the attempt.
    bool skipAuthorize = false;
    bool askTwice = false;
    std::function<void()> before;  // Game facts or clocks change between admission and the final check.
    std::function<void()> after;   // The effect, or something right after it, changes facts or retires.
    unsigned calls = 0;
    unsigned grants = 0;
    unsigned denials = 0;
    unsigned mutations = 0;
    uint64_t controller = 0;
    uint64_t group = 0;
    std::optional<ChannelCommand> order;         // The latest order that mutated.
    std::size_t stepCount = 0;                   // LC3b: the steps of the latest order.
    std::vector<ChannelStepResult> stepResults;  // LC3b: what an order reports per step that ran, when a test sets it.
    std::vector<ChannelStepResult> ran;          // Otherwise: every step applied, or the one failed step.

    std::vector<ChannelStepResult> Steps() const override { return stepResults.empty() ? ran : stepResults; }

    EffectResult Execute(EffectPhase const&, WorldTicket const& ticket, DispatchGuard const& guard,
                         ChannelCommand const& command, std::function<bool()> const& authorize) override
    {
        ++calls;
        ran.clear();
        controller = ticket.scope.controllerGuid;
        group = guard.groupGuid;
        if (before)
            before();
        if (early)
            return *early;
        stepCount = command.steps.size();
        if (skipAuthorize)
            return result;
        bool const granted = authorize();
        ++(granted ? grants : denials);
        if (askTwice)
            ++(authorize() ? grants : denials);
        if (!granted || result == EffectResult::Rejected || result == EffectResult::Unavailable)
            return granted ? result : EffectResult::Rejected;
        ++mutations;
        order = command;
        if (result == EffectResult::Applied)
            ran.assign(command.steps.size(), {ChannelOutcome::Applied, "", ""});
        else
            ran = {{ChannelOutcome::Failed, "", ""}};
        if (after)
            after();
        return result;
    }
};

struct Options
{
    bool executor = true;
    int64_t lifetimeMs = 30000;
};

struct Fixture
{
    Clock now{100000, 200000};
    Settings settings{"fixture", "codex", "http://127.0.0.1:47800", "synthetic-local-channel-token"};
    WorldPolicy world;
    DispatchGuard guard;
    bool visible = true;
    Probe probe;
    WorldChannel channel;
    WorldTicket ticket;

    explicit Fixture(Options options = {})
        : channel(
              world,
              [this](WorldTicket const&, DispatchGuard& current)
              {
                  current = guard;
                  return visible;
              },
              [this] { return now; }, nullptr, options.executor ? &probe : nullptr)
    {
        channel.Configure(true);
        world.Configured(true, settings, "epoch-1");
        Check(world.ObserveDrain(channel.Drained()), "real selection");
        Check(channel.Activate(), "channel opt in");
        WorldScope scope{1, "fixture", "codex", "epoch-1", 11, 22, "context", "party", "visible", "c1", "m1"};
        Check(world.ObserveScope(scope, now), "trusted observed scope");
        Context input;
        input.speakerGuid = 22;
        input.channel = "party";
        input.text = "Ready?";
        ticket = world.Reserve(scope, input, now, options.lifetimeMs);
        Check(ticket.generation != 0, "actual world reservation");
        guard.scope = scope;
        guard.source = DispatchSource::Party;
        guard.speakerGuid = 22;
        guard.groupGuid = 33;
        guard.mapId = 1;
        guard.phaseMask = 1;
        guard.audience = {11, 22};
        guard.humanSpeaker = guard.authority = guard.perceptible = guard.audienceOnline = guard.nonRaid = true;
        Check(channel.Register(ticket, guard), "registered generation route and exact guard");
        Check(world.MarkQueued(ticket, now), "generation may remain blocked independently");
    }

    // Time passes in 1 s steps with world heartbeats and empty world ticks, so no observation gap opens.
    void Advance(int64_t ms)
    {
        while (ms > 0)
        {
            int64_t const step = std::min<int64_t>(ms, 1000);
            ms -= step;
            now.unixMs += step;
            now.steadyMs += step;
            Check(world.ObserveScope(ticket.scope, now) && channel.Update(&phase) == 0, "heartbeat on an empty lane");
        }
    }
    Json Wire(std::string type, std::string id) const
    {
        return {
            {"version", 1},           {"instance", "fixture"},      {"provider", "codex"},
            {"epoch", "epoch-1"},     {"requestId", std::move(id)}, {"deadlineUnixMs", ticket.request.deadline.unixMs},
            {"type", std::move(type)}};
    }
    std::string Query(std::string id) const
    {
        auto j = Wire("query", std::move(id));
        j["controllerGuid"] = "22";
        j["botGuid"] = "11";
        j["query"] = "observable_group";
        return j.dump();
    }
    // A one-step order.
    std::string Command(std::string const& id, char const* act, Json params = Json::object()) const
    {
        return Steps(id, Json::array({{{"act", act}, {"params", std::move(params)}}}));
    }
    // LC3b: an order of several steps.
    std::string Steps(std::string const& id, Json steps) const
    {
        auto j = Wire("command", ticket.request.requestId);
        j["commandId"] = id;
        j["issuerGuid"] = "22";
        j["botGuid"] = "11";
        j["steps"] = std::move(steps);
        return j.dump();
    }
    std::string Attack(std::string const& id) const { return Command(id, "attack_own_target", {{"targetGuid", "44"}}); }
    std::string Mark(std::string const& id) const
    {
        return Command(id, "mark", {{"targetGuid", "44"}, {"icon", "triangle"}});
    }
    uint64_t Receive(std::string const& body)
    {
        uint64_t receipt = 0;
        Check(channel.Receive(ticket.request.requestId, body, receipt) == ChannelError::None,
              "authenticated producer admission");
        return receipt;
    }
    // One message through one world tick, with the post-map-join phase unless a test withholds it.
    uint64_t Send(std::string const& body, bool withPhase = true)
    {
        auto const receipt = Receive(body);
        Check(channel.Update(withPhase ? &phase : nullptr) == 1, "one bounded dequeue");
        return receipt;
    }
    std::unique_ptr<ChannelPacket> Take(uint64_t receipt)
    {
        std::unique_ptr<ChannelPacket> packet;
        Check(channel.Take(receipt, packet) == ChannelError::None && packet, "authenticated completion operation");
        return packet;
    }
    // J5e: the last Result's refusal reason and sentence (empty when applied) and its packet error.
    std::string reason;
    std::string reasonText;
    Json steps;  // LC3b: a result's step list (null for a fixed error).
    ChannelError packetError = ChannelError::None;
    // The single correlated receipt: its outcome, a fixed error name, or "absent" when withheld or disposed.
    std::string Result(uint64_t receipt, std::string const& id)
    {
        reason.clear();
        reasonText.clear();
        steps = Json();
        packetError = ChannelError::None;
        std::unique_ptr<ChannelPacket> packet;
        auto const error = channel.Take(receipt, packet);
        if (error != ChannelError::None)
            return error == ChannelError::Unknown ? "absent" : ChannelErrorName(error);
        Check(packet != nullptr, "taken receipt");
        if (packet->error != ChannelError::None)
        {
            Check(packet->body.empty(), "fixed error without payload");
            packetError = packet->error;
            return ChannelErrorName(packet->error);
        }
        auto const body = Json::parse(packet->body);
        bool const refused = body.value("outcome", "") != "applied";
        steps = body.contains("steps") ? body.at("steps") : Json();
        Check(steps.is_array() && !steps.empty(), "a result lists the order's steps");
        Check(body.size() == (refused ? 12u : 10u) && body["type"] == "command.result" && body["commandId"] == id &&
                  body["requestId"] == ticket.request.requestId && body["epoch"] == "epoch-1",
              "correlated command.result");
        if (refused)
        {
            reason = body.at("reason").get<std::string>();
            reasonText = body.at("reasonText").get<std::string>();
            Check(!reason.empty() && reason.size() <= 40 &&
                      reason.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") == std::string::npos &&
                      !reasonText.empty() && reasonText.size() <= 200 &&
                      reasonText.find_first_of("\"\\") == std::string::npos,
                  "a refusal carries a short code and a plain sentence");
        }
        return body["outcome"].get<std::string>();
    }
};

void PureSeam()
{
    Check(EffectOutcome(EffectResult::Applied, true) == ChannelOutcome::Applied &&
              EffectOutcome(EffectResult::Failed, true) == ChannelOutcome::Failed &&
              EffectOutcome(EffectResult::Rejected, true) == ChannelOutcome::Rejected &&
              EffectOutcome(EffectResult::Unavailable, true) == ChannelOutcome::Rejected &&
              EffectOutcome(EffectResult::Rejected, false) == ChannelOutcome::Rejected &&
              EffectOutcome(EffectResult::Unavailable, false) == ChannelOutcome::Rejected,
          "PB results map to the three wire outcomes");
    Check(EffectOutcome(EffectResult::Applied, false) == ChannelOutcome::Failed &&
              EffectOutcome(EffectResult::Failed, false) == ChannelOutcome::Failed,
          "an unauthorized claimed attempt is failed, never applied");

    CommandRate rate;
    int64_t const s = 200000;
    Check(rate.Admit(11, {1000, s}) && !rate.Admit(11, {1000, s + 1999}) && rate.Admit(12, {1000, s}),
          "per-bot interval on independent lanes");
    Check(!rate.Admit(0, {1000, s + 5000}) && !rate.Admit(13, {1000, -1}), "invalid lane or clock is denied");
    Check(!rate.Admit(11, {99000, s + 1999}), "a wall clock jump cannot open the gate");
    Check(rate.Admit(11, {1000, s + 2000}), "admitted exactly at the interval");
    Check(!rate.Admit(11, {1000, s}) && !rate.Admit(11, {1000, s + 3999}) && rate.Admit(11, {1000, s + 4000}),
          "steady rollback cannot reopen the gate");
    Check(rate.Admit(20, {1000, s + 1000}) && !rate.Admit(20, {1000, s + 5999}) && rate.Admit(20, {1000, s + 6000}),
          "admission during rollback is recorded at the high-water mark");

    CommandRate full;
    for (uint64_t bot = 1; bot <= CommandRate::MaxBots; ++bot)
        Check(full.Admit(bot, {1000, s}), "bounded table fill");
    Check(!full.Admit(100, {1000, s + 1999}) && full.Size() == CommandRate::MaxBots,
          "a full table denies a new lane without growing");
    Check(full.Admit(100, {1000, s + 2000}) && full.Size() == 1 && !full.Admit(100, {1000, s + 2001}),
          "only lanes whose interval passed are evicted");
}

// LCB2: each act reaches the executor as its row's step: the act's playerbots Intent and its parameters as given.
void StepVerbs()
{
    Fixture f;
    auto receipt = f.Send(f.Attack("attack-1"));
    Check(f.probe.calls == 1 && f.probe.grants == 1 && f.probe.mutations == 1, "one authorized attack");
    Check(f.probe.controller == 22 && f.probe.group == 33, "executor gets the captured route and guard");
    auto const* step = &f.probe.order->steps.front();
    Check(f.probe.order->steps.size() == 1 && step->intent == Intent::AttackOwnTarget && step->targetGuid == 44,
          "the attack's step and its target");
    Check(f.Result(receipt, "attack-1") == "applied", "attack receipt");
    f.Advance(CommandRate::IntervalMs);
    receipt = f.Send(f.Mark("mark-1"));
    step = &f.probe.order->steps.front();
    Check(f.probe.calls == 2 && f.probe.mutations == 2 && step->intent == Intent::Mark && step->targetGuid == 44 &&
              step->icon == 3,
          "the mark's step, target and icon");
    Check(f.Result(receipt, "mark-1") == "applied", "mark receipt");
    unsigned calls = 2;
    std::pair<char const*, Intent> const oneShots[] = {{"follow", Intent::Follow}, {"stay", Intent::Stay}};
    for (auto const& [act, typed] : oneShots)
    {
        f.Advance(CommandRate::IntervalMs);
        receipt = f.Send(f.Command(act, act));
        ++calls;
        step = &f.probe.order->steps.front();
        Check(f.probe.calls == calls && f.probe.grants == calls && f.probe.mutations == calls &&
                  step->intent == typed && !step->targetGuid && step->targetName.empty(),
              "one authorized target-less one-shot order");
        Check(f.Result(receipt, act) == "applied", "one-shot receipt");
    }
    f.Advance(CommandRate::IntervalMs);
    receipt = f.Send(f.Command("meet-1", "meet_at", {{"place", "you"}}));
    step = &f.probe.order->steps.front();
    Check(f.probe.calls == 5 && f.probe.grants == 5 && f.probe.mutations == 5 && step->intent == Intent::MeetAt &&
              step->place == "you",
          "one authorized meet_at order with the place as given");
    Check(f.Result(receipt, "meet-1") == "applied", "meet_at receipt");
    auto const& stats = f.channel.Commands();
    Check(stats.proposed == 5 && stats.attempts == 5 && stats.applied == 5 && !stats.refused && !stats.failed &&
              !stats.withheld && !stats.duplicates && f.channel.Drained(),
          "watchlist counters and slots");
}

void OutcomesNeverReplay()
{
    struct Case
    {
        std::optional<EffectResult> early;
        EffectResult result;
        char const* outcome;
        unsigned mutations;
    };
    for (auto const& c : {Case{std::nullopt, EffectResult::Applied, "applied", 1},
                          Case{std::nullopt, EffectResult::Failed, "failed", 1},
                          Case{std::nullopt, EffectResult::Rejected, "rejected", 0},
                          Case{EffectResult::Rejected, EffectResult::Applied, "rejected", 0},
                          Case{EffectResult::Unavailable, EffectResult::Applied, "rejected", 0}})
    {
        Fixture f;
        f.probe.early = c.early;
        f.probe.result = c.result;
        auto receipt = f.Send(f.Attack("once"));
        Check(f.probe.calls == 1 && f.probe.mutations == c.mutations, "one executor call per outcome class");
        Check(f.Result(receipt, "once") == c.outcome, "outcome class receipt");
        f.Advance(CommandRate::IntervalMs);  // The rate gate is not why the retries below are refused.
        Check(f.Result(f.Send(f.Attack("once")), "once") == "duplicate" && f.probe.calls == 1,
              "a retried id never reaches the executor again");
        Check(f.Result(f.Send(f.Mark("once")), "once") == "duplicate" && f.probe.calls == 1,
              "the id stays consumed for every verb");
        Check(f.channel.Update(&phase) == 0 && f.probe.calls == 1 && f.channel.Drained(), "no later tick replays");
        Check(f.channel.Commands().duplicates == 2 && f.channel.Commands().proposed == 1, "duplicates counted");
    }
}

void FixedRefusals()
{
    Fixture lasting;
    lasting.Send(lasting.Command("lasting", "formation", Json{{"shape", "close"}}));
    Check(lasting.probe.calls == 1 && lasting.channel.Commands().attempts == 1,
          "a lasting setting reaches the executor");

    Fixture closed({.executor = false});
    Check(closed.Result(closed.Send(closed.Attack("no-executor")), "no-executor") == "rejected" &&
              !closed.channel.Commands().attempts,
          "null executor keeps today's refusal");
    std::pair<char const*, Json> const oneShots[] = {
        {"follow", Json::object()}, {"stay", Json::object()}, {"meet_at", Json{{"place", "you"}}}};
    for (auto const& [intent, params] : oneShots)
    {
        Fixture none({.executor = false});
        Check(none.Result(none.Send(none.Command(intent, intent, params)), intent) == "rejected" &&
                  !none.channel.Commands().attempts,
              "null executor refuses a one-shot");
        // The closed production executor refuses before any game read: the verb is attempted, nothing changes.
        Fixture shut;
        shut.probe.early = EffectResult::Rejected;
        Check(shut.Result(shut.Send(shut.Command(intent, intent, params)), intent) == "rejected" &&
                  shut.probe.calls == 1 && !shut.probe.mutations && shut.channel.Commands().attempts == 1 &&
                  shut.channel.Commands().refused == 1,
              "a closed executor refuses a one-shot without an effect");
        Check(shut.Result(shut.Send(shut.Attack("lane")), "lane") == "rejected" && shut.probe.calls == 1,
              "the refused one-shot still spent the bot's rate interval");
    }
    Fixture unphased;
    Check(unphased.Result(unphased.Send(unphased.Mark("no-phase"), false), "no-phase") == "rejected" &&
              unphased.probe.calls == 0,
          "no post-map-join phase, no effect");
    Check(unphased.Result(unphased.Send(unphased.Mark("with-phase")), "with-phase") == "applied" &&
              unphased.probe.calls == 1,
          "a phase-less refusal spends no rate interval");
}

// The action matrix: each act of the installed catalogue ends as a delivered effect (one executor call, one authorized
// mutation, an applied receipt, the bot's rate interval spent). LCB2: a body of the old form (a verb and its params)
// has no shape: the channel refuses it as invalid input before any command exists, so nothing is admitted, attempted
// or executed and no rate interval is spent. A fresh channel per row keeps the rows independent.
void ActMatrix()
{
    std::map<std::string, Json> const rows = {{"follow", Json::object()},
                                              {"mark", {{"target", "Hogger"}, {"icon", "skull"}}},
                                              {"go_to", {{"place", "Goldshire"}}},
                                              {"stay", Json::object()},
                                              {"quest", {{"questId", 42}}},
                                              {"meet_at", {{"place", "you"}}},
                                              {"formation", {{"shape", "close"}}},
                                              {"save_mana", {{"enabled", true}}},
                                              {"focus_healing", {{"targetGuid", "44"}}},
                                              {"attack_own_target", {{"targetGuid", "44"}}}};
    std::size_t delivered = 0;
    for (auto const& row : Abilities().rows)
    {
        if (row.topic)
            continue;
        auto const params = rows.find(row.name);
        Check(params != rows.end(), "the matrix names every act of the catalogue");
        Fixture f;
        Check(f.Result(f.Send(f.Command("matrix", row.name.c_str(), params->second)), "matrix") == "applied" &&
                  f.reason.empty(),
              "matrix receipt");
        auto const& stats = f.channel.Commands();
        Check(f.probe.calls == 1 && f.probe.grants == 1 && f.probe.mutations == 1 && stats.attempts == 1 &&
                  stats.applied == 1 && !stats.refused &&
                  f.probe.order->steps.front().intent == static_cast<Intent>(row.code),
              "a delivered act reaches the executor once with one authorized mutation, as its row's Intent");
        Check(f.Result(f.Send(f.Attack("after-delivered")), "after-delivered") == "rejected" && f.probe.calls == 1,
              "a delivered act spends the bot's rate interval");
        Check(f.reason == "too_soon", "a rate refusal names its reason (too_soon)");
        ++delivered;
    }
    Check(delivered == rows.size(), "every act of the matrix delivered");
    std::pair<char const*, Json> const old[] = {{"attack_own_target", Json::object()},
                                                {"mark", {{"targetGuid", "44"}, {"icon", 3}}},
                                                {"follow", Json::object()},
                                                {"stay", {{"standing", true}}},
                                                {"pull", {{"targetGuid", "44"}}}};
    for (auto const& [verb, params] : old)
    {
        Fixture f;
        auto body = Json::parse(f.Command("old", "stay"));
        body.erase("steps");
        body["intent"] = verb;
        body["params"] = params;
        Check(f.Result(f.Send(body.dump()), "old") == "invalid_input" && f.packetError == ChannelError::Invalid,
              "LCB2: an old-form body is invalid input");
        Check(f.probe.calls == 0 && !f.channel.Commands().proposed && !f.channel.Commands().attempts,
              "an old-form body never reaches the executor");
        Check(f.Result(f.Send(f.Attack("after-old")), "after-old") == "applied",
              "a refused old-form body spends no rate interval");
    }
}

void TerminalAndIds()
{
    Fixture f;
    auto query = f.Take(f.Send(f.Query("shared-id")));
    Check(query->error == ChannelError::None && Json::parse(query->body)["code"] == "unavailable",
          "query on its own id");
    query.reset();
    Check(f.Result(f.Send(f.Attack("shared-id")), "shared-id") == "applied" && f.probe.calls == 1,
          "a command id may equal a query id");
    query = f.Take(f.Send(f.Query("command-then-query")));
    query.reset();
    f.Advance(CommandRate::IntervalMs);
    Check(f.Result(f.Send(f.Mark("command-then-query")), "command-then-query") == "applied" && f.probe.calls == 2,
          "query ids never consume command ids");

    auto result = f.Wire("generation.result", f.ticket.request.requestId);
    result["text"] = "Done.";
    Check(f.Result(f.Send(result.dump()), "") == "invalid_input", "LCB6a: a generation result is no channel type");
    f.Advance(CommandRate::IntervalMs);
    Check(f.Result(f.Send(f.Mark("after-stray")), "after-stray") == "applied" && f.probe.calls == 3,
          "a stray post neither closes the route nor consumes an id");

    Fixture c;
    auto const id = c.Receive(c.Attack("closed-route"));
    c.channel.Close(c.ticket);
    Check(c.channel.Update(&phase) == 0 && c.probe.calls == 0 && c.channel.Drained() &&
              c.Result(id, "closed-route") == "absent",
          "a closed parent route disposes its queued command");
}

void RetirementBeforeTick()
{
    Fixture f;
    auto const id = f.Receive(f.Attack("retired"));
    f.world.Configured(true, f.settings, "epoch-2");
    Check(f.channel.Update(&phase) == 0 && f.probe.calls == 0, "epoch switch before the tick: no world work");
    f.channel.Configure(true);
    Check(f.channel.Drained() && f.world.ObserveDrain(f.channel.Drained()) && f.channel.Activate(),
          "retirement disposed the queued command; new epoch selected");
    Check(f.Result(id, "retired") == "absent", "no receipt from a retired epoch");
    uint64_t again = 0;
    Check(f.channel.Receive(f.ticket.request.requestId, f.Attack("retired"), again) == ChannelError::None &&
              f.channel.Update(&phase) == 1 && f.probe.calls == 0 && f.channel.Drained(),
          "the old route cannot carry the command into the new epoch");
}

void QueueAndRate()
{
    Fixture f;
    std::vector<uint64_t> receipts;
    for (unsigned i = 0; i < WorldChannel::MaxPackets; ++i)
        receipts.push_back(f.Receive(f.Attack("burst-" + std::to_string(i))));
    uint64_t overflow = 7;
    Check(f.channel.Receive(f.ticket.request.requestId, f.Attack("burst-over"), overflow) == ChannelError::Capacity &&
              !overflow,
          "full physical queue refuses before parsing");
    for (unsigned tick = 0; tick < WorldChannel::MaxPackets / WorldChannel::TickBudget; ++tick)
        Check(f.channel.Update(&phase) == WorldChannel::TickBudget, "bounded dequeue per tick");
    Check(f.probe.calls == 1 && f.channel.Used() == WorldChannel::MaxPackets,
          "one effect per bot interval; completions keep their slots");
    unsigned applied = 0;
    unsigned rejected = 0;
    for (unsigned i = 0; i < receipts.size(); ++i)
    {
        auto const outcome = f.Result(receipts[i], "burst-" + std::to_string(i));
        applied += outcome == "applied" ? 1 : 0;
        rejected += outcome == "rejected" ? 1 : 0;
    }
    Check(applied == 1 && rejected == 63 && f.channel.Commands().refused == 63, "rate denial is a rejected outcome");
    Check(f.channel.Used() == 0 && f.channel.Drained(), "every physical slot returns after disposal");
}

void LostGuards()
{
    struct Case
    {
        char const* label;
        void (*change)(Fixture&);
    };
    // Lost between admission and the final check: the executor asks once, is denied and mutates nothing.
    Case const denied[] = {
        {"lost authority", [](Fixture& f) { f.guard.authority = false; }},
        {"changed group", [](Fixture& f) { f.guard.groupGuid = 34; }},
        {"lost perception", [](Fixture& f) { f.visible = false; }},
        {"changed controller", [](Fixture& f) { f.guard.speakerGuid = 23; }},
        {"scope revision",
         [](Fixture& f)
         {
             auto revised = f.ticket.scope;
             revised.contextRevision = "c2";
             Check(f.world.ObserveScope(revised, f.now), "trusted revision");
         }},
    };
    for (auto const& c : denied)
    {
        Fixture f;
        f.probe.before = [&f, &c] { c.change(f); };
        auto const receipt = f.Send(f.Attack("lost"));
        Check(f.probe.calls == 1 && f.probe.denials == 1 && !f.probe.grants && !f.probe.mutations, c.label);
        Check(f.Result(receipt, "lost") == "absent" && f.channel.Commands().refused == 1 &&
                  f.channel.Commands().withheld == 1 && f.channel.Drained(),
              "lost authority publishes no receipt");
    }
    Fixture retry;
    retry.probe.before = [&retry] { retry.guard.authority = false; };
    retry.Result(retry.Send(retry.Attack("lost")), "lost");
    retry.probe.before = nullptr;
    retry.guard.authority = true;
    retry.Advance(CommandRate::IntervalMs);
    Check(retry.Result(retry.Send(retry.Attack("lost")), "lost") == "duplicate" && retry.probe.calls == 1,
          "authority regained never replays the refused id");

    Fixture s;
    auto const id = s.Receive(s.Attack("revised"));
    auto revised = s.ticket.scope;
    revised.contextRevision = "c2";
    Check(s.world.ObserveScope(revised, s.now), "trusted revision");
    Check(s.channel.Update(&phase) == 1 && s.probe.calls == 0 && s.channel.Drained() &&
              s.Result(id, "revised") == "absent",
          "a revision before the tick drops the command before admission");
}

void RateAndClock()
{
    Fixture f;
    Check(f.Result(f.Send(f.Attack("r1")), "r1") == "applied", "first admitted");
    f.Advance(CommandRate::IntervalMs - 1);
    Check(f.Result(f.Send(f.Mark("r2")), "r2") == "rejected" && f.probe.calls == 1,
          "rate denial happens before the executor");
    f.Advance(1);
    Check(f.Result(f.Send(f.Attack("r3")), "r3") == "applied" && f.probe.calls == 2, "admitted after the interval");
    f.now.steadyMs -= 1000;
    Check(f.Result(f.Send(f.Attack("r4")), "r4") == "rejected" && f.probe.calls == 2,
          "steady rollback cannot reopen the gate");
    Check(f.Result(f.Send(f.Attack("r2")), "r2") == "duplicate" && f.probe.calls == 2,
          "a rate-denied id stays consumed");

    struct Edge
    {
        int64_t wall;
        int64_t steady;
        bool granted;
        char const* label;
    };
    for (auto const& e :
         {Edge{-1, -1, true, "one ms before both deadlines"}, Edge{-1, 0, false, "steady deadline equality is expired"},
          Edge{0, -1, false, "wall deadline equality is expired"},
          Edge{-2000, 0, false, "wall rollback cannot extend a steady expiry"},
          Edge{0, -2000, false, "steady rollback cannot extend a wall expiry"}})
    {
        Fixture d({.lifetimeMs = 3000});
        d.probe.before = [&d, &e]
        { d.now = {d.ticket.request.deadline.unixMs + e.wall, d.ticket.request.deadline.steadyMs + e.steady}; };
        auto const receipt = d.Send(d.Attack("edge"));
        Check(
            d.probe.calls == 1 && d.probe.grants == (e.granted ? 1u : 0u) && d.probe.mutations == (e.granted ? 1u : 0u),
            e.label);
        Check(d.Result(receipt, "edge") == (e.granted ? "applied" : "absent") && d.channel.Drained(), e.label);
    }
}

void PostEffectJoin()
{
    for (auto const result : {EffectResult::Applied, EffectResult::Failed})
    {
        Fixture f;
        f.probe.result = result;
        f.probe.after = [&f]
        {
            f.visible = false;  // The attack or mark changed what Fresh would observe now.
            f.guard.perceptible = false;
        };
        auto const receipt = f.Send(f.Attack("changes-facts"));
        Check(f.probe.calls == 1 && f.probe.mutations == 1, "one attempt");
        Check(f.Result(receipt, "changes-facts") == (result == EffectResult::Applied ? "applied" : "failed"),
              "the post-effect join publishes the authorized outcome");
        f.visible = f.guard.perceptible = true;
        f.Advance(CommandRate::IntervalMs);
        Check(f.Result(f.Send(f.Attack("changes-facts")), "changes-facts") == "duplicate" && f.probe.calls == 1,
              "one result per command id");
    }

    struct Case
    {
        char const* label;
        void (*after)(Fixture&);
        char const* receipt;
        bool reopens;
    };
    Case const withheld[] = {
        {"world epoch switch after the effect", [](Fixture& f) { f.world.Configured(true, f.settings, "epoch-2"); },
         "absent", false},
        {"channel retired inside the executor", [](Fixture& f) { f.channel.Retire(); }, "inactive", false},
        {"parent route closed inside the executor", [](Fixture& f) { f.channel.Close(f.ticket); }, "absent", true},
        {"receipt deadline passed during the effect",
         [](Fixture& f)
         {
             f.now.unixMs += WorldLivenessMs;
             f.now.steadyMs += WorldLivenessMs;
         },
         "absent", true},
    };
    for (auto const& c : withheld)
    {
        Fixture f;
        f.probe.after = [&f, &c] { c.after(f); };
        auto const receipt = f.Send(f.Attack("withheld"));
        auto const& stats = f.channel.Commands();
        Check(f.probe.calls == 1 && f.probe.mutations == 1 && stats.applied == 1 && stats.withheld == 1, c.label);
        Check(f.Result(receipt, "withheld") == c.receipt && f.channel.Drained(), c.label);
        Check(f.channel.Activate() == c.reopens && f.channel.Update(&phase) == 0 && f.probe.calls == 1,
              "no later tick re-runs the effect");
    }

    Fixture twice;
    twice.probe.askTwice = true;
    Check(twice.Result(twice.Send(twice.Attack("twice")), "twice") == "applied" && twice.probe.grants == 1 &&
              twice.probe.denials == 1,
          "one authorization per command");
    Fixture rogue;
    rogue.probe.skipAuthorize = true;
    Check(rogue.Result(rogue.Send(rogue.Attack("rogue")), "rogue") == "failed" && !rogue.probe.grants &&
              !rogue.probe.denials && rogue.channel.Commands().failed == 1,
          "an effect claimed without authorization is never reported applied");
}

void ActedRoutes()
{
    // DS.14 (b): Close reports whether the route made an authorized effect attempt (Applied or Failed), its receipt
    // published or withheld; refusals, fixed refusals, queries and an unused route do not count. A second Close and
    // an unknown route report nothing.
    struct Case
    {
        char const* label;
        void (*arrange)(Fixture&);
        bool acted;
    };
    Case const cases[] = {
        {"applied", [](Fixture& f) { f.Send(f.Attack("a")); }, true},
        {"failed",
         [](Fixture& f)
         {
             f.probe.result = EffectResult::Failed;
             f.Send(f.Attack("a"));
         },
         true},
        {"withheld receipt",
         [](Fixture& f)
         {
             f.probe.after = [&f]
             {
                 f.now.unixMs += WorldLivenessMs;
                 f.now.steadyMs += WorldLivenessMs;
             };
             f.Send(f.Attack("a"));
         },
         true},
        {"executor refusal",
         [](Fixture& f)
         {
             f.probe.early = EffectResult::Rejected;
             f.Send(f.Attack("a"));
         },
         false},
        {"refused after the grant",
         [](Fixture& f)
         {
             f.probe.result = EffectResult::Rejected;
             f.Send(f.Attack("a"));
         },
         false},
        {"authorization denied",
         [](Fixture& f)
         {
             f.probe.before = [&f] { f.guard.perceptible = false; };
             f.Send(f.Attack("a"));
         },
         false},
        {"no phase", [](Fixture& f) { f.Send(f.Attack("a"), false); }, false},
        {"claimed without authorization",
         [](Fixture& f)
         {
             f.probe.skipAuthorize = true;
             f.Send(f.Attack("a"));
         },
         false},
        {"query only", [](Fixture& f) { f.Send(f.Query("q")); }, false},
        {"no command", [](Fixture&) {}, false},
    };
    for (auto const& c : cases)
    {
        Fixture f;
        c.arrange(f);
        Check(f.channel.Close(f.ticket) == c.acted, c.label);
        Check(!f.channel.Close(f.ticket), "a closed route reports nothing twice");
    }
}

void TransportLoss()
{
    // Outcome decided, then the receipt is lost in transport: nothing re-executes.
    Fixture f;
    auto const lost = f.Send(f.Attack("lost-receipt"));
    f.Advance(WorldLivenessMs);
    std::unique_ptr<ChannelPacket> packet;
    Check(f.channel.Take(lost, packet) == ChannelError::Stale && !packet && f.channel.Drained(),
          "an expired receipt is disposed, never resent");
    Check(f.Result(f.Send(f.Attack("lost-receipt")), "lost-receipt") == "duplicate" && f.probe.calls == 1,
          "a producer retry after receipt loss is a duplicate");
    f.Advance(CommandRate::IntervalMs);
    f.Take(f.Send(f.Mark("dropped-write"))).reset();
    Check(f.Result(f.Send(f.Mark("dropped-write")), "dropped-write") == "duplicate" && f.probe.calls == 2,
          "a taken receipt lost by the writer is not replayed");

    // Lost before admission: the id was never consumed, so it runs exactly once later.
    Fixture p;
    p.visible = false;
    auto const unseen = p.Receive(p.Attack("unseen"));
    Check(p.channel.Update(&phase) == 1 && p.probe.calls == 0 && p.Result(unseen, "unseen") == "absent" &&
              !p.channel.Commands().proposed,
          "lost perception at the tick drops before admission");
    p.visible = true;
    Check(p.Result(p.Send(p.Attack("unseen")), "unseen") == "applied" && p.probe.calls == 1,
          "a never-admitted id runs once later");
    Check(p.Result(p.Send(p.Attack("unseen")), "unseen") == "duplicate" && p.probe.calls == 1, "and only once");
}
}  // namespace

// LC3b: a stepped order is one command on the world thread: one rate slot, one executor call (so one authorize),
// and its result lists each step as the executor reported it.
void SteppedDispatch()
{
    Json const three = Json::array({{{"act", "follow"}, {"params", Json::object()}},
                                    {{"act", "go_to"}, {"params", {{"place", "Goldshire"}}}},
                                    {{"act", "mark"}, {"params", {{"target", "Hogger"}, {"icon", "skull"}}}}});
    ChannelStepResult const ok{ChannelOutcome::Applied, "", ""};
    {
        Fixture f;
        f.probe.stepResults = {ok, ok, ok};
        Check(f.Result(f.Send(f.Steps("s1", three)), "s1") == "applied" && f.probe.calls == 1 && f.probe.grants == 1 &&
                  f.probe.mutations == 1 && f.probe.stepCount == 3,
              "a three-step order reaches the executor once, with one authorize");
        Check(f.steps == Json::parse(R"([{"act":"follow","outcome":"applied"},{"act":"go_to","outcome":"applied"},
                  {"act":"mark","outcome":"applied"}])"),
              "each step as the executor reported it");
        Check(f.channel.Commands().attempts == 1 && f.channel.Commands().applied == 1, "one order is one attempt");
        Check(f.Result(f.Send(f.Steps("s2", three)), "s2") == "rejected" && f.reason == "too_soon" &&
                  f.probe.calls == 1 &&
                  f.steps == Json::parse(R"([{"act":"follow","outcome":"skipped"},{"act":"go_to","outcome":"skipped"},
                      {"act":"mark","outcome":"skipped"}])"),
              "the order took one rate slot: the next within the interval is refused, every step skipped");
    }
    {
        Fixture f;
        f.probe.result = EffectResult::Rejected;
        f.probe.stepResults = {ok, {ChannelOutcome::Rejected, "unknown_name", "You know nothing by that name."}};
        Check(f.Result(f.Send(f.Steps("s3", three)), "s3") == "rejected" && f.reason == "unknown_name" &&
                  f.steps.size() == 3 && f.steps[1].at("reason") == "unknown_name" &&
                  f.steps[2] == Json{{"act", "mark"}, {"outcome", "skipped"}},
              "a refused second step words the order; the third is skipped");
        Check(f.channel.Close(f.ticket), "an earlier step that applied makes the route act");
    }
    {
        Fixture f;
        f.probe.early = EffectResult::Rejected;
        Check(f.Result(f.Send(f.Steps("s4", three)), "s4") == "rejected" && f.reason == "cannot_now" &&
                  f.steps.size() == 3 && f.steps[0] == Json{{"act", "follow"}, {"outcome", "skipped"}} &&
                  !f.channel.Close(f.ticket),
              "an order the executor refused before any step: all skipped, no act");
    }
    {
        Fixture f({.executor = false});
        Check(f.Result(f.Send(f.Steps("s5", three)), "s5") == "rejected" && f.reason == "unavailable" &&
                  f.steps.size() == 3 && f.steps[2] == Json{{"act", "mark"}, {"outcome", "skipped"}},
              "no executor: the order refused with every step skipped");
    }
}

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
        PureSeam();
        StepVerbs();
        OutcomesNeverReplay();
        FixedRefusals();
        ActMatrix();
        TerminalAndIds();
        RetirementBeforeTick();
        QueueAndRate();
        LostGuards();
        RateAndClock();
        PostEffectJoin();
        ActedRoutes();
        TransportLoss();
        SteppedDispatch();
        std::cout << "PASS " << checks << " typed command dispatch checks\n";
    }
    catch (std::exception const& e)
    {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
