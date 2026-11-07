#ifndef MOD_OLLAMA_CHAT_BRIDGE_WATCH_H
#define MOD_OLLAMA_CHAT_BRIDGE_WATCH_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mod-ollama-chat_bridge.h"
#include "mod-ollama-chat_bridge_intent.h"

// DS.14 (c), J6e-c: intentions. The bridge keeps what a bot meant to do; the module keeps only a bounded watch that
// says when to wake the bot again. No action, target or authority is stored: a wake opens a fresh generation route,
// and any act on it passes the whole current path (capture, intent, effect, playerbots, rate, replay, deadline).
namespace OllamaBridge
{
enum class DispatchSource : uint8_t;  // bridge_world.h; a watch stores the value only.

constexpr int64_t WatchMinMs = 5000;       // Shortest wait: untilUnixMs - now.
constexpr int64_t WatchMaxMs = 600000;     // Longest wait, and the whole chain's cap after the player's line.
constexpr uint8_t WatchMaxDepth = 3;       // Wakes in one chain.
constexpr std::size_t MaxWatches = 64;     // One per bot; MaxWorldScopes (asserted in bridge_world.h).
constexpr int64_t WatchScanMs = 500;       // Scan interval of the post-map-join update.
constexpr int64_t WatchSettleMs = 1000;    // An observation counts only this long after arming.
constexpr int64_t WatchDebounceMs = 2000;  // A condition must hold this long; a re-pull starts it again.
constexpr int64_t WakeIntervalMs = 10000;  // At most one wake per bot in this window.
constexpr uint8_t ActivityIdle = 0;        // ActivityCode of an idle or dead bot.

enum class WatchCondition : uint8_t
{
    CombatEnd,
    ActivityDone,
    Time,
    None  // Clears the bot's watch.
};

enum class WatchOutcome : uint8_t
{
    Armed,
    Cleared,
    Rejected
};

inline bool ParseWatchCondition(std::string_view name, WatchCondition& condition)
{
    constexpr std::pair<std::string_view, WatchCondition> names[] = {{"combat_end", WatchCondition::CombatEnd},
                                                                     {"activity_done", WatchCondition::ActivityDone},
                                                                     {"time", WatchCondition::Time},
                                                                     {"none", WatchCondition::None}};
    for (auto const& [text, value] : names)
        if (name == text)
        {
            condition = value;
            return true;
        }
    return false;
}

inline char const* WatchConditionName(WatchCondition condition)
{
    switch (condition)
    {
        case WatchCondition::CombatEnd:
            return "combat_end";
        case WatchCondition::ActivityDone:
            return "activity_done";
        case WatchCondition::Time:
            return "time";
        case WatchCondition::None:
            return "none";
    }
    return "none";
}

// The fixed in-world sentence a wake turn opens with (`input.text`; never mechanism words).
inline char const* WakeText(WatchCondition condition)
{
    switch (condition)
    {
        case WatchCondition::CombatEnd:
            return "Your fight is over.";
        case WatchCondition::ActivityDone:
            return "You have finished what you were doing.";
        default:
            return "The time you meant to wait has passed.";
    }
}

// The bot's facts at arming and at each scan, read on the world thread by the game adapter (J5f's reader).
struct WatchFacts
{
    bool ready = false;  // The bot a playerbot in the world outside battlegrounds and arenas; its partner online.
    bool inCombat = false;
    uint8_t activity = ActivityIdle;  // ActivityCode of what the bot is doing.
};

// What the arming route knows about its own chain; set by the module, never by the wire.
struct WatchRoute
{
    uint8_t depth = 0;         // Wakes before this route: 0 for the player's own line.
    int64_t originUnixMs = 0;  // The player's line that started the chain; 0 is unknown and refused.
};

struct Watch
{
    std::string id;  // The wire commandId (one replay reservation).
    uint64_t botGuid = 0;
    uint64_t partnerGuid = 0;
    DispatchSource source{};
    std::string channel;
    std::string contextId;
    WatchCondition condition = WatchCondition::Time;
    uint8_t activity = ActivityIdle;  // At arming.
    uint8_t depth = 1;                // The wake this watch opens, 1..WatchMaxDepth.
    int64_t originUnixMs = 0;
    int64_t armedSteadyMs = 0;
    int64_t untilUnixMs = 0;
    int64_t untilSteadyMs = 0;
    int64_t holdsSinceMs = -1;  // Steady time of the first settled observation of the condition; -1 while not held.
};

struct WatchCounters
{
    uint64_t armed = 0;
    uint64_t fired = 0;
    uint64_t expired = 0;
    uint64_t dropped = 0;
    uint64_t rejected = 0;

    bool operator==(WatchCounters const&) const = default;
};

// World-thread table of plain values: one watch per bot, at most MaxWatches. Nothing here reads the game; the
// caller passes facts it read, and Scan asks for them once per watch.
class WatchTable
{
public:
    // Arms (a new watch replaces the bot's old one) or refuses with a reason; condition None is Clear's.
    CommandRefusal Arm(Watch watch, WatchRoute const& route, WatchFacts const& facts, int64_t untilUnixMs, Clock now)
    {
        CommandRefusal const refusal = Admission(watch, route, facts, untilUnixMs, now);
        if (refusal != CommandRefusal::None)
        {
            ++_counters.rejected;
            return refusal;
        }
        watch.activity = facts.activity;
        watch.depth = static_cast<uint8_t>(route.depth + 1);
        watch.originUnixMs = route.originUnixMs;
        watch.armedSteadyMs = now.steadyMs;
        watch.untilUnixMs = untilUnixMs;
        watch.untilSteadyMs = now.steadyMs + (untilUnixMs - now.unixMs);
        watch.holdsSinceMs = -1;
        auto const it = Locate(watch.botGuid);
        if (it != _watches.end())
            *it = std::move(watch);
        else
            _watches.push_back(std::move(watch));
        ++_counters.armed;
        return CommandRefusal::None;
    }

    // `none`: the bot lets its watch go (true when it had one). Not a drop: the bot chose it.
    bool Clear(uint64_t botGuid)
    {
        auto const it = Locate(botGuid);
        if (it == _watches.end())
            return false;
        _watches.erase(it);
        return true;
    }

    // Logout of guid as bot or partner: its watches end at once (counted dropped).
    void Cancel(uint64_t guid)
    {
        _counters.dropped += std::erase_if(
            _watches, [&](Watch const& watch) { return watch.botGuid == guid || watch.partnerGuid == guid; });
    }

    // Epoch retirement or reload: every watch ends with its epoch (counted dropped).
    void Retire()
    {
        _counters.dropped += _watches.size();
        _watches.clear();
        _wakes.clear();
    }

    // A scan is due: a watch exists and the last scan is WatchScanMs old. An empty table never asks for one.
    bool Due(Clock now) const { return !_watches.empty() && now.steadyMs - _lastScanMs >= WatchScanMs; }

    // One scan: read(watch, facts) once per watch. A watch whose condition held through the debounce, or whose time
    // came, leaves the table into due (a bot woken in the last WakeIntervalMs waits); one whose time passed first
    // expires silently; one whose bot or partner is gone, or whose bot is in a battleground, is dropped.
    template <typename Read>
    void Scan(Clock now, Read&& read, std::vector<Watch>& due)
    {
        due.clear();
        _lastScanMs = now.steadyMs;
        std::erase_if(_wakes, [&](auto const& wake) { return now.steadyMs >= wake.second; });
        for (std::size_t i = 0; i < _watches.size();)
        {
            WatchFacts facts;
            bool const ready = read(static_cast<Watch const&>(_watches[i]), facts) && facts.ready;
            Verdict const verdict = Judge(_watches[i], ready, facts, now);
            if (verdict == Verdict::Keep)
            {
                ++i;
                continue;
            }
            if (verdict == Verdict::Fire)
                due.push_back(std::move(_watches[i]));
            else
                ++(verdict == Verdict::Expire ? _counters.expired : _counters.dropped);
            _watches.erase(_watches.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    // After the caller tried to open a due watch's wake: fired, or dropped when no wake opened (capture failed).
    void Woke(Watch const& watch, bool opened, Clock now)
    {
        if (!opened)
        {
            ++_counters.dropped;
            return;
        }
        ++_counters.fired;
        _wakes.emplace_back(watch.botGuid, now.steadyMs + WakeIntervalMs);
    }

    Watch const* Find(uint64_t botGuid) const
    {
        auto const it = std::find_if(_watches.begin(), _watches.end(),
                                     [&](Watch const& watch) { return watch.botGuid == botGuid; });
        return it == _watches.end() ? nullptr : &*it;
    }
    bool Empty() const { return _watches.empty(); }
    std::size_t Size() const { return _watches.size(); }
    WatchCounters const& Counters() const { return _counters; }

private:
    enum class Verdict : uint8_t
    {
        Keep,
        Fire,
        Expire,
        Drop
    };

    std::vector<Watch>::iterator Locate(uint64_t botGuid)
    {
        return std::find_if(_watches.begin(), _watches.end(),
                            [&](Watch const& watch) { return watch.botGuid == botGuid; });
    }

    CommandRefusal Admission(Watch const& watch, WatchRoute const& route, WatchFacts const& facts, int64_t untilUnixMs,
                             Clock now) const
    {
        if (watch.condition == WatchCondition::None || !watch.botGuid || !watch.partnerGuid || !facts.ready ||
            route.originUnixMs <= 0 || untilUnixMs < 0 || now.unixMs < 0)
            return CommandRefusal::CannotNow;
        int64_t const wait = untilUnixMs - now.unixMs;
        if (wait < WatchMinMs)
            return CommandRefusal::WaitTooShort;
        if (wait > WatchMaxMs)
            return CommandRefusal::WaitTooLong;
        if (route.depth >= WatchMaxDepth || untilUnixMs > route.originUnixMs + WatchMaxMs)
            return CommandRefusal::PutOff;
        if (watch.condition == WatchCondition::CombatEnd && !facts.inCombat)
            return CommandRefusal::NotFighting;
        if (watch.condition == WatchCondition::ActivityDone && facts.activity == ActivityIdle)
            return CommandRefusal::NotBusy;
        if (!Find(watch.botGuid) && _watches.size() >= MaxWatches)
            return CommandRefusal::CannotNow;
        return CommandRefusal::None;
    }

    bool Waking(uint64_t botGuid) const
    {
        return std::any_of(_wakes.begin(), _wakes.end(), [&](auto const& wake) { return wake.first == botGuid; });
    }

    Verdict Judge(Watch& watch, bool ready, WatchFacts const& facts, Clock now) const
    {
        if (!ready)
            return Verdict::Drop;
        bool const timeUp = now.steadyMs >= watch.untilSteadyMs || now.unixMs >= watch.untilUnixMs;
        bool held = timeUp;
        if (watch.condition != WatchCondition::Time)
        {
            if (timeUp)
                return Verdict::Expire;  // No model call: the moment passed without the change.
            bool const holds =
                watch.condition == WatchCondition::CombatEnd ? !facts.inCombat : facts.activity != watch.activity;
            if (!holds || now.steadyMs - watch.armedSteadyMs < WatchSettleMs)
                watch.holdsSinceMs = -1;  // An action of the arming turn never counts, and a re-pull starts again.
            else if (watch.holdsSinceMs < 0)
                watch.holdsSinceMs = now.steadyMs;
            held = watch.holdsSinceMs >= 0 && now.steadyMs - watch.holdsSinceMs >= WatchDebounceMs;
        }
        return held && !Waking(watch.botGuid) ? Verdict::Fire : Verdict::Keep;
    }

    std::vector<Watch> _watches;
    std::vector<std::pair<uint64_t, int64_t>> _wakes;  // Bot and the steady time its next wake may open.
    int64_t _lastScanMs = std::numeric_limits<int64_t>::min() / 2;
    WatchCounters _counters;  // Survive Retire, like the command counters.
};
}  // namespace OllamaBridge

#endif
