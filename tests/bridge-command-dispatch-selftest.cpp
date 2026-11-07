// Actual WorldChannel, WorldPolicy and ChannelPolicy with an instrumented typed effect seam; no game or PB code.
#include <algorithm>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

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
using Json = nlohmann::json;
unsigned checks = 0;
void Check(bool value, char const* label)
{
    ++checks;
    if (!value)
        throw std::runtime_error(label);
}

EffectPhase const phase{};

// Simulated world facts and one game effect, in the order the PB boundary uses them: map the order from fresh
// facts, ask authorize once, mutate only when granted.
struct Probe final : CommandExecutor
{
    IntentContext facts{22, 11, 33, 33, 33, 44, 44, std::nullopt};
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
    std::optional<IntentOrder> order;

    EffectResult Execute(EffectPhase const&, WorldTicket const& ticket, DispatchGuard const& guard,
                         ChannelCommand const& command, std::function<bool()> const& authorize) override
    {
        ++calls;
        controller = ticket.scope.controllerGuid;
        group = guard.groupGuid;
        if (before)
            before();
        if (early)
            return *early;
        auto const mapped = MapIntent(command, facts);
        if (!mapped)
            return EffectResult::Rejected;
        if (skipAuthorize)
            return result;
        bool const granted = authorize();
        ++(granted ? grants : denials);
        if (askTwice)
            ++(authorize() ? grants : denials);
        if (!granted || result == EffectResult::Rejected || result == EffectResult::Unavailable)
            return granted ? result : EffectResult::Rejected;
        ++mutations;
        order = mapped;
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
    Settings settings{"fixture", "codex", "http://127.0.0.1:47800", "synthetic-local-channel-token", false, "", ""};
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
        Check(world.ObserveDrain(channel.Drained(), RetirementProof::NoLiveCaptures), "real selection");
        Check(channel.Activate(), "channel opt in");
        WorldScope scope{1, "fixture", "codex", "epoch-1", 11, 22, "context", "party", "visible", "c1", "m1", "p1"};
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
    std::string Command(std::string const& id, char const* intent, Json params = Json::object()) const
    {
        auto j = Wire("command", ticket.request.requestId);
        j["commandId"] = id;
        j["issuerGuid"] = "22";
        j["botGuid"] = "11";
        j["intent"] = intent;
        j["params"] = std::move(params);
        return j.dump();
    }
    std::string Attack(std::string const& id) const { return Command(id, "attack_own_target"); }
    std::string Mark(std::string const& id, char const* target = "44") const
    {
        return Command(id, "mark", {{"targetGuid", target}, {"icon", 3}});
    }
    uint64_t Receive(std::string const& body)
    {
        uint64_t receipt = 0;
        Check(channel.Receive(settings.token, ticket.request.requestId, body, receipt) == ChannelError::None,
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
        Check(channel.Take(settings.token, receipt, packet) == ChannelError::None && packet,
              "authenticated completion operation");
        return packet;
    }
    // J5e: the last Result's refusal reason and sentence (empty when applied) and its packet error.
    std::string reason;
    std::string reasonText;
    ChannelError packetError = ChannelError::None;
    // The single correlated receipt: its outcome, a fixed error name, or "absent" when withheld or disposed.
    std::string Result(uint64_t receipt, std::string const& id)
    {
        reason.clear();
        reasonText.clear();
        packetError = ChannelError::None;
        std::unique_ptr<ChannelPacket> packet;
        auto const error = channel.Take(settings.token, receipt, packet);
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
        Check(body.size() == (refused ? 11u : 9u) && body["type"] == "command.result" && body["commandId"] == id &&
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
    Check(kBridgeEffectsReleased, "typed effects are on since the J6 release join (J6b4)");
    unsigned supported = 0;
    for (int intent = 0; intent <= static_cast<int>(ChannelIntent::DungeonClear); ++intent)
        supported += EffectSupported(static_cast<ChannelIntent>(intent)) ? 1 : 0;
    Check(supported == 9 && EffectSupported(ChannelIntent::AttackOwnTarget) && EffectSupported(ChannelIntent::Mark) &&
              EffectSupported(ChannelIntent::Follow) && EffectSupported(ChannelIntent::Stay) &&
              EffectSupported(ChannelIntent::Flee) && EffectSupported(ChannelIntent::Position) &&
              EffectSupported(ChannelIntent::Formation) && EffectSupported(ChannelIntent::SaveMana) &&
              EffectSupported(ChannelIntent::FocusHealing),
          "only the nine PB-backed verbs reach an executor (J12 adds the three lasting settings)");
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

// The production adapter's pure order step: fresh facts in, playerbots order fields out; nothing wire-authored.
void EffectOrders()
{
    IntentContext const facts{22, 11, 33, 33, 33, 44, 44, std::nullopt};
    ChannelCommand const attack{"a", 22, 11, ChannelIntent::AttackOwnTarget, std::monostate{}};
    ChannelCommand const mark{"m", 22, 11, ChannelIntent::Mark, ChannelMark{44, 7}};
    Check(EffectTargetGuid(attack, 44) == 44 && EffectTargetGuid(attack, 0) == 0 && EffectTargetGuid(mark, 99) == 44,
          "an attack resolves the bot's own target, a mark its payload target");
    auto const attacking = BuildEffectOrder(attack, facts);
    Check(attacking && attacking->intent == ChannelIntent::AttackOwnTarget && attacking->botGuid == 11 &&
              attacking->controllerGuid == 22 && attacking->groupGuid == 33 && attacking->targetGuid == 44 &&
              attacking->icon == 0,
          "attack order from fresh facts and the captured group");
    auto const marking = BuildEffectOrder(mark, facts);
    Check(marking && marking->intent == ChannelIntent::Mark && marking->botGuid == 11 &&
              marking->controllerGuid == 22 && marking->groupGuid == 33 && marking->targetGuid == 44 &&
              marking->icon == 7,
          "mark order carries its target and icon");

    IntentContext unseen = facts;
    unseen.eligibleVisibleTargetGuid = 0;
    Check(!BuildEffectOrder(attack, unseen) && !BuildEffectOrder(mark, unseen), "an unseen target builds no order");
    IntentContext retargeted = facts;
    retargeted.ownTargetGuid = 45;
    Check(!BuildEffectOrder(attack, retargeted), "an attack needs its own target to be the eligible one");
    IntentContext elsewhere = facts;
    elsewhere.eligibleVisibleTargetGuid = 45;
    Check(!BuildEffectOrder(mark, elsewhere), "a mark needs its payload target to be the eligible one");
    IntentContext moved = facts;
    moved.botGroupGuid = 34;
    Check(!BuildEffectOrder(attack, moved) && !BuildEffectOrder(mark, moved), "a changed group builds no order");
    IntentContext ungrouped = facts;
    ungrouped.expectedGroupGuid = ungrouped.controllerGroupGuid = ungrouped.botGroupGuid = 0;
    auto const alone = BuildEffectOrder(attack, ungrouped);
    Check(alone && alone->groupGuid == 0 && alone->targetGuid == 44,
          "DS.14: an ungrouped bot attacks its own target with no group");
    Check(!BuildEffectOrder(mark, ungrouped), "DS.14: an ungrouped bot sets no group mark");
    IntentContext outside = facts;
    outside.controllerGroupGuid = 0;
    auto const markedOutside = BuildEffectOrder(mark, outside);
    Check(markedOutside && markedOutside->groupGuid == 33,
          "DS.14: a partner outside the bot's group may still ask for a mark in the bot's group");
    ChannelCommand foreign = attack;
    foreign.issuerGuid = 23;
    Check(!BuildEffectOrder(foreign, facts), "another issuer builds no order");
    ChannelCommand wired = attack;
    wired.params = ChannelTarget{44};
    Check(!BuildEffectOrder(wired, facts), "an attack never takes a wire target");
    ChannelCommand icon = mark;
    icon.params = ChannelMark{44, 8};
    Check(!BuildEffectOrder(icon, facts), "an out-of-range icon builds no order");

    for (ChannelIntent const intent : {ChannelIntent::Follow, ChannelIntent::Stay, ChannelIntent::Flee})
    {
        ChannelCommand const oneShot{"o", 22, 11, intent, std::monostate{}};
        auto const ordered = BuildEffectOrder(oneShot, facts);
        Check(ordered && ordered->intent == intent && ordered->botGuid == 11 && ordered->controllerGuid == 22 &&
                  ordered->groupGuid == 33 && ordered->targetGuid == 0 && ordered->icon == 0 &&
                  ordered->position == ChannelPosition{},
              "a target-less one-shot order carries the captured group, no target and no position");
        Check(EffectTargetGuid(oneShot, 44) == 0, "a target-less one-shot resolves no unit");
        auto const unseenOrder = BuildEffectOrder(oneShot, unseen);
        auto const retargetedOrder = BuildEffectOrder(oneShot, retargeted);
        Check(unseenOrder && !unseenOrder->targetGuid && retargetedOrder && !retargetedOrder->targetGuid,
              "own or eligible target facts never enter a target-less order");
        Check(!BuildEffectOrder(oneShot, moved), "a one-shot needs the bot's group unchanged since the capture");
        auto const lone = BuildEffectOrder(oneShot, ungrouped);
        Check(intent == ChannelIntent::Follow ? !lone : lone && lone->groupGuid == 0,
              "DS.14: an ungrouped bot stays or flees on its own; it follows nobody");
        auto const away = BuildEffectOrder(oneShot, outside);
        Check(intent == ChannelIntent::Follow ? !away : away && away->groupGuid == 33,
              "DS.14: a partner outside the bot's group gets a stay or a flee, not a follow");
        ChannelCommand other = oneShot;
        other.issuerGuid = 23;
        Check(!BuildEffectOrder(other, facts), "another issuer builds no one-shot");
        other = oneShot;
        other.botGuid = 12;
        Check(!BuildEffectOrder(other, facts), "another bot builds no one-shot");
        other = oneShot;
        other.params = ChannelTarget{44};
        Check(!BuildEffectOrder(other, facts), "a one-shot never takes a wire target");
        other.params = ChannelMark{44, 3};
        Check(!BuildEffectOrder(other, facts), "a one-shot never takes a wire mark");
        other.params = ChannelPosition{1.5, -2.25, 3.0, 1};
        IntentContext pathable = facts;
        pathable.pathableNearbyPosition = ChannelPosition{1.5, -2.25, 3.0, 1};
        Check(!BuildEffectOrder(other, pathable), "a one-shot never takes a wire position");
    }

    ChannelPosition const spot{1.5, -2.25, 3.0, 1};
    ChannelCommand const position{"x", 22, 11, ChannelIntent::Position, spot};
    Check(!MapIntent(position, facts) && !BuildEffectOrder(position, facts),
          "a position without the pathable fact builds no order");
    IntentContext pathable = facts;
    pathable.pathableNearbyPosition = spot;
    auto const moving = BuildEffectOrder(position, pathable);
    Check(moving && moving->intent == ChannelIntent::Position && moving->botGuid == 11 &&
              moving->controllerGuid == 22 && moving->groupGuid == 33 && moving->targetGuid == 0 && moving->icon == 0 &&
              moving->position == spot,
          "a position order copies the checked coordinates and map");
    Check(EffectTargetGuid(position, 44) == 0, "a position resolves no unit");
    for (ChannelPosition const checked : {ChannelPosition{1.5, -2.25, 3.0, 2}, ChannelPosition{1.5, -2.25, 3.5, 1},
                                          ChannelPosition{-2.25, 1.5, 3.0, 1}})
    {
        IntentContext checkedElsewhere = facts;
        checkedElsewhere.pathableNearbyPosition = checked;
        Check(!BuildEffectOrder(position, checkedElsewhere), "a position other than the checked one builds no order");
    }
    IntentContext pathableMoved = pathable;
    pathableMoved.botGroupGuid = 34;
    IntentContext pathableUngrouped = pathable;
    pathableUngrouped.expectedGroupGuid = pathableUngrouped.controllerGroupGuid = pathableUngrouped.botGroupGuid = 0;
    auto const movingAlone = BuildEffectOrder(position, pathableUngrouped);
    Check(!BuildEffectOrder(position, pathableMoved) && movingAlone && movingAlone->groupGuid == 0 &&
              movingAlone->position == spot,
          "a position needs the bot's group unchanged; an ungrouped bot moves with no group");
    ChannelCommand stray = position;
    stray.issuerGuid = 23;
    Check(!BuildEffectOrder(stray, pathable), "another issuer builds no position");
    stray = position;
    stray.botGuid = 12;
    Check(!BuildEffectOrder(stray, pathable), "another bot builds no position");
    stray = position;
    stray.params = ChannelTarget{44};
    Check(!BuildEffectOrder(stray, pathable), "a position never takes a wire target");
    stray.params = std::monostate{};
    Check(!BuildEffectOrder(stray, pathable), "a position needs its coordinates");
    ChannelPosition const unbounded{std::numeric_limits<double>::infinity(), -2.25, 3.0, 1};
    stray.params = unbounded;
    IntentContext infinite = facts;
    infinite.pathableNearbyPosition = unbounded;
    Check(!BuildEffectOrder(stray, infinite), "a non-finite position never maps, even as a matching fact");

    ChannelCommand const pull{"p", 22, 11, ChannelIntent::Pull, ChannelTarget{44}};
    Check(MapIntent(pull, facts) && !BuildEffectOrder(pull, facts) && EffectTargetGuid(pull, 44) == 0,
          "a mappable verb without an accepted effect builds no order");
    unsigned orders = 0;
    for (int intent = 0; intent <= static_cast<int>(ChannelIntent::DungeonClear); ++intent)
    {
        ChannelCommand other = attack;
        other.intent = static_cast<ChannelIntent>(intent);
        if (!EffectSupported(other.intent))
            orders += (BuildEffectOrder(other, facts) || EffectTargetGuid(other, 44)) ? 1 : 0;
    }
    Check(orders == 0, "no other verb resolves a target or builds an order");
}

void TypedVerbs()
{
    Fixture f;
    auto receipt = f.Send(f.Attack("attack-1"));
    Check(f.probe.calls == 1 && f.probe.grants == 1 && f.probe.mutations == 1, "one authorized attack");
    Check(f.probe.controller == 22 && f.probe.group == 33, "executor gets the captured route and guard");
    auto const* target = std::get_if<ChannelTarget>(&f.probe.order->params);
    Check(f.probe.order->intent == ChannelIntent::AttackOwnTarget && target && target->guid == 44,
          "typed own-target order from fresh facts");
    Check(f.Result(receipt, "attack-1") == "applied", "attack receipt");
    f.Advance(CommandRate::IntervalMs);
    receipt = f.Send(f.Mark("mark-1"));
    auto const* mark = std::get_if<ChannelMark>(&f.probe.order->params);
    Check(f.probe.calls == 2 && f.probe.mutations == 2 && f.probe.order->intent == ChannelIntent::Mark && mark &&
              mark->guid == 44 && mark->icon == 3,
          "typed mark order");
    Check(f.Result(receipt, "mark-1") == "applied", "mark receipt");
    unsigned calls = 2;
    std::pair<char const*, ChannelIntent> const oneShots[] = {
        {"follow", ChannelIntent::Follow}, {"stay", ChannelIntent::Stay}, {"flee", ChannelIntent::Flee}};
    for (auto const& [intent, typed] : oneShots)
    {
        f.Advance(CommandRate::IntervalMs);
        receipt = f.Send(f.Command(intent, intent));
        ++calls;
        Check(f.probe.calls == calls && f.probe.grants == calls && f.probe.mutations == calls &&
                  f.probe.order->intent == typed && std::holds_alternative<std::monostate>(f.probe.order->params),
              "one authorized target-less one-shot order");
        Check(f.Result(receipt, intent) == "applied", "one-shot receipt");
    }
    Json const spot{{"x", 1.5}, {"y", -2.25}, {"z", 3.0}, {"mapId", 1}};
    f.Advance(CommandRate::IntervalMs);
    f.probe.facts.pathableNearbyPosition = ChannelPosition{1.5, -2.25, 3.0, 1};
    receipt = f.Send(f.Command("position-1", "position", spot));
    auto const* position = std::get_if<ChannelPosition>(&f.probe.order->params);
    Check(f.probe.calls == 6 && f.probe.grants == 6 && f.probe.mutations == 6 &&
              f.probe.order->intent == ChannelIntent::Position && position &&
              *position == ChannelPosition{1.5, -2.25, 3.0, 1},
          "one authorized position order with the exact checked coordinates");
    Check(f.Result(receipt, "position-1") == "applied", "position receipt");
    auto const& stats = f.channel.Commands();
    Check(stats.proposed == 6 && stats.attempts == 6 && stats.applied == 6 && !stats.refused && !stats.failed &&
              !stats.withheld && !stats.duplicates && f.channel.Drained(),
          "watchlist counters and slots");

    // Without the adapter's pathable fact, or with a different checked position, the order never maps.
    Fixture g;
    Check(g.Result(g.Send(g.Command("unpathable", "position", spot)), "unpathable") == "rejected" &&
              g.probe.calls == 1 && !g.probe.grants && !g.probe.mutations,
          "an unpathable position is refused before authorization");
    g.Advance(CommandRate::IntervalMs);
    g.probe.facts.pathableNearbyPosition = ChannelPosition{1.5, -2.25, 4.0, 1};
    Check(g.Result(g.Send(g.Command("elsewhere", "position", spot)), "elsewhere") == "rejected" && g.probe.calls == 2 &&
              !g.probe.grants && !g.probe.mutations,
          "a position other than the checked one is refused before authorization");
    Check(g.channel.Commands().attempts == 2 && g.channel.Commands().refused == 2 && !g.channel.Commands().applied,
          "refused positions are counted attempts without an effect");
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
    Fixture f;
    // J12: focus_healing, save_mana and formation are lasting plans now; the four without a playerbots effect stay
    // refused (DS.9).
    std::pair<char const*, Json> const others[] = {{"regroup", Json::object()},
                                                   {"pull", Json{{"targetGuid", "44"}}},
                                                   {"assign_role", Json{{"role", "tank"}}},
                                                   {"dungeon_clear", Json{{"mode", "on"}}}};
    unsigned refused = 0;
    for (auto const& [intent, params] : others)
    {
        auto const id = "refused-" + std::to_string(refused++);
        Check(f.Result(f.Send(f.Command(id, intent, params)), id) == "rejected", "fixed refusal receipt");
    }
    Check(refused == 4 && f.probe.calls == 0 && f.channel.Commands().refused == 4 && !f.channel.Commands().attempts,
          "all four other verbs refused with no executor call");
    Check(f.Result(f.Send(f.Attack("after-refusals")), "after-refusals") == "applied" && f.probe.calls == 1,
          "refusals spend no rate interval");
    Fixture lasting;
    lasting.Send(lasting.Command("lasting", "formation", Json{{"shape", "close"}}));
    Check(lasting.probe.calls == 1 && lasting.channel.Commands().attempts == 1,
          "a lasting setting reaches the executor");

    Fixture closed({.executor = false});
    Check(closed.Result(closed.Send(closed.Attack("no-executor")), "no-executor") == "rejected" &&
              !closed.channel.Commands().attempts,
          "null executor keeps today's refusal");
    std::pair<char const*, Json> const oneShots[] = {
        {"follow", Json::object()},
        {"stay", Json::object()},
        {"flee", Json::object()},
        {"position", Json{{"x", 1.0}, {"y", 2.0}, {"z", 3.0}, {"mapId", 1}}}};
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

// The J6 action matrix: each of the 13 wire verbs ends as exactly one of a delivered effect (one executor call, one
// authorized mutation, an applied receipt, the bot's rate interval spent) or its fixed refusal (a rejected receipt, no
// executor call, nothing attempted, no rate interval spent). A fresh channel per verb keeps the rows independent.
void VerbMatrix()
{
    struct Row
    {
        char const* intent;
        Json params;
        bool delivered;
    };
    Json const spot{{"x", 1.5}, {"y", -2.25}, {"z", 3.0}, {"mapId", 1}};
    Row const rows[] = {{"attack_own_target", Json::object(), true},
                        {"mark", Json{{"targetGuid", "44"}, {"icon", 3}}, true},
                        {"follow", Json::object(), true},
                        {"stay", Json::object(), true},
                        {"flee", Json::object(), true},
                        {"position", spot, true},
                        {"pull", Json{{"targetGuid", "44"}}, false},
                        {"regroup", Json::object(), false},
                        {"formation", Json{{"shape", "close"}}, true},
                        {"save_mana", Json{{"enabled", true}}, true},
                        {"focus_healing", Json{{"targetGuid", "44"}}, true},
                        {"assign_role", Json{{"role", "tank"}}, false},
                        {"dungeon_clear", Json{{"mode", "on"}}, false}};
    Check(std::size(rows) == static_cast<size_t>(ChannelIntent::DungeonClear) + 1, "the matrix names every wire verb");
    unsigned delivered = 0;
    unsigned refusals = 0;
    for (auto const& row : rows)
    {
        Fixture f;
        f.probe.facts.pathableNearbyPosition = ChannelPosition{1.5, -2.25, 3.0, 1};
        Check(f.Result(f.Send(f.Command("matrix", row.intent, row.params)), "matrix") ==
                  (row.delivered ? "applied" : "rejected"),
              "matrix receipt");
        Check(row.delivered ? f.reason.empty() : f.reason == "not_supported",
              "a fixed refusal names its reason (not_supported)");
        auto const& stats = f.channel.Commands();
        if (row.delivered)
        {
            ++delivered;
            Check(f.probe.calls == 1 && f.probe.grants == 1 && f.probe.mutations == 1 && stats.attempts == 1 &&
                      stats.applied == 1 && !stats.refused,
                  "a delivered verb reaches the executor once with one authorized mutation");
            Check(f.Result(f.Send(f.Attack("after-delivered")), "after-delivered") == "rejected" && f.probe.calls == 1,
                  "a delivered verb spends the bot's rate interval");
            Check(f.reason == "too_soon", "a rate refusal names its reason (too_soon)");
        }
        else
        {
            ++refusals;
            Check(f.probe.calls == 0 && f.probe.grants == 0 && f.probe.mutations == 0 && !stats.attempts &&
                      stats.refused == 1 && !stats.applied && !stats.failed,
                  "a fixed refusal never reaches the executor and is not an attempt");
            Check(f.Result(f.Send(f.Attack("after-refusal")), "after-refusal") == "applied" && f.probe.calls == 1,
                  "a fixed refusal spends no rate interval");
        }
    }
    Check(delivered == 9 && refusals == 4, "nine delivered verbs and four fixed refusals");
    // Standing follow and stay (a duration or a standing flag) have no wire shape: the exact parameter check refuses
    // the packet as invalid input before any command exists, so nothing is admitted, attempted or executed.
    // J5e: the refusal is the Standing error, which the listener words as invalid_input with reason
    // standing_not_supported.
    for (char const* intent : {"follow", "stay"})
        for (Json const& extra : {Json{{"standing", true}}, Json{{"durationMs", 30000}}})
        {
            Fixture f;
            Check(f.Result(f.Send(f.Command("standing", intent, extra)), "standing") == "invalid_input" &&
                      f.packetError == ChannelError::Standing,
                  "a standing follow or stay has no typed shape and names its reason");
            Check(f.probe.calls == 0 && !f.channel.Commands().proposed && !f.channel.Commands().attempts,
                  "a standing verb never reaches the executor");
            Check(f.Result(f.Send(f.Attack("after-standing")), "after-standing") == "applied",
                  "a refused standing verb spends no rate interval");
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
    Check(f.Result(f.Send(result.dump()), "") == "unknown_request", "generation result closes the request");
    f.Advance(CommandRate::IntervalMs);
    Check(f.Result(f.Send(f.Attack("after-terminal")), "after-terminal") == "closed" && f.probe.calls == 2,
          "a command after its request is terminal is closed before reservation or effect");
    Check(
        f.Result(f.Send(f.Attack("after-terminal")), "after-terminal") == "closed" && !f.channel.Commands().duplicates,
        "the closed command consumed no id");

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
    Check(f.channel.Drained() && f.world.ObserveDrain(f.channel.Drained(), RetirementProof::NoLiveCaptures) &&
              f.channel.Activate(),
          "retirement disposed the queued command; new epoch selected");
    Check(f.Result(id, "retired") == "absent", "no receipt from a retired epoch");
    uint64_t again = 0;
    Check(f.channel.Receive(f.settings.token, f.ticket.request.requestId, f.Attack("retired"), again) ==
                  ChannelError::None &&
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
    Check(f.channel.Receive(f.settings.token, f.ticket.request.requestId, f.Attack("burst-over"), overflow) ==
                  ChannelError::Capacity &&
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
    // Fresh facts that do not map to the order: refused before the final check, no authorization asked.
    Case const unmapped[] = {
        {"other controller", [](Fixture& f) { f.probe.facts.controllerGuid = 23; }},
        {"bot left the group", [](Fixture& f) { f.probe.facts.botGroupGuid = 34; }},
        {"own target changed", [](Fixture& f) { f.probe.facts.ownTargetGuid = 45; }},
        {"no eligible target", [](Fixture& f) { f.probe.facts.eligibleVisibleTargetGuid = 0; }},
    };
    for (auto const& c : unmapped)
    {
        Fixture f;
        c.change(f);
        Check(f.Result(f.Send(f.Attack("unmapped")), "unmapped") == "rejected" && f.probe.calls == 1 &&
                  !f.probe.grants && !f.probe.denials && !f.probe.mutations,
              c.label);
    }
    Fixture m;
    Check(m.Result(m.Send(m.Mark("invisible", "45")), "invisible") == "rejected" && m.probe.calls == 1 &&
              !m.probe.mutations,
          "mark of an ineligible target");

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
        {"channel retired inside the executor", [](Fixture& f) { f.channel.Retire(); }, "unauthorized", false},
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

void TransportLoss()
{
    // Outcome decided, then the receipt is lost in transport: nothing re-executes.
    Fixture f;
    auto const lost = f.Send(f.Attack("lost-receipt"));
    f.Advance(WorldLivenessMs);
    std::unique_ptr<ChannelPacket> packet;
    Check(f.channel.Take(f.settings.token, lost, packet) == ChannelError::Stale && !packet && f.channel.Drained(),
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

int main()
{
    try
    {
        PureSeam();
        EffectOrders();
        TypedVerbs();
        OutcomesNeverReplay();
        FixedRefusals();
        VerbMatrix();
        TerminalAndIds();
        RetirementBeforeTick();
        QueueAndRate();
        LostGuards();
        RateAndClock();
        PostEffectJoin();
        TransportLoss();
        std::cout << "PASS " << checks << " typed command dispatch checks\n";
    }
    catch (std::exception const& e)
    {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
