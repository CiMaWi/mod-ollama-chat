// Offline checks of the pure J6e-c watch table (DS.14 (c)): admission, settled observation, debounce, firing, silent
// expiry, wake rate, cancellation and the scan cost rule. No game reads: the test plays the world thread's reader.
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "mod-ollama-chat_bridge_watch.h"

namespace
{
using namespace OllamaBridge;
unsigned checks = 0;
void Check(bool condition, char const* label)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(label);
}

Clock const start{1000000, 50000};
Clock At(int64_t ms) { return {start.unixMs + ms, start.steadyMs + ms}; }

Watch Make(uint64_t bot, WatchCondition condition, uint64_t partner = 900)
{
    Watch watch;
    watch.id = "watch-" + std::to_string(bot);
    watch.botGuid = bot;
    watch.partnerGuid = partner;
    watch.channel = "party";
    watch.contextId = "context-" + std::to_string(bot);
    watch.condition = condition;
    return watch;
}

WatchFacts const fighting{true, true, 2};
WatchFacts const busy{true, false, 7};
WatchFacts const idle{true, false, ActivityIdle};
WatchRoute const line{0, start.unixMs};

// The world thread's reader: the same facts for every watch, counted.
struct Reader
{
    WatchFacts facts = idle;
    bool readable = true;
    unsigned reads = 0;
};

std::vector<Watch> Scan(WatchTable& table, Reader& reader, Clock now)
{
    std::vector<Watch> due;
    table.Scan(
        now,
        [&](Watch const&, WatchFacts& facts)
        {
            ++reader.reads;
            facts = reader.facts;
            return reader.readable;
        },
        due);
    return due;
}

void Words()
{
    Check(std::string(WakeText(WatchCondition::CombatEnd)) == "Your fight is over." &&
              std::string(WakeText(WatchCondition::ActivityDone)) == "You have finished what you were doing." &&
              std::string(WakeText(WatchCondition::Time)) == "The time you meant to wait has passed.",
          "words: the fixed in-world wake sentences");
    for (auto const condition :
         {WatchCondition::CombatEnd, WatchCondition::ActivityDone, WatchCondition::Time, WatchCondition::None})
    {
        WatchCondition parsed = WatchCondition::None;
        Check(ParseWatchCondition(WatchConditionName(condition), parsed) && parsed == condition,
              "words: condition names round-trip");
    }
    WatchCondition parsed = WatchCondition::Time;
    for (char const* bad : {"combat", "", "Combat_End", "their_answer", "after"})
        Check(!ParseWatchCondition(bad, parsed) && parsed == WatchCondition::Time, "words: other names refused");
}

void Admission()
{
    WatchTable table;
    auto const arm = [&](Watch watch, WatchFacts const& facts, int64_t wait, WatchRoute route = line)
    { return table.Arm(std::move(watch), route, facts, start.unixMs + wait, start); };
    Check(arm(Make(1, WatchCondition::Time), idle, 4999) == CommandRefusal::WaitTooShort, "admit: 4999 ms too short");
    Check(arm(Make(1, WatchCondition::Time), idle, 5000) == CommandRefusal::None, "admit: 5000 ms armed");
    Check(arm(Make(2, WatchCondition::Time), idle, 600001) == CommandRefusal::WaitTooLong, "admit: 600001 ms too long");
    Check(arm(Make(2, WatchCondition::Time), idle, 600000) == CommandRefusal::None, "admit: 600000 ms armed");
    Check(arm(Make(3, WatchCondition::CombatEnd), idle, 60000) == CommandRefusal::NotFighting,
          "admit: combat_end outside a fight is not_fighting");
    Check(arm(Make(3, WatchCondition::CombatEnd), fighting, 60000) == CommandRefusal::None,
          "admit: combat_end in a fight");
    Check(arm(Make(4, WatchCondition::ActivityDone), idle, 60000) == CommandRefusal::NotBusy,
          "admit: activity_done while idle is not_busy");
    Check(arm(Make(4, WatchCondition::ActivityDone), busy, 60000) == CommandRefusal::None,
          "admit: activity_done while busy");
    Check(table.Size() == 4 && table.Find(4)->activity == 7 && table.Find(3)->activity == 2,
          "admit: the arm-time activity is stored");

    auto replacement = Make(1, WatchCondition::CombatEnd, 901);
    replacement.contextId = "context-new";
    Check(arm(replacement, fighting, 90000) == CommandRefusal::None, "admit: a new watch replaces the bot's old one");
    auto const* replaced = table.Find(1);
    Check(table.Size() == 4 && replaced && replaced->condition == WatchCondition::CombatEnd &&
              replaced->partnerGuid == 901 && replaced->contextId == "context-new" &&
              replaced->untilUnixMs == start.unixMs + 90000 && replaced->untilSteadyMs == start.steadyMs + 90000,
          "admit: one watch per bot, with the new fields");
    Check(replaced->depth == 1 && replaced->originUnixMs == start.unixMs && replaced->armedSteadyMs == start.steadyMs &&
              replaced->holdsSinceMs == -1,
          "admit: depth, origin and arming time stored");

    Check(arm(Make(5, WatchCondition::Time), idle, 60000, {2, start.unixMs}) == CommandRefusal::None &&
              table.Find(5)->depth == 3,
          "admit: the second wake may arm the third");
    Check(arm(Make(6, WatchCondition::Time), idle, 60000, {3, start.unixMs}) == CommandRefusal::PutOff,
          "admit: the third wake may not arm a fourth");
    WatchRoute const old{1, start.unixMs - 300000};
    Check(arm(Make(6, WatchCondition::Time), idle, 300000, old) == CommandRefusal::None && table.Find(6)->depth == 2,
          "admit: until exactly 600 s after the player's line");
    Check(arm(Make(7, WatchCondition::Time), idle, 300001, old) == CommandRefusal::PutOff,
          "admit: until past 600 s after the player's line is put_off");
    Check(arm(Make(7, WatchCondition::Time), idle, 60000, {0, 0}) == CommandRefusal::CannotNow,
          "admit: an unknown origin");
    Check(arm(Make(7, WatchCondition::Time), WatchFacts{}, 60000) == CommandRefusal::CannotNow,
          "admit: a bot not ready");
    Check(arm(Make(0, WatchCondition::Time), idle, 60000) == CommandRefusal::CannotNow, "admit: no bot");
    Check(arm(Make(7, WatchCondition::Time, 0), idle, 60000) == CommandRefusal::CannotNow, "admit: no partner");
    Check(arm(Make(7, WatchCondition::None), idle, 60000) == CommandRefusal::CannotNow, "admit: none is never armed");
    Check(!table.Find(7) && table.Size() == 6, "admit: nothing refused was stored");
    auto const counters = table.Counters();
    Check(counters.armed == 7 && counters.rejected == 11 && !counters.fired && !counters.expired && !counters.dropped,
          "admit: armed and rejected counted");
    Check(table.Clear(1) && !table.Find(1) && !table.Clear(1) && table.Counters() == counters,
          "admit: none clears the bot's watch without counting");

    WatchTable full;
    for (uint64_t bot = 1; bot <= MaxWatches; ++bot)
        Check(
            full.Arm(Make(bot, WatchCondition::Time), line, idle, start.unixMs + 60000, start) == CommandRefusal::None,
            "full: fill the table");
    Check(full.Size() == MaxWatches &&
              full.Arm(Make(MaxWatches + 1, WatchCondition::Time), line, idle, start.unixMs + 60000, start) ==
                  CommandRefusal::CannotNow &&
              full.Size() == MaxWatches,
          "full: a full table refuses a new bot");
    Check(full.Arm(Make(MaxWatches, WatchCondition::CombatEnd), line, fighting, start.unixMs + 60000, start) ==
                  CommandRefusal::None &&
              full.Size() == MaxWatches && full.Find(MaxWatches)->condition == WatchCondition::CombatEnd,
          "full: a full table still replaces a bot's watch");
}

void Firing()
{
    WatchTable table;
    Reader reader;
    Check(table.Arm(Make(1, WatchCondition::CombatEnd), line, fighting, start.unixMs + 60000, start) ==
              CommandRefusal::None,
          "fire: combat_end armed");
    reader.facts = idle;  // The fight ended within the arming turn.
    Check(Scan(table, reader, At(500)).empty() && table.Find(1)->holdsSinceMs == -1,
          "fire: an observation 500 ms after arming does not count");
    Check(Scan(table, reader, At(999)).empty() && table.Find(1)->holdsSinceMs == -1,
          "fire: an observation 999 ms after arming does not count");
    Check(Scan(table, reader, At(1000)).empty() && table.Find(1)->holdsSinceMs == start.steadyMs + 1000,
          "fire: the hold starts with the first settled observation");
    Check(Scan(table, reader, At(2999)).empty() && table.Size() == 1, "fire: held 1999 ms keeps");
    auto due = Scan(table, reader, At(3000));
    Check(due.size() == 1 && due[0].botGuid == 1 && due[0].id == "watch-1" && table.Empty(),
          "fire: held 2000 ms fires and leaves the table");
    Check(!table.Counters().fired && !table.Counters().expired && !table.Counters().dropped,
          "fire: counted only once the wake opened");
    table.Woke(due[0], true, At(3000));
    Check(table.Counters().fired == 1, "fire: an opened wake counts fired");

    Check(table.Arm(Make(2, WatchCondition::CombatEnd), line, fighting, start.unixMs + 60000, start) ==
              CommandRefusal::None,
          "fire: second combat_end armed");
    Check(Scan(table, reader, At(1500)).empty() && Scan(table, reader, At(3000)).empty(), "fire: holding 1500 ms");
    reader.facts = fighting;
    Check(Scan(table, reader, At(3200)).empty() && table.Find(2)->holdsSinceMs == -1,
          "fire: a re-pull breaks the hold");
    reader.facts = idle;
    Check(Scan(table, reader, At(3600)).empty() && Scan(table, reader, At(5599)).empty(),
          "fire: after a re-pull the hold needs its whole 2 s again");
    Check(Scan(table, reader, At(5600)).size() == 1, "fire: 2 s after the re-pull ended");

    // Same-tick own action: the bot attacked in the arming turn, so its activity changed at once.
    WatchTable own;
    Check(
        own.Arm(Make(3, WatchCondition::ActivityDone), line, busy, start.unixMs + 60000, start) == CommandRefusal::None,
        "own: activity_done armed while travelling");
    reader.facts = fighting;
    for (int64_t const ms : {0, 500, 999})
        Check(Scan(own, reader, At(ms)).empty() && own.Find(3)->holdsSinceMs == -1,
              "own: the arming turn's own act does not count");
    Check(Scan(own, reader, At(1000)).empty() && Scan(own, reader, At(2999)).empty(), "own: no wake before 3 s");
    Check(Scan(own, reader, At(3000)).size() == 1, "own: the changed activity counts after settle and debounce");

    WatchTable same;
    Check(same.Arm(Make(4, WatchCondition::ActivityDone), line, busy, start.unixMs + 60000, start) ==
              CommandRefusal::None,
          "same: activity_done armed");
    reader.facts = busy;
    for (int64_t const ms : {1000, 4000, 10000})
        Check(Scan(same, reader, At(ms)).empty() && same.Find(4)->holdsSinceMs == -1,
              "same: the arm-time activity still current is not done");
    reader.facts = idle;
    Check(Scan(same, reader, At(10500)).empty() && Scan(same, reader, At(12500)).size() == 1,
          "same: idle counts as done after the debounce");
}

void Expiry()
{
    WatchTable table;
    Reader reader;
    reader.facts = fighting;
    Check(table.Arm(Make(1, WatchCondition::CombatEnd), line, fighting, start.unixMs + 5000, start) ==
              CommandRefusal::None,
          "expire: armed for 5 s");
    Check(Scan(table, reader, At(4999)).empty() && table.Size() == 1, "expire: still fighting before until");
    Check(Scan(table, reader, At(5000)).empty() && table.Empty() && table.Counters().expired == 1 &&
              !table.Counters().fired && !table.Counters().dropped,
          "expire: until without the change drops silently, no wake");
    unsigned const reads = reader.reads;
    Check(!table.Due(At(6000)), "expire: no scan due afterwards");
    Check(Scan(table, reader, At(6000)).empty() && reader.reads == reads,
          "expire: an expired watch is never read again");

    reader.facts = idle;  // The condition held, but the time ran out before the debounce ended.
    Check(table.Arm(Make(2, WatchCondition::CombatEnd), line, fighting, start.unixMs + 6000, start) ==
              CommandRefusal::None,
          "expire: second watch armed");
    Check(
        Scan(table, reader, At(4500)).empty() && Scan(table, reader, At(6000)).empty() && table.Counters().expired == 2,
        "expire: the time beats an unfinished debounce");

    Check(table.Arm(Make(3, WatchCondition::Time), line, idle, start.unixMs + 7000, start) == CommandRefusal::None,
          "time: armed for 7 s");
    Check(Scan(table, reader, At(6999)).empty(), "time: not before until");
    auto const due = Scan(table, reader, At(7000));
    Check(due.size() == 1 && due[0].condition == WatchCondition::Time && table.Counters().expired == 2,
          "time: fires at until, not expired");
}

void WakeRate()
{
    WatchTable table;
    Reader reader;
    Check(table.Arm(Make(1, WatchCondition::Time), line, idle, start.unixMs + 5000, start) == CommandRefusal::None,
          "rate: armed");
    auto due = Scan(table, reader, At(5000));
    Check(due.size() == 1, "rate: first wake due");
    table.Woke(due[0], true, At(5000));
    // The wake route re-arms at once (depth 1), and another bot waits for the same moment.
    Check(table.Arm(Make(1, WatchCondition::Time), {1, start.unixMs}, idle, At(5000).unixMs + 5000, At(5000)) ==
                  CommandRefusal::None &&
              table.Arm(Make(2, WatchCondition::Time), line, idle, At(5000).unixMs + 5000, At(5000)) ==
                  CommandRefusal::None,
          "rate: re-armed, and a second bot armed");
    due = Scan(table, reader, At(10000));
    Check(due.size() == 1 && due[0].botGuid == 2 && table.Find(1), "rate: a bot woken 5 s ago waits; others do not");
    table.Woke(due[0], true, At(10000));
    Check(Scan(table, reader, At(14999)).empty() && table.Find(1), "rate: still waiting 9999 ms after its wake");
    due = Scan(table, reader, At(15000));
    Check(due.size() == 1 && due[0].botGuid == 1 && due[0].depth == 2, "rate: fires once 10 s passed");
    table.Woke(due[0], false, At(15000));
    Check(table.Counters().fired == 2 && table.Counters().dropped == 1, "rate: a wake that did not open is dropped");
    Check(table.Arm(Make(1, WatchCondition::Time), {2, start.unixMs}, idle, At(15000).unixMs + 5000, At(15000)) ==
              CommandRefusal::None,
          "rate: armed after a failed open");
    Check(Scan(table, reader, At(20000)).size() == 1, "rate: a failed open sets no wake window");
}

void Cancellation()
{
    WatchTable table;
    Reader reader;
    auto const arm = [&](uint64_t bot, uint64_t partner)
    {
        Check(table.Arm(Make(bot, WatchCondition::Time, partner), line, idle, start.unixMs + 60000, start) ==
                  CommandRefusal::None,
              "cancel: armed");
    };
    arm(1, 900);
    arm(2, 901);
    arm(3, 1);  // Bot 1 is bot 3's partner.
    arm(4, 902);
    table.Cancel(901);
    Check(table.Size() == 3 && !table.Find(2) && table.Counters().dropped == 1, "cancel: the partner's logout");
    table.Cancel(1);
    Check(table.Size() == 1 && !table.Find(1) && !table.Find(3) && table.Counters().dropped == 3,
          "cancel: a logout ends watches as bot and as partner");
    reader.readable = false;  // Bot gone, out of the world, in a battleground or arena, partner offline.
    Check(Scan(table, reader, At(500)).empty() && table.Empty() && table.Counters().dropped == 4,
          "cancel: an unreadable bot is dropped at the next scan");
    reader.readable = true;
    arm(5, 900);
    reader.facts = WatchFacts{};
    Check(Scan(table, reader, At(1000)).empty() && table.Empty() && table.Counters().dropped == 5,
          "cancel: a bot not ready is dropped at the next scan");

    reader.facts = idle;
    arm(6, 900);
    arm(7, 900);
    table.Woke(Make(8, WatchCondition::Time), true, At(1000));
    table.Retire();
    Check(table.Empty() && table.Counters().dropped == 7 && !table.Due(At(5000)),
          "retire: every watch ends with its epoch, counted dropped");
    Check(table.Arm(Make(8, WatchCondition::Time), line, idle, start.unixMs + 6000, start) == CommandRefusal::None &&
              Scan(table, reader, At(6000)).size() == 1,
          "retire: wake windows end with the epoch too");
}

void Due()
{
    WatchTable table;
    Reader reader;
    Check(!table.Due(start) && !table.Due(At(100000)), "due: an empty table never asks for a scan");
    Check(table.Arm(Make(1, WatchCondition::Time), line, idle, start.unixMs + 60000, start) == CommandRefusal::None,
          "due: armed");
    Check(table.Due(At(0)), "due: a watch makes a scan due");
    Check(Scan(table, reader, At(0)).empty() && reader.reads == 1, "due: one read per watch per scan");
    Check(!table.Due(At(499)) && table.Due(At(500)), "due: scans at most every 500 ms");
    Check(table.Clear(1) && !table.Due(At(10000)), "due: emptied, no scan is due");
    Check(Scan(table, reader, At(10000)).empty() && reader.reads == 1, "due: a scan of an empty table reads nothing");
}
}  // namespace

int main()
{
    try
    {
        Words();
        Admission();
        Firing();
        Expiry();
        WakeRate();
        Cancellation();
        Due();
        std::cout << "PASS " << checks << " watch table checks\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL after " << checks << ": " << error.what() << '\n';
        return 1;
    }
}
