// The production BridgeCommandExecutor class (src/mod-ollama-chat_bridge_effect.cpp) is extracted verbatim by the
// runner into bridge-effect-executor.inc (its private section made public so Effect can be called directly) and
// compiled against game and playerbots doubles that count every read and every call. The playerbots Intent, Result
// and Order types are extracted from mod-playerbots' BridgeIntentEffect.h. MapIntent and BuildEffectOrder are the
// real headers. J12: the executor's plan ledger (the real PlanLedger) with Capture/Restore doubles that model each
// bot's lasting parts; the world tick's expiry, group, controller and logout checks and restores. No game, no
// playerbots, no server.
#include <functional>
#include <iostream>
#include <map>
#include <new>
#include <stdexcept>
#include <string>

#include "bridge-effect-playerbots.inc"
#include "mod-ollama-chat_bridge_plan.h"
#include "mod-ollama-chat_bridge_world.h"

// ---- game doubles -------------------------------------------------------------------------------------------------
float g_SayDistance = 17.5f;
// The handler's self-mark re-baseline (DS.10), counted.
unsigned ownMarkNotes = 0;
uint64_t ownMarkBot = 0;
void OllamaChat_BridgeNoteOwnMark(uint64_t botGuid)
{
    ++ownMarkNotes;
    ownMarkBot = botGuid;
}

struct ObjectGuid
{
    ObjectGuid() = default;
    explicit ObjectGuid(uint64_t raw) : _raw(raw) {}
    uint64_t GetRawValue() const { return _raw; }

private:
    uint64_t _raw = 0;
};

struct Sight
{
    bool inMap = true;
    bool samePhase = true;
    bool inDistance = true;
    bool canSee = true;
};

struct Unit
{
    bool inWorld = true;
    Sight sight;  // What this unit, as an observer, makes of any other unit.
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
// The bot's playerbots AI: only its master matters to the plan tick's controller check.
struct PlayerbotAI
{
    Player* master = nullptr;
    Player* GetMaster() const { return master; }
};

struct Counters
{
    unsigned playerLookups = 0;
    unsigned unitLookups = 0;
    unsigned targetReads = 0;
    unsigned groupReads = 0;
    unsigned distanceCalls = 0;
    float lastDistance = -1.0f;
};
Counters game;

struct Player : Unit
{
    Group* group = nullptr;
    uint64_t target = 0;
    PlayerbotAI* ai = nullptr;
    bool IsInMap(Unit const*) const { return sight.inMap; }
    bool InSamePhase(Unit const*) const { return sight.samePhase; }
    bool IsWithinDistInMap(Unit const*, float distance) const
    {
        ++game.distanceCalls;
        game.lastDistance = distance;
        return sight.inDistance;
    }
    bool CanSeeOrDetect(Unit const*, bool, bool) const { return sight.canSee; }
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

struct PlayerbotsMgr
{
    static PlayerbotsMgr& instance()
    {
        static PlayerbotsMgr manager;
        return manager;
    }
    PlayerbotAI* GetPlayerbotAI(Player* player) const { return player ? player->ai : nullptr; }
};

namespace OllamaBridge
{
// The steady clock the executor reads once per order (bridge.cpp is not linked).
int64_t testNowMs = 1000;
Clock Now() { return {0, testNowMs}; }

struct EffectPhase
{
    PlayerbotBridge::PostMapJoin join;
};

namespace
{
using BridgePlans = PlanLedger<PlayerbotBridge::Baseline>;  // As in bridge_effect.cpp.
#include "bridge-effect-executor.inc"
}  // namespace
}  // namespace OllamaBridge

namespace
{
using namespace OllamaBridge;
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
    EffectPhase phase;
    WorldTicket ticket;
    DispatchGuard guard;
    unsigned authorizeCalls = 0;

    Env()
    {
        game = Counters{};
        PlayerbotBridge::pb = PlayerbotBridge::Doubles{};
        g_SayDistance = 17.5f;
        ownMarkNotes = 0;
        ownMarkBot = 0;
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

    EffectResult Run(ChannelCommand const& command)
    {
        return BridgeCommandExecutor::Effect(phase, ticket, guard, command, Authorize());
    }
};

ChannelPosition Pos() { return ChannelPosition{100.5, 200.25, 300.125, 1}; }

ChannelCommand Cmd(ChannelIntent intent, ChannelParams params = std::monostate{})
{
    ChannelCommand command;
    command.commandId = "c1";
    command.issuerGuid = kController;
    command.botGuid = kBot;
    command.intent = intent;
    command.params = std::move(params);
    return command;
}

ChannelCommand ValidCmd(ChannelIntent intent)
{
    switch (intent)
    {
        case ChannelIntent::Pull:
        case ChannelIntent::FocusHealing:
            return Cmd(intent, ChannelTarget{kTarget});
        case ChannelIntent::Mark:
            return Cmd(intent, ChannelMark{kTarget, 3});
        case ChannelIntent::AssignRole:
            return Cmd(intent, ChannelRole::Tank);
        case ChannelIntent::SaveMana:
            return Cmd(intent, true);
        case ChannelIntent::Formation:
            return Cmd(intent, ChannelShape::Close);
        case ChannelIntent::Position:
            return Cmd(intent, Pos());
        case ChannelIntent::DungeonClear:
            return Cmd(intent, ChannelMode::On);
        default:
            return Cmd(intent);
    }
}

struct Verb
{
    ChannelIntent intent;
    PlayerbotBridge::Intent op;
    char const* name;
};
Verb const kVerbs[] = {{ChannelIntent::AttackOwnTarget, PlayerbotBridge::Intent::AttackOwnTarget, "attack_own_target"},
                       {ChannelIntent::Mark, PlayerbotBridge::Intent::Mark, "mark"},
                       {ChannelIntent::Follow, PlayerbotBridge::Intent::Follow, "follow"},
                       {ChannelIntent::Stay, PlayerbotBridge::Intent::Stay, "stay"},
                       {ChannelIntent::Flee, PlayerbotBridge::Intent::Flee, "flee"},
                       {ChannelIntent::Position, PlayerbotBridge::Intent::Position, "position"},
                       {ChannelIntent::Formation, PlayerbotBridge::Intent::Formation, "formation"},
                       {ChannelIntent::SaveMana, PlayerbotBridge::Intent::SaveMana, "save_mana"},
                       {ChannelIntent::FocusHealing, PlayerbotBridge::Intent::FocusHealing, "focus_healing"}};

// Nothing reached playerbots and no game read happened.
bool Untouched()
{
    auto const& p = PlayerbotBridge::pb;
    return p.executes == 0 && p.reaches == 0 && game.playerLookups == 0 && game.unitLookups == 0 &&
           game.targetReads == 0 && game.groupReads == 0;
}

void VerbSwitch()
{
    for (Verb const& verb : kVerbs)
    {
        Env env;
        PlayerbotBridge::pb.reachable = true;
        PlayerbotBridge::pb.result = PlayerbotBridge::Result::Applied;
        auto const command = ValidCmd(verb.intent);
        Check(env.Run(command) == EffectResult::Applied, verb.name);
        auto const& pb = PlayerbotBridge::pb;
        Check(pb.executes == 1, "exactly one playerbots call");
        Check(pb.order.Operation == verb.op, "verb maps to its own playerbots operation");
        Check(pb.order.Bot == kBot && pb.order.Controller == kController && pb.order.Group == kGroup,
              "bot, controller and group from scope and guard");
        Check(pb.order.VisibilityDistance == g_SayDistance, "visibility radius");
        Check(pb.phase == &env.phase.join, "the joined phase is forwarded");
        Check(env.authorizeCalls == 1 && pb.authorizeCalled == 1 && pb.authorizeResult,
              "authorize passes through and the executor adds none");
        bool const targeted = verb.intent == ChannelIntent::AttackOwnTarget || verb.intent == ChannelIntent::Mark ||
                              verb.intent == ChannelIntent::FocusHealing;
        Check(pb.order.Target == (targeted ? kTarget : 0), "target only for attack, mark and focus heal");
        Check(pb.order.Icon == (verb.intent == ChannelIntent::Mark ? 3 : 0), "icon only for mark");
        Check(pb.order.Shape == (verb.intent == ChannelIntent::Formation ? PlayerbotBridge::FormationShape::Close
                                                                         : PlayerbotBridge::FormationShape::None),
              "shape only for formation");
        Check(pb.order.Enabled == (verb.intent == ChannelIntent::SaveMana), "enable flag only for save_mana");
        bool const position = verb.intent == ChannelIntent::Position;
        Check(pb.order.MapId == (position ? 1u : 0u) && pb.order.X == (position ? 100.5 : 0.0) &&
                  pb.order.Y == (position ? 200.25 : 0.0) && pb.order.Z == (position ? 300.125 : 0.0),
              "destination only for position");
        Check(pb.reaches == (position ? 1u : 0u), "reachability query only for position");
        Check(game.targetReads == (verb.intent == ChannelIntent::AttackOwnTarget ? 1u : 0u),
              "own target read only for attack");
        Check(game.unitLookups == (targeted ? 1u : 0u), "target unit resolved only for attack, mark and focus heal");
        Check(game.playerLookups == 2, "bot and controller resolved by guid");
    }
}

void OtherVerbs()
{
    for (uint8_t raw = 0; raw <= static_cast<uint8_t>(ChannelIntent::DungeonClear); ++raw)
    {
        auto const intent = static_cast<ChannelIntent>(raw);
        bool supported = false;
        for (Verb const& verb : kVerbs)
            supported = supported || verb.intent == intent;
        if (supported)
            continue;
        Env env;
        PlayerbotBridge::pb.reachable = true;
        Check(env.Run(ValidCmd(intent)) == EffectResult::Rejected, "unsupported verb is rejected");
        Check(Untouched() && env.authorizeCalls == 0, "unsupported verb: no game read, no playerbots call");
    }
    // Out-of-range verb values too.
    Env env;
    Check(env.Run(Cmd(static_cast<ChannelIntent>(250))) == EffectResult::Rejected && Untouched(),
          "unknown verb value is rejected before any read");
}

void Gates()
{
    {
        Env env;
        env.phase.join.current = false;
        Check(env.Run(ValidCmd(ChannelIntent::Follow)) == EffectResult::Rejected && Untouched(),
              "a phase that is no longer current reads nothing");
    }
    {
        Env env;
        BridgeCommandExecutor executor;
        static_assert(kBridgeEffectsReleased, "effects are on since J6b4 (DS.5)");
        Check(executor.Execute(env.phase, env.ticket, env.guard, ValidCmd(ChannelIntent::Follow), env.Authorize()) ==
                      EffectResult::Applied &&
                  PlayerbotBridge::pb.executes == 1 && env.authorizeCalls == 1,
              "open release gate: Execute reaches the typed effect");
    }
    {
        Env env;
        env.phase.join.current = false;
        Check(env.Run(ValidCmd(ChannelIntent::Mark)) == EffectResult::Rejected && ownMarkNotes == 0,
              "a mark refused before playerbots re-baselines nothing");
    }
    for (int which = 0; which < 4; ++which)
    {
        Env env;
        if (which == 0)
            ObjectAccessor::Players().erase(kBot);
        else if (which == 1)
            ObjectAccessor::Players().erase(kController);
        else if (which == 2)
            env.bot.inWorld = false;
        else
            env.controller.inWorld = false;
        Check(env.Run(ValidCmd(ChannelIntent::Follow)) == EffectResult::Rejected && PlayerbotBridge::pb.executes == 0,
              "missing or out-of-world bot/controller is rejected");
    }
}

void Facts()
{
    // Group facts: controller group, bot group and the captured group must all agree and be nonzero.
    for (int which = 0; which < 5; ++which)
    {
        Env env;
        if (which == 0)
            env.controllerGroup.guid = kGroup + 1;
        else if (which == 1)
            env.botGroup.guid = kGroup + 1;
        else if (which == 2)
            env.guard.groupGuid = kGroup + 1;
        else if (which == 3)
            env.bot.group = nullptr;
        else
        {
            env.guard.groupGuid = 0;
            env.bot.group = env.controller.group = nullptr;
        }
        Check(env.Run(ValidCmd(ChannelIntent::Stay)) == EffectResult::Rejected && PlayerbotBridge::pb.executes == 0,
              "group mismatch or no group is rejected");
    }
    {
        // The captured group, not a re-read one, is what playerbots is given.
        Env env;
        Check(env.Run(ValidCmd(ChannelIntent::Stay)) == EffectResult::Applied &&
                  PlayerbotBridge::pb.order.Group == env.guard.groupGuid,
              "captured group reaches playerbots");
    }
    {
        // Scope, not wire: a command for another bot or from another issuer is refused.
        Env env;
        auto command = ValidCmd(ChannelIntent::Stay);
        command.botGuid = kOther;
        Check(env.Run(command) == EffectResult::Rejected && PlayerbotBridge::pb.executes == 0, "wire bot mismatch");
        command = ValidCmd(ChannelIntent::Stay);
        command.issuerGuid = kOther;
        Check(env.Run(command) == EffectResult::Rejected && PlayerbotBridge::pb.executes == 0, "wire issuer mismatch");
    }
    // Attack: own target only.
    {
        Env env;
        env.bot.target = 0;
        Check(env.Run(ValidCmd(ChannelIntent::AttackOwnTarget)) == EffectResult::Rejected && game.unitLookups == 0 &&
                  PlayerbotBridge::pb.executes == 0,
              "attack without an own target resolves nothing");
    }
    {
        Env env;
        env.bot.target = kOther;
        Check(env.Run(ValidCmd(ChannelIntent::AttackOwnTarget)) == EffectResult::Applied &&
                  PlayerbotBridge::pb.order.Target == kOther,
              "attack targets the bot's own current target");
    }
    {
        Env env;
        env.bot.target = 9999;  // Not resolvable.
        Check(env.Run(ValidCmd(ChannelIntent::AttackOwnTarget)) == EffectResult::Rejected &&
                  PlayerbotBridge::pb.executes == 0,
              "unresolvable own target");
    }
    {
        // A mark never reads the bot's target and uses the wire guid.
        Env env;
        env.bot.target = kOther;
        Check(env.Run(ValidCmd(ChannelIntent::Mark)) == EffectResult::Applied && game.targetReads == 0 &&
                  PlayerbotBridge::pb.order.Target == kTarget,
              "mark ignores the bot's own target");
    }
    // Target visibility must hold for the bot and for the controller, in each respect.
    for (ChannelIntent intent : {ChannelIntent::AttackOwnTarget, ChannelIntent::Mark})
        for (int observer = 0; observer < 2; ++observer)
            for (int respect = 0; respect < 6; ++respect)
            {
                Env env;
                Unit& who = observer == 0 ? static_cast<Unit&>(env.bot) : static_cast<Unit&>(env.controller);
                if (respect == 0)
                    who.sight.inMap = false;
                else if (respect == 1)
                    who.sight.samePhase = false;
                else if (respect == 2)
                    who.sight.inDistance = false;
                else if (respect == 3)
                    who.sight.canSee = false;
                else if (respect == 4)
                    env.target.inWorld = false;
                else
                    ObjectAccessor::Units().erase(kTarget);
                if (intent == ChannelIntent::AttackOwnTarget && respect == 5)
                    env.bot.target = kTarget;  // Guid stays; the unit cannot be resolved.
                Check(env.Run(ValidCmd(intent)) == EffectResult::Rejected && PlayerbotBridge::pb.executes == 0,
                      "target not visible to both is rejected");
            }
    {
        Env env;
        env.Run(ValidCmd(ChannelIntent::Mark));
        Check(game.distanceCalls == 2 && game.lastDistance == g_SayDistance, "visibility uses g_SayDistance");
        Env closed;
        g_SayDistance = 0.0f;
        Check(closed.Run(ValidCmd(ChannelIntent::Mark)) == EffectResult::Rejected, "zero radius sees nothing");
        g_SayDistance = 17.5f;
    }
    // Position: a fact only when playerbots finds it reachable, and the probe carries the exact destination.
    {
        Env env;
        PlayerbotBridge::pb.reachable = false;
        Check(env.Run(ValidCmd(ChannelIntent::Position)) == EffectResult::Rejected &&
                  PlayerbotBridge::pb.reaches == 1 && PlayerbotBridge::pb.executes == 0,
              "unreachable position is rejected");
    }
    {
        Env env;
        PlayerbotBridge::pb.reachable = true;
        Check(env.Run(ValidCmd(ChannelIntent::Position)) == EffectResult::Applied, "reachable position");
        auto const& probe = PlayerbotBridge::pb.probe;
        Check(probe.Operation == PlayerbotBridge::Intent::Position && probe.Bot == kBot &&
                  probe.Controller == kController && probe.VisibilityDistance == g_SayDistance && probe.MapId == 1 &&
                  probe.X == 100.5 && probe.Y == 200.25 && probe.Z == 300.125,
              "reachability probe carries scope guids, radius and the exact destination");
        Check(PlayerbotBridge::pb.reachPhase == &env.phase.join, "probe uses the joined phase");
    }
    {
        // A non-position verb never asks about reachability even when a position payload would be reachable.
        Env env;
        PlayerbotBridge::pb.reachable = true;
        Check(env.Run(ValidCmd(ChannelIntent::Flee)) == EffectResult::Applied && PlayerbotBridge::pb.reaches == 0,
              "no reachability query for other verbs");
    }
}

// J12: the wire's shape and flag reach playerbots as its own values; a focus heal target must be seen.
void LastingOrders()
{
    struct Pair
    {
        ChannelShape wire;
        PlayerbotBridge::FormationShape playerbots;
    };
    Pair const pairs[] = {{ChannelShape::Close, PlayerbotBridge::FormationShape::Close},
                          {ChannelShape::Spread, PlayerbotBridge::FormationShape::Spread},
                          {ChannelShape::Line, PlayerbotBridge::FormationShape::Line}};
    for (Pair const& pair : pairs)
    {
        Env env;
        Check(env.Run(Cmd(ChannelIntent::Formation, pair.wire)) == EffectResult::Applied &&
                  PlayerbotBridge::pb.order.Shape == pair.playerbots,
              "each wire shape maps to its playerbots shape");
    }
    {
        Env env;
        Check(env.Run(Cmd(ChannelIntent::SaveMana, false)) == EffectResult::Applied &&
                  PlayerbotBridge::pb.executes == 1 && !PlayerbotBridge::pb.order.Enabled,
              "save_mana off carries false");
    }
    {
        Env env;
        env.target.inWorld = false;
        Check(env.Run(ValidCmd(ChannelIntent::FocusHealing)) == EffectResult::Rejected &&
                  PlayerbotBridge::pb.executes == 0 && env.authorizeCalls == 0,
              "a focus heal target nobody sees is refused before playerbots");
    }
}

PlanKey MovementKey() { return {kBot, PlanKind::Movement}; }
uint16_t Movement() { return PlayerbotBridge::pb.state[kBot][Part::Movement].Strategies; }

// A stay plan over a person's earlier movement state 4.
void ApplyStay(Env& env)
{
    PlayerbotBridge::pb.state[kBot][Part::Movement].Strategies = 4;
    Check(env.Run(ValidCmd(ChannelIntent::Stay)) == EffectResult::Applied, "stay plan applied");
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
        Check(env.Run(ValidCmd(ChannelIntent::Stay)) == EffectResult::Rejected && pb.captures == 0 &&
                  pb.executes == 0 && env.authorizeCalls == 0 && plans.Empty(),
              "a plan the ledger refuses (stale generation) reads and changes nothing");
    }
    {
        Env env;
        PlayerbotBridge::pb.captureFails = true;
        Check(env.Run(ValidCmd(ChannelIntent::Formation)) == EffectResult::Rejected && pb.captures == 1 &&
                  pb.executes == 0 && env.authorizeCalls == 0 && plans.Empty(),
              "no baseline, no mutation");
    }
    for (auto const result : {PlayerbotBridge::Result::Rejected, PlayerbotBridge::Result::Unavailable})
    {
        Env env;
        PlayerbotBridge::pb.result = result;
        env.Run(ValidCmd(ChannelIntent::SaveMana));
        Check(pb.executes == 1 && pb.captures == 1 && plans.Empty() && plans.Counters().unrecorded == 0,
              "an attempt that promises no mutation records none");
    }
    // Review J12 NOTE B: unreachable today, but an attempt that may have written and left no entry is counted.
    for (auto const result : {PlayerbotBridge::Result::Applied, PlayerbotBridge::Result::Failed})
    {
        Env env;
        PlayerbotBridge::pb.result = result;
        PlayerbotBridge::pb.captureFailsAfterExecute = true;
        env.Run(ValidCmd(ChannelIntent::Formation));
        Check(pb.executes == 1 && pb.captures == 2 && plans.Empty() && plans.Counters().applied == 0 &&
                  plans.Counters().unrecorded == 1,
              "an attempt whose state cannot be read afterwards is counted as unrecorded");
    }
    {
        Env env;
        PlayerbotBridge::pb.duringExecute = [] { BridgeCommandExecutor::Plans().RetireAll(); };
        Check(env.Run(ValidCmd(ChannelIntent::SaveMana)) == EffectResult::Applied && pb.captures == 2 &&
                  plans.Empty() && plans.Counters().unrecorded == 1,
              "an attempt whose record the ledger refuses is counted as unrecorded");
    }
    {
        Env env;
        PlayerbotBridge::pb.result = PlayerbotBridge::Result::Failed;
        Check(
            env.Run(ValidCmd(ChannelIntent::SaveMana)) == EffectResult::Failed && pb.captures == 2 && plans.Size() == 1,
            "a failed attempt may have written: recorded");
    }
    {
        Env env;
        PlayerbotBridge::pb.state[kBot][Part::Movement].Strategies = 4;
        Check(env.Run(ValidCmd(ChannelIntent::Follow)) == EffectResult::Applied, "follow plan");
        testNowMs += 1000;
        PlayerbotBridge::pb.state[kBot][Part::Movement].Strategies = 9;  // A person's own command in between.
        Check(env.Run(ValidCmd(ChannelIntent::Stay)) == EffectResult::Applied && pb.captures == 4 &&
                  pb.capturesAtExecute == 3,
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
        Check(env.Run(ValidCmd(ChannelIntent::Stay)) == EffectResult::Rejected && pb.executes == 1,
              "a repeat whose capture fails does not execute");
    }
    {
        Env env;
        ApplyStay(env);
        testNowMs += PlanLifetimeMs - 1;
        PlayerbotBridge::pb.executeAdvanceMs = 5;
        Check(env.Run(ValidCmd(ChannelIntent::Stay)) == EffectResult::Applied && plans.Counters().renewed == 1 &&
                  plans.LiveApplied(MovementKey()),
              "admission and record use one clock reading, so a slow renewal is still recorded");
    }
    for (auto const intent : {ChannelIntent::Flee, ChannelIntent::Mark, ChannelIntent::AttackOwnTarget})
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
        env.phase.join.current = false;
        BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + PlanLifetimeMs);
        Check(pb.restores == 0 && plans.Size() == 1 && !plans.Empty(), "no tick work outside the post-map-join phase");
    }
    for (int which = 0; which < 7; ++which)
    {
        Env env;
        ApplyStay(env);
        if (which == 0)
            env.botGroup.guid = 99;  // The bot's group changed.
        else if (which == 1)
            env.bot.group = nullptr;  // The bot left its group.
        else if (which == 2)
            env.botAI.master = &env.stranger;  // Another master.
        else if (which == 3)
            ObjectAccessor::Players().erase(kController);  // The controller logged out.
        else if (which == 4)
            env.controllerGroup.guid = 77;  // The controller is in another group.
        else if (which == 5)
        {
            env.botAI.master = nullptr;  // Masterless, and someone else leads.
            env.botGroup.leader = kStranger;
        }
        else
            env.bot.ai = nullptr;  // No longer a bot.
        BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + 1);
        Check(pb.restores == 1 && Movement() == 4 && plans.Empty() && plans.Counters().regrouped == 1,
              "a group change or lost controller authority restores at once");
    }
    {
        Env env;
        ApplyStay(env);
        env.botAI.master = nullptr;
        BridgeCommandExecutor::UpdatePlans(env.phase, testNowMs + 1);
        Check(pb.restores == 0 && plans.Size() == 1, "the controller leading a masterless bot keeps its plan");
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
        Check(env.Run(ValidCmd(ChannelIntent::Formation)) == EffectResult::Applied &&
                  env.Run(ValidCmd(ChannelIntent::SaveMana)) == EffectResult::Applied && plans.Size() == 2,
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
    for (Verb const& verb : kVerbs)
        for (Pair const& pair : pairs)
        {
            Env env;
            PlayerbotBridge::pb.reachable = true;
            PlayerbotBridge::pb.result = pair.in;
            Check(env.Run(ValidCmd(verb.intent)) == pair.out && PlayerbotBridge::pb.executes == 1,
                  "playerbots result maps one to one; unknown is failed, never applied");
            bool const mayHaveMarked = verb.intent == ChannelIntent::Mark &&
                                       (pair.out == EffectResult::Applied || pair.out == EffectResult::Failed);
            Check(ownMarkNotes == (mayHaveMarked ? 1u : 0u) && (!mayHaveMarked || ownMarkBot == kBot),
                  "only a mark that applied or may have applied re-baselines the marking bot's own marks");
        }
}
}  // namespace

int main()
{
    try
    {
        VerbSwitch();
        OtherVerbs();
        Gates();
        Facts();
        ResultMapping();
        LastingOrders();
        PlanFlow();
        PlanTick();
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << "\n";
        return 1;
    }
    std::cout << "PASS " << checks << " production bridge effect adapter checks\n";
    return 0;
}
