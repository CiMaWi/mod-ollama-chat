// The production typed-effect unit (src/mod-ollama-chat_bridge_effect.cpp) is compiled whole, with playerbots' own
// order types (BridgeIntentEffect.h, BridgeAbilities.h), against game doubles and doubles of playerbots' executor
// boundary that count every read and every call. The world phase is built as in production: by the world tick.
// LCB2: every command is stepped, so every case runs through Execute as WorldChannel calls it. J12: the executor's
// plan ledger (the real PlanLedger) with Capture/Restore doubles that model each bot's lasting parts; the world tick's
// expiry, group, controller and logout checks and restores. No game, no playerbots, no server.
#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "BridgeAbilities.h"
#include "BridgeIntentEffect.h"
#include "mod-ollama-chat_bridge_plan.h"
#include "mod-ollama-chat_bridge_world.h"

// ---- game doubles -------------------------------------------------------------------------------------------------
struct ObjectGuid
{
    ObjectGuid() = default;
    explicit ObjectGuid(uint64_t raw) : _raw(raw) {}
    uint64_t GetRawValue() const { return _raw; }

private:
    uint64_t _raw = 0;
};

struct Unit
{
    bool inWorld = true;
    bool IsInWorld() const { return inWorld; }
};

struct Group
{
    uint64_t guid = 0;
    uint64_t leader = 0;
    ObjectGuid GetGUID() const { return ObjectGuid(guid); }
    ObjectGuid GetLeaderGUID() const { return ObjectGuid(leader); }
};

struct Player;
// The bot's playerbots AI and its master: the unit has no way to ask for them, and the plan tick's cases change them
// to show that who leads the bot plays no part.
struct PlayerbotAI
{
    Player* master = nullptr;
};

struct Counters
{
    unsigned playerLookups = 0;
    unsigned unitLookups = 0;
    unsigned targetReads = 0;
    unsigned groupReads = 0;
};
Counters game;

struct Player : Unit
{
    Group* group = nullptr;
    uint64_t target = 0;
    PlayerbotAI* ai = nullptr;
    Group* GetGroup()
    {
        ++game.groupReads;
        return group;
    }
    ObjectGuid GetTarget() const
    {
        ++game.targetReads;
        return ObjectGuid(target);
    }
};

struct ObjectAccessor
{
    static std::map<uint64_t, Player*>& Players()
    {
        static std::map<uint64_t, Player*> value;
        return value;
    }
    static std::map<uint64_t, Unit*>& Units()
    {
        static std::map<uint64_t, Unit*> value;
        return value;
    }
    static Player* FindConnectedPlayer(ObjectGuid guid)
    {
        ++game.playerLookups;
        auto const found = Players().find(guid.GetRawValue());
        return found == Players().end() ? nullptr : found->second;
    }
    static Unit* GetUnit(Player const&, ObjectGuid guid)
    {
        ++game.unitLookups;
        auto const found = Units().find(guid.GetRawValue());
        return found == Units().end() ? nullptr : found->second;
    }
};

// ---- playerbots doubles: the executor boundary the unit calls (declared in the two headers above) ----------------
namespace PlayerbotBridge
{
struct Doubles
{
    bool current = true;  // What PostMapJoin::Current answers.
    unsigned executes = 0;
    unsigned reaches = 0;
    unsigned authorizeCalled = 0;
    bool authorizeResult = false;
    bool reachable = false;
    Result result = Result::Applied;
    Refusal refusal = Refusal::None;  // DS.14: what Execute reports through its out-param.
    Order order;
    Order probe;
    PostMapJoin const* phase = nullptr;
    PostMapJoin const* reachPhase = nullptr;
    // J12: each bot's lasting parts as playerbots holds them (Bot and Held are filled on every copy).
    std::map<uint64_t, std::map<Part, Baseline>> state;
    unsigned captures = 0;
    unsigned capturesAtExecute = 0;  // Captures made before the latest Execute.
    bool captureFails = false;
    bool captureFailsAfterExecute = false;  // Execute sets captureFails (the bot left the world inside the call).
    std::function<void()> duringExecute;    // Runs inside Execute (e.g. the ledger changes under the attempt).
    bool writes = true;                     // Applied and Failed write the order's part (Failed: a partial write).
    int64_t* clock = nullptr;               // Execute advances *clock by executeAdvanceMs (a slow attempt).
    int64_t executeAdvanceMs = 0;
    unsigned restores = 0;
    std::optional<RestoreResult> restoreResult;  // Forced Restore outcome, else the real rule below.
    uint64_t restoredBot = 0;
    Baseline restoreBefore;
    Baseline restoreApplied;
    // LC3b: the span Execute of a stepped order.
    unsigned spanExecutes = 0;
    std::vector<Order> steps;               // The orders of the latest span Execute.
    std::vector<StepOutcome> stepOutcomes;  // Per step; a step past the list applies.
    std::optional<Result> spanEarly;        // Refused before authorize (a row-less verb, a malformed step).
    Refusal wordsFor = Refusal::None;       // The latest RefusalWords argument.
};
Doubles pb;

PostMapJoin::PostMapJoin() : _thread(std::this_thread::get_id()) {}
bool PostMapJoin::Current() const { return pb.current; }

Baseline HeldCopy(uint64_t bot, Part part)
{
    Baseline value = pb.state[bot][part];
    value.Bot = bot;
    value.Held = part;
    return value;
}

// The table's Lasting column (BridgeAbilities.cpp): this double repeats its five.
Part LastingPart(Intent operation)
{
    switch (operation)
    {
        case Intent::Follow:
        case Intent::Stay:
            return Part::Movement;
        case Intent::Formation:
            return Part::Formation;
        case Intent::SaveMana:
            return Part::ManaSave;
        case Intent::FocusHealing:
            return Part::FocusHeal;
        default:
            return Part::None;
    }
}

void HeldWrite(Order const& order)
{
    Part const part = LastingPart(order.Operation);
    if (part == Part::None)
        return;
    Baseline& value = pb.state[order.Bot][part];
    switch (order.Operation)
    {
        case Intent::Follow:
            value.Strategies = 1;
            break;
        case Intent::Stay:
            value.Strategies = 2;
            break;
        case Intent::Formation:
            value.Formation = "shape" + std::to_string(static_cast<int>(order.Shape));
            break;
        case Intent::SaveMana:
            value.ManaSaveLevel = order.Enabled ? 0.5 : 1.0;
            break;
        default:
            value.FocusHealTargets = {order.Target};
            value.FocusHealStrategy = true;
            break;
    }
}

// The one-order Execute: the stepped executor must never call it (the cases check executes == 0).
Result Execute(PostMapJoin const& phase, Order const& order, std::function<bool()> const& authorize, Refusal* why)
{
    ++pb.executes;
    if (why)
        *why = pb.refusal;
    pb.capturesAtExecute = pb.captures;
    pb.order = order;
    pb.phase = &phase;
    ++pb.authorizeCalled;
    pb.authorizeResult = authorize();
    if (pb.writes && (pb.result == Result::Applied || pb.result == Result::Failed))
        HeldWrite(order);
    return pb.result;
}

// LC3b: one order of steps, one authorize; each step's configured outcome, the first not Applied ends the order.
Result Execute(PostMapJoin const& phase, std::span<Order const> steps, std::function<bool()> const& authorize,
               std::vector<StepOutcome>* outcomes)
{
    ++pb.spanExecutes;
    pb.capturesAtExecute = pb.captures;
    pb.steps.assign(steps.begin(), steps.end());
    pb.phase = &phase;
    if (outcomes)
        outcomes->clear();
    if (pb.spanEarly)
        return *pb.spanEarly;
    ++pb.authorizeCalled;
    pb.authorizeResult = authorize();
    if (pb.clock)
        *pb.clock += pb.executeAdvanceMs;
    Result result = Result::Applied;
    for (std::size_t index = 0; index < steps.size() && result == Result::Applied; ++index)
    {
        StepOutcome const outcome =
            index < pb.stepOutcomes.size() ? pb.stepOutcomes[index] : StepOutcome{Result::Applied, Refusal::None};
        if (outcomes)
            outcomes->push_back(outcome);
        if (pb.writes && (outcome.Outcome == Result::Applied || outcome.Outcome == Result::Failed))
            HeldWrite(steps[index]);
        result = outcome.Outcome;
    }
    if (pb.duringExecute)
        pb.duringExecute();
    if (pb.captureFailsAfterExecute)
        pb.captureFails = true;
    return result;
}

RefusalText const& RefusalWords(Refusal reason)
{
    static RefusalText words{Refusal::None, "pb_word", "A playerbots sentence."};
    pb.wordsFor = reason;
    words.Code = reason;
    return words;
}

bool Capture(PostMapJoin const& phase, uint64_t bot, Part part, Baseline& out)
{
    ++pb.captures;
    out = Baseline();
    if (!phase.Current() || pb.captureFails || part == Part::None)
        return false;
    out = HeldCopy(bot, part);
    return true;
}

RestoreResult Restore(PostMapJoin const& phase, uint64_t bot, Baseline const& before, Baseline const& applied)
{
    ++pb.restores;
    pb.restoredBot = bot;
    pb.restoreBefore = before;
    pb.restoreApplied = applied;
    if (!phase.Current() || before.Held == Part::None || before.Held != applied.Held || before.Bot != bot ||
        applied.Bot != bot)
        return RestoreResult::Rejected;
    if (pb.restoreResult)
        return *pb.restoreResult;
    Baseline const current = HeldCopy(bot, before.Held);
    if (current == before)
        return RestoreResult::Restored;
    if (current != applied)
        return RestoreResult::Superseded;
    Baseline written = before;
    written.Bot = 0;
    written.Held = Part::None;
    pb.state[bot][before.Held] = written;
    return RestoreResult::Restored;
}

bool PositionReachable(PostMapJoin const& phase, Order const& order)
{
    ++pb.reaches;
    pb.probe = order;
    pb.reachPhase = &phase;
    return pb.reachable;
}
}  // namespace PlayerbotBridge

namespace OllamaBridge
{
// The steady clock the executor reads once per order (bridge.cpp is not linked).
int64_t testNowMs = 1000;
Clock Now() { return {0, testNowMs}; }
}  // namespace OllamaBridge

#define OLLAMA_SELFTEST_GAME_DOUBLES
#include "../src/mod-ollama-chat_bridge_effect.cpp"

namespace
{
using namespace OllamaBridge;
// The world phase of the running tick: only ::OllamaDispatch_Update (below) can build one.
EffectPhase const* worldPhase = nullptr;
unsigned checks = 0;
void Check(bool value, char const* label)
{
    ++checks;
    if (!value)
        throw std::runtime_error(label);
}

constexpr uint64_t kBot = 11;
constexpr uint64_t kController = 22;
constexpr uint64_t kGroup = 33;
constexpr uint64_t kTarget = 44;
constexpr uint64_t kOther = 55;
constexpr uint64_t kStranger = 66;
constexpr uint64_t kGeneration = 7;
using PlayerbotBridge::Part;

// The executor's one ledger, fresh (its capacity is const, so rebuilt in place).
void ResetPlans()
{
    BridgePlans& plans = BridgeCommandExecutor::Plans();
    plans.~BridgePlans();
    new (&plans) BridgePlans();
}

struct Env
{
    Player bot;
    Player controller;
    Player stranger;
    PlayerbotAI botAI;
    Group botGroup;
    Group controllerGroup;
    Unit target;
    Unit other;
    EffectPhase const& phase = *worldPhase;
    WorldTicket ticket;
    DispatchGuard guard;
    unsigned authorizeCalls = 0;
    CommandRefusal why = CommandRefusal::None;  // The last Run's refusal reason.
    std::vector<ChannelStepResult> ran;         // LC3b: the last Run's step results.

    Env()
    {
        game = Counters{};
        PlayerbotBridge::pb = PlayerbotBridge::Doubles{};
        botGroup.guid = controllerGroup.guid = kGroup;
        botGroup.leader = controllerGroup.leader = kController;
        bot.group = &botGroup;
        controller.group = &controllerGroup;
        bot.target = kTarget;
        bot.ai = &botAI;
        botAI.master = &controller;
        ObjectAccessor::Players() = {{kBot, &bot}, {kController, &controller}, {kStranger, &stranger}};
        ObjectAccessor::Units() = {{kTarget, &target}, {kOther, &other}};
        ticket.scope.botGuid = kBot;
        ticket.scope.controllerGuid = kController;
        ticket.generation = kGeneration;
        guard.groupGuid = kGroup;
        testNowMs = 1000;
        PlayerbotBridge::pb.clock = &testNowMs;
        ResetPlans();
        BridgeCommandExecutor::Plans().Open(kGeneration);
    }

    std::function<bool()> Authorize()
    {
        return [this]
        {
            ++authorizeCalls;
            return true;
        };
    }

    // Through the executor's own entry, as WorldChannel calls it: its refusal and its step results.
    EffectResult Run(ChannelCommand const& command)
    {
        BridgeCommandExecutor executor;
        auto const result = executor.Execute(phase, ticket, guard, command, Authorize());
        why = executor.Refusal();
        ran = executor.Steps();
        return result;
    }
};

ChannelPosition Pos() { return ChannelPosition{100.5, 200.25, 300.125, 1}; }

using PlayerbotBridge::Intent;

ChannelStep Step(Intent intent)
{
    ChannelStep step;
    step.intent = intent;
    return step;
}

ChannelCommand Steps(std::vector<ChannelStep> steps)
{
    ChannelCommand command;
    command.commandId = "c1";
    command.issuerGuid = kController;
    command.botGuid = kBot;
    command.steps = std::move(steps);
    return command;
}

// One step of the act, with the fields it takes.
ChannelCommand ValidCmd(Intent intent)
{
    auto step = Step(intent);
    switch (intent)
    {
        case Intent::FocusHealing:
            step.targetGuid = kTarget;
            break;
        case Intent::Mark:
            step.targetGuid = kTarget;
            step.icon = 3;
            break;
        case Intent::SaveMana:
            step.enabled = true;
            break;
        case Intent::Formation:
            step.shape = 0;
            break;
        case Intent::Position:
            step.position = Pos();
            break;
        case Intent::GoTo:
        case Intent::MeetAt:
            step.place = "Goldshire";
            break;
        case Intent::Quest:
            step.questId = 4242;
            break;
        default:
            break;
    }
    return Steps({step});
}

// The acts of the old one-order path, each sent as a one-step order.
Intent const kActs[] = {
    Intent::AttackOwnTarget, Intent::Mark,      Intent::Follow,    Intent::Stay,         Intent::Flee,
    Intent::Position,        Intent::Formation, Intent::SaveMana,  Intent::FocusHealing, Intent::GoTo,
    Intent::Quest,           Intent::Errands,   Intent::SetHearth, Intent::GrindHere,    Intent::GatherHere,
    Intent::MeetAt,          Intent::PausePlan, Intent::ResumePlan};

// Nothing reached playerbots and no game read happened.
bool Untouched()
{
    auto const& p = PlayerbotBridge::pb;
    return p.spanExecutes == 0 && p.reaches == 0 && game.playerLookups == 0 && game.unitLookups == 0 &&
           game.targetReads == 0 && game.groupReads == 0;
}

void Gates()
{
    {
        Env env;
        PlayerbotBridge::pb.current = false;
        Check(env.Run(ValidCmd(Intent::Follow)) == EffectResult::Rejected && Untouched(),
              "a phase that is no longer current reads nothing");
    }
    {
        Env env;
        Check(env.Run(ValidCmd(Intent::Follow)) == EffectResult::Applied && PlayerbotBridge::pb.spanExecutes == 1 &&
                  env.authorizeCalls == 1,
              "Execute reaches the typed effect");
    }
    {
        Env env;
        PlayerbotBridge::pb.current = false;
        Check(env.Run(ValidCmd(Intent::Mark)) == EffectResult::Rejected,
              "a mark refused before playerbots is rejected");
    }
    for (int which = 0; which < 3; ++which)
    {
        Env env;
        if (which == 0)
            ObjectAccessor::Players().erase(kBot);
        else if (which == 1)
            ObjectAccessor::Players().erase(kController);
        else
            env.bot.inWorld = false;
        Check(env.Run(ValidCmd(Intent::Follow)) == EffectResult::Rejected && PlayerbotBridge::pb.spanExecutes == 0 &&
                  env.why == CommandRefusal::Unavailable,
              "a missing or out-of-world bot, or an offline partner, is rejected");
    }
    {
        // DS.14: the partner need only be online; playerbots checks what each verb needs of it.
        Env env;
        env.controller.inWorld = false;
        Check(env.Run(ValidCmd(Intent::Stay)) == EffectResult::Applied && PlayerbotBridge::pb.spanExecutes == 1,
              "a connected partner out of the world still gets the bot's own act");
    }
}

// DS.14: the captured group, not a re-read one, is what playerbots is given; an ungrouped bot's stay reaches it with
// group 0 and the ledger keeps it as the partner's plan. What each act needs of the group, the partner and a target is
// playerbots' gate.
void Groups()
{
    {
        Env env;
        env.guard.groupGuid = 0;
        env.bot.group = env.controller.group = nullptr;
        BridgePlans& plans = BridgeCommandExecutor::Plans();
        Check(env.Run(ValidCmd(Intent::Stay)) == EffectResult::Applied && PlayerbotBridge::pb.steps[0].Group == 0 &&
                  plans.Size() == 1 && plans.LiveBots().size() == 1 && plans.LiveBots()[0].owner.groupGuid == 0 &&
                  plans.LiveBots()[0].owner.controllerGuid == kController,
              "an ungrouped stay is applied with group 0 and recorded as the partner's plan");
    }
    {
        Env env;
        Check(env.Run(ValidCmd(Intent::Stay)) == EffectResult::Applied &&
                  PlayerbotBridge::pb.steps[0].Group == env.guard.groupGuid,
              "captured group reaches playerbots");
    }
}

// J12: a step's shape (its choice's index) and flag reach playerbots as its own values.
void LastingOrders()
{
    PlayerbotBridge::FormationShape const shapes[] = {PlayerbotBridge::FormationShape::Close,
                                                      PlayerbotBridge::FormationShape::Spread,
                                                      PlayerbotBridge::FormationShape::Line};
    for (uint8_t index = 0; index < 3; ++index)
    {
        Env env;
        auto line = Step(Intent::Formation);
        line.shape = index;
        Check(env.Run(Steps({line})) == EffectResult::Applied && PlayerbotBridge::pb.steps[0].Shape == shapes[index],
              "each shape choice maps to its playerbots shape");
    }
    {
        Env env;
        auto mana = Step(Intent::SaveMana);
        mana.enabled = false;
        Check(env.Run(Steps({mana})) == EffectResult::Applied && PlayerbotBridge::pb.spanExecutes == 1 &&
                  !PlayerbotBridge::pb.steps[0].Enabled,
              "save_mana off carries false");
    }
}

PlanKey MovementKey() { return {kBot, PlanKind::Movement}; }
uint16_t Movement() { return PlayerbotBridge::pb.state[kBot][Part::Movement].Strategies; }

// A stay plan over a person's earlier movement state 4.
void ApplyStay(Env& env)
{
    PlayerbotBridge::pb.state[kBot][Part::Movement].Strategies = 4;
    Check(env.Run(ValidCmd(Intent::Stay)) == EffectResult::Applied, "stay plan applied");
}

// J12 executor flow: admission and capture before every attempt, record after every attempt that may have written.
void PlanFlow()
{
    auto const& pb = PlayerbotBridge::pb;
    BridgePlans& plans = BridgeCommandExecutor::Plans();
    {
        Env env;
        ApplyStay(env);
        Check(pb.capturesAtExecute == 1 && pb.captures == 2, "the state is captured before the attempt and after it");
        Check(plans.Size() == 1 && plans.Counters().applied == 1 && plans.Counters().unrecorded == 0,
              "one plan recorded");
        auto const applied = plans.LiveApplied(MovementKey());
        Check(applied && applied->Strategies == 2 && applied->Held == Part::Movement && applied->Bot == kBot,
              "the ledger keeps what the bridge set, for this bot and part");
    }
    {
        Env env;
        env.ticket.generation = kGeneration + 1;
        Check(env.Run(ValidCmd(Intent::Stay)) == EffectResult::Rejected && pb.captures == 0 && pb.spanExecutes == 0 &&
                  env.authorizeCalls == 0 && plans.Empty(),
              "a plan the ledger refuses (stale generation) reads and changes nothing");
    }
    {
        Env env;
        PlayerbotBridge::pb.captureFails = true;
        Check(env.Run(ValidCmd(Intent::Formation)) == EffectResult::Rejected && pb.captures == 1 &&
                  pb.spanExecutes == 0 && env.authorizeCalls == 0 && plans.Empty(),
              "no baseline, no mutation");
    }
    for (auto const result : {PlayerbotBridge::Result::Rejected, PlayerbotBridge::Result::Unavailable})
    {
        Env env;
        PlayerbotBridge::pb.stepOutcomes = {{result, PlayerbotBridge::Refusal::None}};
        env.Run(ValidCmd(Intent::SaveMana));
        Check(pb.spanExecutes == 1 && pb.captures == 1 && plans.Empty() && plans.Counters().unrecorded == 0,
              "an attempt that promises no mutation records none");
    }
    // Review J12 NOTE B: unreachable today, but an attempt that may have written and left no entry is counted.
    for (auto const result : {PlayerbotBridge::Result::Applied, PlayerbotBridge::Result::Failed})
    {
        Env env;
        PlayerbotBridge::pb.stepOutcomes = {{result, PlayerbotBridge::Refusal::None}};
        PlayerbotBridge::pb.captureFailsAfterExecute = true;
        env.Run(ValidCmd(Intent::Formation));
        Check(pb.spanExecutes == 1 && pb.captures == 2 && plans.Empty() && plans.Counters().applied == 0 &&
                  plans.Counters().unrecorded == 1,
              "an attempt whose state cannot be read afterwards is counted as unrecorded");
    }
    {
        Env env;
        PlayerbotBridge::pb.duringExecute = [] { BridgeCommandExecutor::Plans().RetireAll(); };
        Check(env.Run(ValidCmd(Intent::SaveMana)) == EffectResult::Applied && pb.captures == 2 && plans.Empty() &&
                  plans.Counters().unrecorded == 1,
              "an attempt whose record the ledger refuses is counted as unrecorded");
    }
    {
        Env env;
        PlayerbotBridge::pb.stepOutcomes = {{PlayerbotBridge::Result::Failed, PlayerbotBridge::Refusal::None}};
        Check(env.Run(ValidCmd(Intent::SaveMana)) == EffectResult::Failed && pb.captures == 2 && plans.Size() == 1,
              "a failed attempt may have written: recorded");
    }
    {
        Env env;
        PlayerbotBridge::pb.state[kBot][Part::Movement].Strategies = 4;
        Check(env.Run(ValidCmd(Intent::Follow)) == EffectResult::Applied, "follow plan");
        testNowMs += 1000;
        PlayerbotBridge::pb.state[kBot][Part::Movement].Strategies = 9;  // A person's own command in between.
        Check(env.Run(ValidCmd(Intent::Stay)) == EffectResult::Applied && pb.captures == 4 && pb.capturesAtExecute == 3,
              "a repeat captures the state again before its attempt");
        Check(plans.Size() == 1 && plans.Counters().applied == 1 && plans.Counters().renewed == 1,
              "follow then stay renew the one movement plan");
        BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + PlanLifetimeMs);
        Check(pb.restores == 1 && pb.restoreBefore.Strategies == 9 && pb.restoreApplied.Strategies == 2 &&
                  Movement() == 9,
              "the person's state captured before the repeat is the restore target");
    }
    {
        Env env;
        ApplyStay(env);
        testNowMs += 1000;
        PlayerbotBridge::pb.captureFails = true;
        Check(env.Run(ValidCmd(Intent::Stay)) == EffectResult::Rejected && pb.spanExecutes == 1,
              "a repeat whose capture fails does not execute");
    }
    {
        Env env;
        ApplyStay(env);
        testNowMs += PlanLifetimeMs - 1;
        PlayerbotBridge::pb.executeAdvanceMs = 5;
        Check(env.Run(ValidCmd(Intent::Stay)) == EffectResult::Applied && plans.Counters().renewed == 1 &&
                  plans.LiveApplied(MovementKey()),
              "admission and record use one clock reading, so a slow renewal is still recorded");
    }
    for (auto const intent : {Intent::Flee, Intent::Mark, Intent::AttackOwnTarget})
    {
        Env env;
        Check(env.Run(ValidCmd(intent)) == EffectResult::Applied && pb.captures == 0 && plans.Empty() &&
                  plans.Counters().unrecorded == 0,
              "one-shot verbs have no plan");
    }
}

// J12 world tick through the executor: apply -> expire -> restore, group and controller changes, logout, retries,
// a later command, retirement, and one part per kind.
void PlanTick()
{
    auto const& pb = PlayerbotBridge::pb;
    BridgePlans& plans = BridgeCommandExecutor::Plans();
    {
        Env env;
        unsigned const lookups = game.playerLookups;
        BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs);
        Check(game.playerLookups == lookups && pb.restores == 0, "an empty ledger reads nothing");
    }
    {
        Env env;
        int64_t const start = testNowMs;
        ApplyStay(env);
        BridgeCommandExecutor::UpdatePlans(env.phase, start + PlanLifetimeMs - 1);
        Check(pb.restores == 0 && plans.Size() == 1 && Movement() == 2, "a live plan is not restored early");
        BridgeCommandExecutor::UpdatePlans(env.phase, start + PlanLifetimeMs);
        Check(pb.restores == 1 && pb.restoredBot == kBot && pb.restoreBefore.Strategies == 4 &&
                  pb.restoreApplied.Strategies == 2,
              "expiry restores the captured state");
        Check(Movement() == 4 && plans.Empty() && plans.Counters().expired == 1 && plans.Counters().restored == 1,
              "the baseline is back and the ledger is empty");
        BridgeCommandExecutor::UpdatePlans(env.phase, start + PlanLifetimeMs + 1);
        Check(pb.restores == 1, "restored once");
    }
    {
        Env env;
        ApplyStay(env);
        PlayerbotBridge::pb.current = false;
        BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + PlanLifetimeMs);
        Check(pb.restores == 0 && plans.Size() == 1 && !plans.Empty(), "no tick work outside the post-map-join phase");
    }
    // Review J6e-a B1: a stay or a focus heal admitted from a partner outside the grouped bot's group needed no partner
    // in that group, so it holds across world ticks; the partner logging out restores it.
    for (auto const intent : {Intent::Stay, Intent::FocusHealing})
    {
        for (int which = 0; which < 2; ++which)
        {
            Env env;
            PlayerbotBridge::pb.state[kBot][Part::Movement].Strategies = 4;
            if (which == 0)
                env.controllerGroup.guid = kGroup + 1;  // The partner is in another group.
            else
                env.controller.group = nullptr;  // The partner is in none.
            Check(env.Run(ValidCmd(intent)) == EffectResult::Applied && plans.Size() == 1,
                  "a stay or a focus heal from a partner outside the bot's group is applied and recorded");
            auto const held = [intent]
            {
                return intent == Intent::Stay ? Movement() == 2
                                              : PlayerbotBridge::pb.state[kBot][Part::FocusHeal].FocusHealStrategy;
            };
            for (int64_t tick = 1; tick <= 3; ++tick)
                BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + tick);
            Check(pb.restores == 0 && plans.Size() == 1 && plans.Counters().regrouped == 0 && held(),
                  "it holds across world ticks while the partner stays online outside the bot's group");
            ObjectAccessor::Players().erase(kController);
            BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + 4);
            Check(pb.restores == 1 && plans.Empty() && plans.Counters().regrouped == 1 && !held(),
                  "the partner logging out restores it");
        }
    }
    // Review J6e-a B1: a group change or the partner logging out ends every plan at once; the partner leaving the bot's
    // group ends only a plan whose admission needed the partner in it (a follow), never a stay.
    for (bool const follow : {false, true})
    {
        for (int which = 0; which < 5; ++which)
        {
            Env env;
            PlayerbotBridge::pb.state[kBot][Part::Movement].Strategies = 4;
            Check(env.Run(ValidCmd(follow ? Intent::Follow : Intent::Stay)) == EffectResult::Applied,
                  "follow or stay plan applied");
            if (which == 0)
                env.botGroup.guid = 99;  // The bot's group changed.
            else if (which == 1)
                env.bot.group = nullptr;  // The bot left its group.
            else if (which == 2)
                ObjectAccessor::Players().erase(kController);  // The partner logged out.
            else if (which == 3)
                env.controllerGroup.guid = 77;  // The partner is in another group.
            else
                env.controller.group = nullptr;  // The partner left the bot's group.
            BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + 1);
            bool const kept = !follow && which >= 3;
            Check(kept ? pb.restores == 0 && plans.Size() == 1 && Movement() == 2
                       : pb.restores == 1 && Movement() == 4 && plans.Empty() && plans.Counters().regrouped == 1,
                  "a group change or the partner logging out restores at once; the partner leaving the bot's group "
                  "restores a follow, never a stay");
        }
    }
    // DS.14: who leads the bot plays no part; the plan holds while its partner stays in the bot's group.
    for (int which = 0; which < 5; ++which)
    {
        Env env;
        ApplyStay(env);
        if (which == 0)
            env.botAI.master = &env.stranger;  // Another master.
        else if (which == 1)
            env.botAI.master = nullptr;  // Masterless, led by the partner.
        else if (which == 2)
        {
            env.botAI.master = &env.stranger;  // The partner is neither master nor leader.
            env.botGroup.leader = env.controllerGroup.leader = kStranger;
        }
        else if (which == 3)
            env.bot.ai = nullptr;  // No playerbots AI to ask.
        else
            env.controller.inWorld = false;  // Connected, between maps.
        BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + 1);
        Check(pb.restores == 0 && plans.Size() == 1 && Movement() == 2,
              "the plan holds while its partner stays in the bot's group, whoever leads the bot");
    }
    // DS.14: an ungrouped bot's plan holds while its partner is online and the bot stays ungrouped.
    for (int which = 0; which < 4; ++which)
    {
        Env env;
        env.guard.groupGuid = 0;
        env.bot.group = env.controller.group = nullptr;
        ApplyStay(env);
        bool kept = true;
        if (which == 1)
            env.controller.group = &env.controllerGroup;  // The partner joins a group of their own.
        else if (which == 2)
        {
            ObjectAccessor::Players().erase(kController);  // The partner logged out.
            kept = false;
        }
        else if (which == 3)
        {
            env.bot.group = &env.botGroup;  // The bot joined a group.
            kept = false;
        }
        BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + 1);
        Check(kept ? pb.restores == 0 && plans.Size() == 1 && Movement() == 2
                   : pb.restores == 1 && Movement() == 4 && plans.Empty() && plans.Counters().regrouped == 1,
              "an ungrouped plan holds while the bot stays alone and the partner online; it is restored when the "
              "partner logs out or the bot joins a group");
    }
    // The partner rule is per plan: the partner leaving the bot's group ends its save_mana plan and keeps its stay.
    {
        Env env;
        PlayerbotBridge::pb.state[kBot][Part::ManaSave].ManaSaveLevel = 1.0;
        ApplyStay(env);
        Check(env.Run(ValidCmd(Intent::SaveMana)) == EffectResult::Applied && plans.Size() == 2,
              "a stay and a save_mana plan from the partner in the bot's group");
        env.controller.group = nullptr;
        BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + 1);
        Check(pb.restores == 1 && pb.state.at(kBot).at(Part::ManaSave).ManaSaveLevel == 1.0 && Movement() == 2 &&
                  plans.Size() == 1 && plans.LiveApplied(MovementKey()) && plans.Counters().regrouped == 1,
              "the partner leaving the bot's group restores save_mana and keeps the stay");
    }
    {
        Env env;
        ApplyStay(env);
        BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + 1);
        Check(pb.restores == 0 && plans.Size() == 1 && Movement() == 2, "unchanged group and master keep the plan");
    }
    {
        Env env;
        ApplyStay(env);
        ObjectAccessor::Players().erase(kBot);
        BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + 1);
        Check(pb.restores == 0 && plans.Empty() && plans.Counters().departed == 1,
              "a logged-out bot's plan is dropped unrestored");
    }
    {
        Env env;
        ApplyStay(env);
        PlayerbotBridge::pb.restoreResult = PlayerbotBridge::RestoreResult::Gone;
        BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + PlanLifetimeMs);
        Check(pb.restores == 1 && plans.Empty() && plans.Counters().departed == 1 && plans.Counters().abandoned == 0,
              "a restore that finds the bot gone drops the plan");
    }
    {
        Env env;
        ApplyStay(env);
        int64_t const due = testNowMs + PlanLifetimeMs;
        PlayerbotBridge::pb.restoreResult = PlayerbotBridge::RestoreResult::Failed;
        BridgeCommandExecutor::UpdatePlans(env.phase, due);
        Check(pb.restores == 1 && plans.Size() == 1, "a failed restore stays pending");
        BridgeCommandExecutor::UpdatePlans(env.phase, due + PlanRestoreRetryMs - 1);
        Check(pb.restores == 1, "retried only after the retry interval");
        BridgeCommandExecutor::UpdatePlans(env.phase, due + PlanRestoreRetryMs);
        Check(pb.restores == 2 && plans.Size() == 1, "second attempt");
        BridgeCommandExecutor::UpdatePlans(env.phase, due + 2 * PlanRestoreRetryMs);
        Check(pb.restores == 3 && plans.Empty() && plans.Counters().abandoned == 1, "dropped after the last attempt");
    }
    {
        Env env;
        ApplyStay(env);
        PlayerbotBridge::pb.state[kBot][Part::Movement].Strategies = 1;  // A person's follow since.
        BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + PlanLifetimeMs);
        Check(pb.restores == 1 && Movement() == 1 && plans.Empty() && plans.Counters().restored == 1,
              "a later command wins: superseded, left as it is");
    }
    {
        Env env;
        ApplyStay(env);
        plans.RetireAll();  // What RetireBridgeWork's hook does.
        Check(plans.Size() == 1 && !plans.Empty() && Movement() == 2, "retirement leaves the plan restore-pending");
        BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + 1);
        Check(pb.restores == 1 && Movement() == 4 && plans.Empty() && plans.Counters().retired == 1,
              "the next tick restores a retired plan");
    }
    {
        Env env;
        PlayerbotBridge::pb.state[kBot][Part::Formation].Formation = "chaos";
        PlayerbotBridge::pb.state[kBot][Part::ManaSave].ManaSaveLevel = 1.0;
        Check(env.Run(ValidCmd(Intent::Formation)) == EffectResult::Applied &&
                  env.Run(ValidCmd(Intent::SaveMana)) == EffectResult::Applied && plans.Size() == 2,
              "formation and save_mana are two plans");
        Check(pb.state.at(kBot).at(Part::Formation).Formation != "chaos" &&
                  pb.state.at(kBot).at(Part::ManaSave).ManaSaveLevel == 0.5,
              "both applied");
        BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + PlanLifetimeMs);
        Check(pb.restores == 2 && pb.state.at(kBot).at(Part::Formation).Formation == "chaos" &&
                  pb.state.at(kBot).at(Part::ManaSave).ManaSaveLevel == 1.0 && Movement() == 0 && plans.Empty(),
              "each kind restores its own part only");
    }
}

void ResultMapping()
{
    struct Pair
    {
        PlayerbotBridge::Result in;
        EffectResult out;
    };
    Pair const pairs[] = {{PlayerbotBridge::Result::Rejected, EffectResult::Rejected},
                          {PlayerbotBridge::Result::Unavailable, EffectResult::Unavailable},
                          {PlayerbotBridge::Result::Applied, EffectResult::Applied},
                          {PlayerbotBridge::Result::Failed, EffectResult::Failed},
                          {static_cast<PlayerbotBridge::Result>(99), EffectResult::Failed}};
    for (Intent const intent : kActs)
        for (Pair const& pair : pairs)
        {
            Env env;
            PlayerbotBridge::pb.stepOutcomes = {{pair.in, PlayerbotBridge::Refusal::None}};
            Check(env.Run(ValidCmd(intent)) == pair.out && PlayerbotBridge::pb.spanExecutes == 1,
                  "playerbots result maps one to one; unknown is failed, never applied");
        }

    // DS.14: playerbots' reason for a refusal, in the bot's own words; no reason leaves the channel's cannot_now.
    struct Reason
    {
        PlayerbotBridge::Refusal in;
        CommandRefusal out;
    };
    Reason const reasons[] = {{PlayerbotBridge::Refusal::None, CommandRefusal::None},
                              {PlayerbotBridge::Refusal::Dead, CommandRefusal::Dead},
                              {PlayerbotBridge::Refusal::Battleground, CommandRefusal::Battleground},
                              {PlayerbotBridge::Refusal::Travelling, CommandRefusal::Travelling},
                              {PlayerbotBridge::Refusal::NotWithYou, CommandRefusal::NotWithYou},
                              {PlayerbotBridge::Refusal::TooFar, CommandRefusal::TooFar},
                              {PlayerbotBridge::Refusal::NotInGroup, CommandRefusal::NotInGroup},
                              {PlayerbotBridge::Refusal::Unreachable, CommandRefusal::Unreachable},
                              {PlayerbotBridge::Refusal::TargetNotVisible, CommandRefusal::TargetNotVisible},
                              {PlayerbotBridge::Refusal::NoTarget, CommandRefusal::NoTarget},
                              {PlayerbotBridge::Refusal::CannotNow, CommandRefusal::CannotNow},
                              {PlayerbotBridge::Refusal::NotPlanner, CommandRefusal::NotPlanner},
                              {PlayerbotBridge::Refusal::GoalHeld, CommandRefusal::GoalHeld},
                              {PlayerbotBridge::Refusal::UnknownPlace, CommandRefusal::UnknownPlace},
                              {PlayerbotBridge::Refusal::NoRoute, CommandRefusal::NoRoute},
                              {PlayerbotBridge::Refusal::NoSuchQuest, CommandRefusal::NoSuchQuest},
                              {PlayerbotBridge::Refusal::NothingHere, CommandRefusal::NothingHere},
                              {PlayerbotBridge::Refusal::AlreadyThere, CommandRefusal::AlreadyThere},
                              {PlayerbotBridge::Refusal::UnknownName, CommandRefusal::UnknownName},
                              {static_cast<PlayerbotBridge::Refusal>(99), CommandRefusal::None}};
    for (Reason const& reason : reasons)
    {
        {
            Env env;
            PlayerbotBridge::pb.stepOutcomes = {{PlayerbotBridge::Result::Rejected, reason.in}};
            Check(env.Run(ValidCmd(Intent::Follow)) == EffectResult::Rejected && env.why == reason.out,
                  "each playerbots refusal reason maps to its own words; none or an unknown one stays unworded");
        }
        {
            Env env;
            PlayerbotBridge::pb.stepOutcomes = {{PlayerbotBridge::Result::Applied, reason.in}};
            Check(env.Run(ValidCmd(Intent::Follow)) == EffectResult::Applied && env.why == CommandRefusal::None,
                  "a reason is read only from a refusal");
        }
    }
}
}  // namespace

// ---- LC3b: stepped orders -----------------------------------------------------------------------------------------
bool AllApplied(std::vector<ChannelStepResult> const& ran, std::size_t count)
{
    return ran.size() == count &&
           std::all_of(
               ran.begin(), ran.end(), [](ChannelStepResult const& step)
               { return step.outcome == ChannelOutcome::Applied && step.reason.empty() && step.reasonText.empty(); });
}

void SteppedOrders()
{
    using PlayerbotBridge::Refusal;
    using PlayerbotBridge::Result;
    auto const& pb = PlayerbotBridge::pb;
    BridgePlans& plans = BridgeCommandExecutor::Plans();
    auto mark = Step(Intent::Mark);
    mark.targetName = "Hogger";
    mark.icon = 7;
    auto go = Step(Intent::GoTo);
    go.place = "Goldshire";
    auto spot = Step(Intent::Position);
    spot.position = Pos();
    auto close = Step(Intent::Formation);
    close.shape = 0;
    {
        // One order of all steps: one span Execute, one authorize; names and spots go to playerbots' gate as given.
        Env env;
        Check(env.Run(Steps({mark, go, spot})) == EffectResult::Applied && env.why == CommandRefusal::None &&
                  AllApplied(env.ran, 3),
              "a stepped order applied, each step applied");
        Check(pb.spanExecutes == 1 && pb.executes == 0 && env.authorizeCalls == 1 && pb.authorizeCalled == 1 &&
                  pb.phase == &env.phase.join,
              "one span Execute in the world phase, one authorize, no one-order Execute");
        Check(pb.steps.size() == 3 && pb.steps[0].Operation == Intent::Mark && pb.steps[0].TargetName == "Hogger" &&
                  pb.steps[0].Target == 0 && pb.steps[0].Icon == 7 && pb.steps[1].Operation == Intent::GoTo &&
                  pb.steps[1].PlaceName == "Goldshire" && pb.steps[2].Operation == Intent::Position &&
                  pb.steps[2].MapId == 1 && pb.steps[2].X == 100.5 && pb.steps[2].Y == 200.25 &&
                  pb.steps[2].Z == 300.125,
              "each step's act and fields as given: playerbots resolves the name");
        Check(std::all_of(pb.steps.begin(), pb.steps.end(),
                          [](PlayerbotBridge::Order const& order)
                          {
                              return order.Bot == kBot && order.Controller == kController && order.Group == kGroup;
                          }),
              "every step carries the scope's bot, partner and captured group");
        Check(pb.reaches == 0 && plans.Size() == 0, "no module pre-check of the spot; one-shot steps record no plan");
    }
    {
        Env env;
        auto heal = Step(Intent::FocusHealing);
        heal.targetGuid = kTarget;
        auto quest = Step(Intent::Quest);
        quest.questName = "The Collector";
        auto line = Step(Intent::Formation);
        line.shape = 2;
        Check(env.Run(Steps({heal, quest, line})) == EffectResult::Applied && pb.steps.size() == 3 &&
                  pb.steps[0].Target == kTarget && pb.steps[0].TargetName.empty() &&
                  pb.steps[1].QuestName == "The Collector" && pb.steps[1].QuestId == 0 &&
                  pb.steps[2].Shape == PlayerbotBridge::FormationShape::Line && plans.Size() == 2,
              "a unit by id, a quest by title, a shape by its choice; both lasting steps recorded");
    }
    {
        Env env;
        auto quest = Step(Intent::Quest);
        quest.questId = 4242;
        auto mana = Step(Intent::SaveMana);
        mana.enabled = true;
        Check(env.Run(Steps({quest, mana, close})) == EffectResult::Applied && pb.steps[0].QuestId == 4242 &&
                  pb.steps[0].QuestName.empty() && pb.steps[1].Enabled &&
                  pb.steps[2].Shape == PlayerbotBridge::FormationShape::Close,
              "a quest by id, a flag, the first shape");
    }
    {
        // Every lasting step admitted and captured before the order, recorded after it.
        Env env;
        PlayerbotBridge::pb.state[kBot][Part::Movement].Strategies = 4;
        Check(env.Run(Steps({Step(Intent::Stay), close})) == EffectResult::Applied && pb.capturesAtExecute == 2 &&
                  pb.captures == 4 && plans.Size() == 2 && plans.Counters().applied == 2,
              "two lasting steps: captured before the order, recorded after it");
        auto const applied = plans.LiveApplied(MovementKey());
        Check(applied && applied->Strategies == 2 && Movement() == 2, "the stay plan keeps what the order set");
    }
    {
        // A refused second step ends the order: the first is recorded, the refused and the skipped ones are not.
        Env env;
        PlayerbotBridge::pb.stepOutcomes = {{Result::Applied, Refusal::None}, {Result::Rejected, Refusal::UnknownName}};
        auto mana = Step(Intent::SaveMana);
        mana.enabled = true;
        auto heal = Step(Intent::FocusHealing);
        heal.targetName = "Brother Paxton";
        Check(env.Run(Steps({Step(Intent::Stay), mana, heal})) == EffectResult::Rejected &&
                  env.why == CommandRefusal::UnknownName && pb.capturesAtExecute == 3 && pb.captures == 4 &&
                  plans.Size() == 1 && plans.LiveApplied(MovementKey()),
              "only the lasting step that ran and applied is recorded");
        Check(env.ran.size() == 2 && env.ran[0].outcome == ChannelOutcome::Applied &&
                  env.ran[1].outcome == ChannelOutcome::Rejected && env.ran[1].reason == "pb_word" &&
                  env.ran[1].reasonText == "A playerbots sentence." && pb.wordsFor == Refusal::UnknownName,
              "the refused step in playerbots' words for its refusal");
    }
    {
        // A failed step may have written: it is recorded, worded as the module's failure.
        Env env;
        PlayerbotBridge::pb.stepOutcomes = {{Result::Failed, Refusal::None}};
        auto const failed = DescribeRefusal(CommandRefusal::Failed);
        Check(env.Run(Steps({Step(Intent::Stay), go})) == EffectResult::Failed && plans.Size() == 1 &&
                  env.ran.size() == 1 && env.ran[0].outcome == ChannelOutcome::Failed &&
                  env.ran[0].reason == failed.code && env.ran[0].reasonText == failed.text,
              "a failed lasting step is recorded and worded failed");
    }
    {
        // A refusal without a reason reads cannot_now; a refused mark notes nothing.
        Env env;
        PlayerbotBridge::pb.stepOutcomes = {{Result::Applied, Refusal::None}, {Result::Rejected, Refusal::None}};
        auto const cannot = DescribeRefusal(CommandRefusal::CannotNow);
        Check(env.Run(Steps({go, mark})) == EffectResult::Rejected && env.ran.size() == 2 &&
                  env.ran[1].reason == cannot.code && env.ran[1].reasonText == cannot.text,
              "an unworded refusal reads cannot_now");
    }
    {
        Env env;
        PlayerbotBridge::pb.stepOutcomes = {{Result::Unavailable, Refusal::None}};
        auto const unavailable = DescribeRefusal(CommandRefusal::Unavailable);
        Check(env.Run(Steps({go})) == EffectResult::Unavailable && env.why == CommandRefusal::Unavailable &&
                  env.ran.size() == 1 && env.ran[0].outcome == ChannelOutcome::Rejected &&
                  env.ran[0].reason == unavailable.code,
              "an unavailable step is rejected as unavailable");
    }
    {
        // playerbots refused the order before any step ran (a malformed step): no step result, no record.
        Env env;
        PlayerbotBridge::pb.spanEarly = Result::Unavailable;
        Check(env.Run(Steps({Step(Intent::Stay), go})) == EffectResult::Unavailable && env.ran.empty() &&
                  env.authorizeCalls == 0 && plans.Size() == 0 && pb.captures == 1,
              "an order refused before its steps: no step result, no record");
    }
    {
        // Two steps of one lasting part: refused before any admission, capture or Execute.
        Env env;
        Check(env.Run(Steps({Step(Intent::Follow), go, Step(Intent::Stay)})) == EffectResult::Rejected &&
                  env.why == CommandRefusal::CannotNow && pb.spanExecutes == 0 && pb.captures == 0 &&
                  env.authorizeCalls == 0 && env.ran.empty() && plans.Size() == 0,
              "two steps of one lasting part are refused before anything runs");
    }
    {
        // A lasting step another owner's plan blocks: busy before any capture.
        Env env;
        ApplyStay(env);
        auto const captures = pb.captures;
        auto const executes = pb.spanExecutes;
        env.guard.groupGuid = kOther;
        Check(env.Run(Steps({go, close})) == EffectResult::Rejected && env.why == CommandRefusal::Busy &&
                  pb.spanExecutes == executes && pb.captures == captures && env.ran.empty(),
              "a blocked lasting step: busy, no capture, nothing runs");
    }
    {
        Env env;
        PlayerbotBridge::pb.captureFails = true;
        Check(env.Run(Steps({go, Step(Intent::Stay)})) == EffectResult::Rejected &&
                  env.why == CommandRefusal::CannotNow && pb.spanExecutes == 0 && env.authorizeCalls == 0,
              "a part that cannot be captured: nothing runs");
    }
    {
        Env env;
        PlayerbotBridge::pb.current = false;
        Check(env.Run(Steps({go})) == EffectResult::Rejected && env.why == CommandRefusal::Unavailable &&
                  pb.spanExecutes == 0 && env.authorizeCalls == 0,
              "no current world phase: unavailable, nothing runs");
    }
    {
        Env env;
        env.bot.inWorld = false;
        Check(env.Run(Steps({go})) == EffectResult::Rejected && env.why == CommandRefusal::Unavailable &&
                  pb.spanExecutes == 0,
              "a bot out of the world: unavailable, nothing runs");
    }
    {
        // LC16c: an act with no plan kind: its Intent value and the four new fields go to playerbots as given; it has
        // no plan kind, so no ledger entry.
        Env env;
        auto cast = Step(static_cast<Intent>(22));
        cast.itemName = "Hearthstone";
        cast.spellName = "Frost Nova";
        cast.choice = 1;
        cast.count = 1000;
        Check(env.Run(Steps({cast, mark})) == EffectResult::Applied && pb.steps.size() == 2 &&
                  static_cast<uint8_t>(pb.steps[0].Operation) == 22 && pb.steps[0].ItemName == "Hearthstone" &&
                  pb.steps[0].SpellName == "Frost Nova" && pb.steps[0].Choice == 1 && pb.steps[0].Count == 1000 &&
                  pb.steps[0].Icon == 0 && pb.steps[0].Shape == PlayerbotBridge::FormationShape::None &&
                  pb.steps[1].ItemName.empty() && pb.steps[1].SpellName.empty() && pb.steps[1].Choice == 0 &&
                  pb.steps[1].Count == 0 && pb.steps[1].Icon == 7 && plans.Size() == 0 && AllApplied(env.ran, 2),
              "an open act's value and its item, spell, choice and count reach playerbots; no plan recorded");
    }
    {
        // A refusal the module has no CommandRefusal for passes in playerbots' word and sentence.
        Env env;
        auto cast = Step(static_cast<Intent>(22));
        cast.count = 3;
        PlayerbotBridge::pb.stepOutcomes = {{Result::Rejected, static_cast<Refusal>(200)}};
        Check(env.Run(Steps({cast})) == EffectResult::Rejected && env.ran.size() == 1 &&
                  env.ran[0].outcome == ChannelOutcome::Rejected && env.ran[0].reason == "pb_word" &&
                  env.ran[0].reasonText == "A playerbots sentence." && pb.wordsFor == static_cast<Refusal>(200),
              "an open refusal in playerbots' words");
    }
}

// EffectPhase is built only by the world tick, its friend: the selftest's tick builds one and runs the cases in it.
void OllamaDispatch_Update(uint32_t, PlayerbotBridge::PostMapJoin const& join)
{
    OllamaBridge::EffectPhase const phase(join);
    worldPhase = &phase;
    Gates();
    Groups();
    ResultMapping();
    LastingOrders();
    PlanFlow();
    PlanTick();
    SteppedOrders();
    worldPhase = nullptr;
}

// PostMapJoin's one friend: the token is built on the stack of the world script's tick.
class OllamaBotRandomChatter
{
public:
    static void Tick()
    {
        PlayerbotBridge::PostMapJoin const join;
        OllamaDispatch_Update(0, join);
    }
};

int main()
{
    try
    {
        OllamaBotRandomChatter::Tick();
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << "\n";
        return 1;
    }
    std::cout << "PASS " << checks << " production bridge effect adapter checks\n";
    return 0;
}
