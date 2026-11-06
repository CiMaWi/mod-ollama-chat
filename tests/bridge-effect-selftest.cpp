// The production BridgeCommandExecutor class (src/mod-ollama-chat_bridge_effect.cpp) is extracted verbatim by the
// runner into bridge-effect-executor.inc (its private section made public so Effect can be called directly) and
// compiled against game and playerbots doubles that count every read and every call. The playerbots Intent, Result
// and Order types are extracted from mod-playerbots' BridgeIntentEffect.h. MapIntent and BuildEffectOrder are the
// real headers. No game, no playerbots, no server.
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

#include "bridge-effect-playerbots.inc"
#include "mod-ollama-chat_bridge_world.h"

// ---- game doubles -------------------------------------------------------------------------------------------------
float g_SayDistance = 17.5f;

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
    ObjectGuid GetGUID() const { return ObjectGuid(guid); }
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

namespace OllamaBridge
{
struct EffectPhase
{
    PlayerbotBridge::PostMapJoin join;
};

namespace
{
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

struct Env
{
    Player bot;
    Player controller;
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
        botGroup.guid = controllerGroup.guid = kGroup;
        bot.group = &botGroup;
        controller.group = &controllerGroup;
        bot.target = kTarget;
        ObjectAccessor::Players() = {{kBot, &bot}, {kController, &controller}};
        ObjectAccessor::Units() = {{kTarget, &target}, {kOther, &other}};
        ticket.scope.botGuid = kBot;
        ticket.scope.controllerGuid = kController;
        guard.groupGuid = kGroup;
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
                       {ChannelIntent::Position, PlayerbotBridge::Intent::Position, "position"}};

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
        bool const targeted = verb.intent == ChannelIntent::AttackOwnTarget || verb.intent == ChannelIntent::Mark;
        Check(pb.order.Target == (targeted ? kTarget : 0), "target only for attack and mark");
        Check(pb.order.Icon == (verb.intent == ChannelIntent::Mark ? 3 : 0), "icon only for mark");
        bool const position = verb.intent == ChannelIntent::Position;
        Check(pb.order.MapId == (position ? 1u : 0u) && pb.order.X == (position ? 100.5 : 0.0) &&
                  pb.order.Y == (position ? 200.25 : 0.0) && pb.order.Z == (position ? 300.125 : 0.0),
              "destination only for position");
        Check(pb.reaches == (position ? 1u : 0u), "reachability query only for position");
        Check(game.targetReads == (verb.intent == ChannelIntent::AttackOwnTarget ? 1u : 0u),
              "own target read only for attack");
        Check(game.unitLookups == (targeted ? 1u : 0u), "target unit resolved only for attack and mark");
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
        static_assert(!kBridgeEffectsReleased, "effects stay closed in this plan state");
        Check(executor.Execute(env.phase, env.ticket, env.guard, ValidCmd(ChannelIntent::Follow), env.Authorize()) ==
                      EffectResult::Rejected &&
                  Untouched(),
              "closed release gate: fixed refusal before any read");
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
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << "\n";
        return 1;
    }
    std::cout << "PASS " << checks << " production bridge effect adapter checks\n";
    return 0;
}
