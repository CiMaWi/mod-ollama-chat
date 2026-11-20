// The production typed-effect unit (src/mod-ollama-chat_bridge_effect.cpp) is compiled whole, with playerbots' own
// order types (BridgeIntentEffect.h, BridgeAbilities.h), against game doubles and doubles of playerbots' executor
// boundary that count every read and every call. The world phase is built as in production: by the world tick.
// LCB2: every command is stepped, so every case runs through Execute as WorldChannel calls it. LC.36: a lasting step
// runs like any other; the unit keeps no ledger and names no Capture or Restore. No game, no playerbots, no server.
#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "BridgeAbilities.h"
#include "BridgeIntentEffect.h"
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

// The one-order Execute: the stepped executor must never call it (the cases check executes == 0).
Result Execute(PostMapJoin const& phase, Order const& order, std::function<bool()> const& authorize, Refusal* why)
{
    ++pb.executes;
    if (why)
        *why = pb.refusal;
    pb.order = order;
    pb.phase = &phase;
    ++pb.authorizeCalled;
    pb.authorizeResult = authorize();
    return pb.result;
}

// LC3b: one order of steps, one authorize; each step's configured outcome, the first not Applied ends the order.
Result Execute(PostMapJoin const& phase, std::span<Order const> steps, std::function<bool()> const& authorize,
               std::vector<StepOutcome>* outcomes)
{
    ++pb.spanExecutes;
    pb.steps.assign(steps.begin(), steps.end());
    pb.phase = &phase;
    if (outcomes)
        outcomes->clear();
    if (pb.spanEarly)
        return *pb.spanEarly;
    ++pb.authorizeCalled;
    pb.authorizeResult = authorize();
    Result result = Result::Applied;
    for (std::size_t index = 0; index < steps.size() && result == Result::Applied; ++index)
    {
        StepOutcome const outcome =
            index < pb.stepOutcomes.size() ? pb.stepOutcomes[index] : StepOutcome{Result::Applied, Refusal::None};
        if (outcomes)
            outcomes->push_back(outcome);
        result = outcome.Outcome;
    }
    return result;
}

RefusalText const& RefusalWords(Refusal reason)
{
    static RefusalText words{Refusal::None, "pb_word", "A playerbots sentence."};
    pb.wordsFor = reason;
    words.Code = reason;
    return words;
}

bool PositionReachable(PostMapJoin const& phase, Order const& order)
{
    ++pb.reaches;
    pb.probe = order;
    pb.reachPhase = &phase;
    return pb.reachable;
}
}  // namespace PlayerbotBridge

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

struct Env
{
    Player bot;
    Player controller;
    Player stranger;
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
        ObjectAccessor::Players() = {{kBot, &bot}, {kController, &controller}, {kStranger, &stranger}};
        ObjectAccessor::Units() = {{kTarget, &target}, {kOther, &other}};
        ticket.scope.botGuid = kBot;
        ticket.scope.controllerGuid = kController;
        ticket.generation = kGeneration;
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
    Intent::AttackOwnTarget, Intent::Mark,      Intent::Follow,       Intent::Stay,       Intent::Flee,
    Intent::Formation,       Intent::SaveMana,  Intent::FocusHealing, Intent::GoTo,       Intent::Quest,
    Intent::Errands,         Intent::SetHearth, Intent::GrindHere,    Intent::GatherHere, Intent::MeetAt,
    Intent::PausePlan,       Intent::ResumePlan};

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
// group 0. What each act needs of the group, the partner and a target is
// playerbots' gate.
void Groups()
{
    {
        Env env;
        env.guard.groupGuid = 0;
        env.bot.group = env.controller.group = nullptr;
        Check(env.Run(ValidCmd(Intent::Stay)) == EffectResult::Applied && PlayerbotBridge::pb.steps[0].Group == 0 &&
                  PlayerbotBridge::pb.steps[0].Controller == kController,
              "an ungrouped stay is applied with group 0 for its partner");
    }
    {
        Env env;
        Check(env.Run(ValidCmd(Intent::Stay)) == EffectResult::Applied &&
                  PlayerbotBridge::pb.steps[0].Group == env.guard.groupGuid,
              "captured group reaches playerbots");
    }
}

// A step's shape (its choice's index) and flag reach playerbots as its own values.
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
    auto mark = Step(Intent::Mark);
    mark.targetName = "Hogger";
    mark.icon = 7;
    auto go = Step(Intent::GoTo);
    go.place = "Goldshire";
    auto meet = Step(Intent::MeetAt);
    meet.place = "you";
    auto close = Step(Intent::Formation);
    close.shape = 0;
    {
        // One order of all steps: one span Execute, one authorize; names and places go to playerbots' gate as given.
        Env env;
        Check(env.Run(Steps({mark, go, meet})) == EffectResult::Applied && env.why == CommandRefusal::None &&
                  AllApplied(env.ran, 3),
              "a stepped order applied, each step applied");
        Check(pb.spanExecutes == 1 && pb.executes == 0 && env.authorizeCalls == 1 && pb.authorizeCalled == 1 &&
                  pb.phase == &env.phase.join,
              "one span Execute in the world phase, one authorize, no one-order Execute");
        Check(pb.steps.size() == 3 && pb.steps[0].Operation == Intent::Mark && pb.steps[0].TargetName == "Hogger" &&
                  pb.steps[0].Target == 0 && pb.steps[0].Icon == 7 && pb.steps[1].Operation == Intent::GoTo &&
                  pb.steps[1].PlaceName == "Goldshire" && pb.steps[2].Operation == Intent::MeetAt &&
                  pb.steps[2].PlaceName == "you",
              "each step's act and fields as given: playerbots resolves the name");
        Check(std::all_of(pb.steps.begin(), pb.steps.end(),
                          [](PlayerbotBridge::Order const& order)
                          {
                              return order.Bot == kBot && order.Controller == kController && order.Group == kGroup;
                          }),
              "every step carries the scope's bot, partner and captured group");
        Check(pb.reaches == 0, "no module pre-check of the place");
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
                  pb.steps[2].Shape == PlayerbotBridge::FormationShape::Line && AllApplied(env.ran, 3),
              "a unit by id, a quest by title, a shape by its choice");
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
        // A refused second step ends the order: the skipped third one gets no result.
        Env env;
        PlayerbotBridge::pb.stepOutcomes = {{Result::Applied, Refusal::None}, {Result::Rejected, Refusal::UnknownName}};
        auto mana = Step(Intent::SaveMana);
        mana.enabled = true;
        auto heal = Step(Intent::FocusHealing);
        heal.targetName = "Brother Paxton";
        Check(env.Run(Steps({Step(Intent::Stay), mana, heal})) == EffectResult::Rejected &&
                  env.why == CommandRefusal::UnknownName && pb.steps.size() == 3,
              "a refused step ends the order");
        Check(env.ran.size() == 2 && env.ran[0].outcome == ChannelOutcome::Applied &&
                  env.ran[1].outcome == ChannelOutcome::Rejected && env.ran[1].reason == "pb_word" &&
                  env.ran[1].reasonText == "A playerbots sentence." && pb.wordsFor == Refusal::UnknownName,
              "the refused step in playerbots' words for its refusal");
    }
    {
        // A failed step may have written: it is worded as the module's failure.
        Env env;
        PlayerbotBridge::pb.stepOutcomes = {{Result::Failed, Refusal::None}};
        auto const failed = DescribeRefusal(CommandRefusal::Failed);
        Check(env.Run(Steps({Step(Intent::Stay), go})) == EffectResult::Failed && env.ran.size() == 1 &&
                  env.ran[0].outcome == ChannelOutcome::Failed && env.ran[0].reason == failed.code &&
                  env.ran[0].reasonText == failed.text,
              "a failed lasting step is worded failed");
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
        // playerbots refused the order before any step ran (a malformed step): no step result.
        Env env;
        PlayerbotBridge::pb.spanEarly = Result::Unavailable;
        Check(env.Run(Steps({Step(Intent::Stay), go})) == EffectResult::Unavailable && env.ran.empty() &&
                  env.authorizeCalls == 0,
              "an order refused before its steps: no step result");
    }
    {
        // LC.36: the hand-over keeps no ledger. Two steps of one lasting part run as one order (the bot's last action
        // wins), and a lasting step from another group runs over an earlier one: playerbots decides, nothing blocks.
        Env env;
        Check(env.Run(Steps({Step(Intent::Follow), go, Step(Intent::Stay)})) == EffectResult::Applied &&
                  env.why == CommandRefusal::None && pb.spanExecutes == 1 && env.authorizeCalls == 1 &&
                  pb.steps.size() == 3 && pb.steps[2].Operation == Intent::Stay && AllApplied(env.ran, 3),
              "two steps of one lasting part run as one order");
        env.guard.groupGuid = kOther;
        Check(env.Run(Steps({close, Step(Intent::Stay)})) == EffectResult::Applied && pb.spanExecutes == 2 &&
                  pb.steps[0].Group == kOther && AllApplied(env.ran, 2),
              "a lasting step after another group's runs; nothing holds it back");
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
        // LC16c: an open act: its Intent value and the four new fields go to playerbots as given.
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
                  pb.steps[1].Count == 0 && pb.steps[1].Icon == 7 && AllApplied(env.ran, 2),
              "an open act's value and its item, spell, choice and count reach playerbots");
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
