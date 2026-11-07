#ifndef MOD_OLLAMA_CHAT_BRIDGE_PLAN_H
#define MOD_OLLAMA_CHAT_BRIDGE_PLAN_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "mod-ollama-chat_bridge_intent.h"

// J12 lasting-plan ledger: pure bookkeeping, no game, playerbots, clock or transport types. It records, per bot and
// plan kind, the restore target (the state before the bridge's first change, or a person's later change), the value
// the bridge set last, the owner (group, partner, generation) and the expiry, and turns expiry, group or
// partner change and retirement into restore-pending entries the world tick restores. Value is the game side's
// state type for one plan kind; its operator== compares that kind's state. World-thread only; the steady clock in
// milliseconds is passed in. Memory only: a restart forgets it. While Empty(), every per-tick call is O(1) and
// allocates nothing; otherwise LiveBots() is O(entries x bots) and Pending() allocates, both bounded by
// MaxPlanEntries.
namespace OllamaBridge
{
// One applied plan lasts ten minutes from its latest application (person-paced, never lingers across a dungeon); a
// repeat of the same kind renews it. A code constant, no config option.
constexpr int64_t PlanLifetimeMs = 10 * 60 * 1000;
constexpr std::size_t MaxPlanEntries = 256;  // 64 world scopes x four plan kinds.
// A failed restore is tried again after PlanRestoreRetryMs, at most MaxPlanRestoreAttempts times in all, so a brief
// failure (a teleport or loading screen) is retried for about twenty seconds instead of three world updates.
constexpr uint8_t MaxPlanRestoreAttempts = 3;
constexpr int64_t PlanRestoreRetryMs = 10 * 1000;

enum class PlanKind : uint8_t
{
    Movement,  // Follow and stay share one playerbots movement state, so they share one baseline.
    Formation,
    SaveMana,
    FocusHealing,
    Count
};

// The lasting verbs. One-shot verbs (attack, flee, position, mark: raid icons are group state another bot or person
// may move, so a per-bot restore could not compose) and refused verbs (DS.9) have no plan.
inline std::optional<PlanKind> PlanKindFor(ChannelIntent intent)
{
    switch (intent)
    {
        case ChannelIntent::Follow:
        case ChannelIntent::Stay:
            return PlanKind::Movement;
        case ChannelIntent::Formation:
            return PlanKind::Formation;
        case ChannelIntent::SaveMana:
            return PlanKind::SaveMana;
        case ChannelIntent::FocusHealing:
            return PlanKind::FocusHealing;
        default:
            return std::nullopt;
    }
}

struct PlanKey
{
    uint64_t botGuid = 0;
    PlanKind kind = PlanKind::Count;
};

inline bool operator==(PlanKey const& left, PlanKey const& right)
{
    return left.botGuid == right.botGuid && left.kind == right.kind;
}

struct PlanOwner
{
    uint64_t groupGuid = 0;
    uint64_t controllerGuid = 0;  // DS.14: the conversation partner.
    uint64_t generation = 0;
    // DS.14: the plan's admission needed the partner in the bot's group (follow, formation, save_mana), so
    // it ends when the partner leaves that group; other plans (a stay, a focus heal) hold on the bot's group
    // and the partner being online. Per plan, not per owner: Admit ignores it, a renewal takes the new rule.
    bool partnerInGroup = false;
};

// Unknown kinds and zero guids fail closed.
inline bool ValidPlanKey(PlanKey const& key) { return key.botGuid && key.kind < PlanKind::Count; }

// DS.14: group 0 is an ungrouped bot's plan (a stay); it ends when its partner logs out, the bot joins a group, or at
// expiry.
inline bool ValidPlanOwner(PlanOwner const& owner) { return owner.controllerGuid && owner.generation; }

enum class PlanAdmission : uint8_t
{
    Refused,  // Mutate nothing.
    Capture,  // First plan of this kind for the bot: the state before the mutation becomes the restore target.
    Renew     // Same kind again: replace the value and renew the expiry; the restore target stays unless a person
              // changed the state since the bridge's last change.
};

struct PlanCounters
{
    uint64_t applied = 0;     // New entries, baseline captured.
    uint64_t renewed = 0;     // Repeats of a live entry.
    uint64_t expired = 0;     // Live -> restore-pending at the deadline (or on a clock rollback).
    uint64_t regrouped = 0;   // Live -> restore-pending on a group change or the partner's loss.
    uint64_t retired = 0;     // Live -> restore-pending on retirement or a generation change.
    uint64_t restored = 0;    // Pending entries the game side restored (or found superseded by a later change).
    uint64_t departed = 0;    // Entries dropped unrestored because the bot logged out.
    uint64_t abandoned = 0;   // Pending entries dropped after MaxPlanRestoreAttempts failed restores.
    uint64_t unrecorded = 0;  // Attempts that may have written but left no entry (NoteUnrecorded): no restore.

    bool operator==(PlanCounters const&) const = default;
};

// The owner of a bot's live plans (one owner per bot), for the tick's group, partner and logout checks.
struct PlanLiveBot
{
    uint64_t botGuid = 0;
    PlanOwner owner;
};

template <typename Value>
struct PlanRestore
{
    PlanKey key;
    Value baseline;
    Value applied;
    uint8_t attempts = 0;
};

template <typename Value>
class PlanLedger
{
public:
    explicit PlanLedger(std::size_t capacity = MaxPlanEntries) : _capacity(std::min(capacity, MaxPlanEntries)) {}

    // The current bridge generation; 0 closes admission. Live entries of another generation become restore-pending.
    void Open(uint64_t generation)
    {
        if (generation != _generation)
            RetireLive();
        _generation = generation;
    }

    // Retirement, reload or switch: every live entry becomes restore-pending and admission closes until Open.
    void RetireAll()
    {
        RetireLive();
        _generation = 0;
    }

    // A query: decides whether an order may mutate and whether it needs a baseline first. One owner per bot; a bot
    // with a restore-pending entry takes no new plan until it is restored; a full table refuses and never evicts.
    PlanAdmission Admit(PlanKey const& key, PlanOwner const& owner, int64_t nowMs) const
    {
        // ValidPlanOwner refuses generation 0, so a closed ledger admits nothing.
        if (owner.generation != _generation || !ValidPlanKey(key) || !ValidPlanOwner(owner) || nowMs < 0 ||
            nowMs > std::numeric_limits<int64_t>::max() - PlanLifetimeMs)
            return PlanAdmission::Refused;
        bool found = false;
        for (Entry const& entry : _entries)
        {
            if (entry.key.botGuid != key.botGuid)
                continue;
            // Live entries carry the open generation (Open retires the others), so owner means group and partner.
            if (entry.pending || entry.owner.groupGuid != owner.groupGuid ||
                entry.owner.controllerGuid != owner.controllerGuid)
                return PlanAdmission::Refused;
            if (entry.key == key)
            {
                if (Due(entry, nowMs))
                    return PlanAdmission::Refused;  // Restore first; a late repeat never revives an expired plan.
                found = true;
            }
        }
        if (found)
            return PlanAdmission::Renew;
        return _entries.size() < _capacity ? PlanAdmission::Capture : PlanAdmission::Refused;
    }

    // After the game side attempted the mutation, with the same nowMs as Admit. before is the kind's state captured
    // just before the mutation, applied the state the bridge left. One rule for the restore target: it is the state
    // before the bridge's first change, unless the state before a repeat differs from what the bridge set last; then a
    // person changed it in between, and that change becomes the target. Returns false and records nothing when refused.
    bool Record(PlanKey const& key, PlanOwner const& owner, Value const& before, Value const& applied, int64_t nowMs)
    {
        switch (Admit(key, owner, nowMs))
        {
            case PlanAdmission::Capture:
                _entries.push_back({key, owner, before, applied, nowMs, nowMs + PlanLifetimeMs, false, 0, 0});
                ++_counters.applied;
                return true;
            case PlanAdmission::Renew:
                for (Entry& entry : _entries)
                {
                    if (entry.key == key)
                    {
                        if (!(before == entry.applied))
                            entry.baseline = before;
                        entry.applied = applied;
                        entry.appliedMs = nowMs;
                        entry.deadlineMs = nowMs + PlanLifetimeMs;
                        // A follow renewed by a stay holds as a stay, and the other way round.
                        entry.owner.partnerInGroup = owner.partnerInGroup;
                        ++_counters.renewed;
                        return true;
                    }
                }
                return false;
            default:
                return false;
        }
    }

    // Exactly at the deadline a plan expires; a steady clock that went backwards expires it too (never extends).
    std::size_t Expire(int64_t nowMs)
    {
        std::size_t moved = 0;
        for (Entry& entry : _entries)
        {
            if (!entry.pending && Due(entry, nowMs))
            {
                entry.pending = true;
                ++moved;
            }
        }
        _counters.expired += moved;
        return moved;
    }

    // The bot's current group (0 when ungrouped), whether its plans' partner is online and whether the partner is in
    // that group. A changed group or an offline partner makes the bot's live entries restore-pending; a partner
    // outside the group does so only for entries whose admission needed the partner in it (PlanOwner).
    std::size_t Observe(uint64_t botGuid, uint64_t groupGuid, bool partnerOnline, bool partnerInGroup)
    {
        std::size_t moved = 0;
        for (Entry& entry : _entries)
        {
            if (!entry.pending && entry.key.botGuid == botGuid &&
                (entry.owner.groupGuid != groupGuid || !partnerOnline ||
                 (entry.owner.partnerInGroup && !partnerInGroup)))
            {
                entry.pending = true;
                ++moved;
            }
        }
        _counters.regrouped += moved;
        return moved;
    }

    // The bot logged out: nothing can be restored. Drops every entry of the bot, live or pending.
    std::size_t Departed(uint64_t botGuid)
    {
        std::size_t const before = _entries.size();
        std::erase_if(_entries, [botGuid](Entry const& entry) { return entry.key.botGuid == botGuid; });
        std::size_t const dropped = before - _entries.size();
        _counters.departed += dropped;
        return dropped;
    }

    // Restore-pending entries due now, in application order, for the world tick's restore pass: never tried, or
    // PlanRestoreRetryMs after the last failure (a steady clock that went backwards retries at once, never stalls).
    std::vector<PlanRestore<Value>> Pending(int64_t nowMs) const
    {
        std::vector<PlanRestore<Value>> result;
        for (Entry const& entry : _entries)
            if (entry.pending && RetryDue(entry, nowMs))
                result.push_back({entry.key, entry.baseline, entry.applied, entry.attempts});
        return result;
    }

    // The game side restored the entry (or left a person's later change alone). Once per entry: a second call, or a
    // call for a live entry, returns false.
    bool Restored(PlanKey const& key)
    {
        auto const it = FindPending(key);
        if (it == _entries.end())
            return false;
        _entries.erase(it);
        ++_counters.restored;
        return true;
    }

    // A failed restore stays pending and is due again PlanRestoreRetryMs later, until MaxPlanRestoreAttempts; then it
    // is dropped and counted.
    bool RestoreFailed(PlanKey const& key, int64_t nowMs)
    {
        auto const it = FindPending(key);
        if (it == _entries.end())
            return false;
        if (++it->attempts >= MaxPlanRestoreAttempts)
        {
            _entries.erase(it);
            ++_counters.abandoned;
            return true;
        }
        it->failedMs = std::max<int64_t>(nowMs, 0);
        return true;
    }

    // Distinct bots with live entries and their owner, in application order, for the world tick's group, partner
    // and logout checks.
    std::vector<PlanLiveBot> LiveBots() const
    {
        std::vector<PlanLiveBot> bots;
        for (Entry const& entry : _entries)
            if (!entry.pending && std::none_of(bots.begin(), bots.end(), [&entry](PlanLiveBot const& bot)
                                               { return bot.botGuid == entry.key.botGuid; }))
                bots.push_back({entry.key.botGuid, entry.owner});
        return bots;
    }

    // What the bridge set last for a live entry; nullopt when the entry is not live.
    std::optional<Value> LiveApplied(PlanKey const& key) const
    {
        for (Entry const& entry : _entries)
            if (!entry.pending && entry.key == key)
                return entry.applied;
        return std::nullopt;
    }

    // The caller attempted a mutation it could not record (the state after it unreadable, or Record refused): that
    // effect has no restore. Counted only, so the watchlist line shows it.
    void NoteUnrecorded() { ++_counters.unrecorded; }

    // No live and no pending entry: the only state in which the bridge holds no capture to restore.
    bool Empty() const { return _entries.empty(); }
    std::size_t Size() const { return _entries.size(); }
    uint64_t Generation() const { return _generation; }
    PlanCounters const& Counters() const { return _counters; }

private:
    struct Entry
    {
        PlanKey key;
        PlanOwner owner;
        Value baseline;
        Value applied;
        int64_t appliedMs = 0;
        int64_t deadlineMs = 0;
        bool pending = false;
        uint8_t attempts = 0;
        int64_t failedMs = 0;  // Steady time of the last failed restore, never negative.
    };

    static bool Due(Entry const& entry, int64_t nowMs) { return nowMs >= entry.deadlineMs || nowMs < entry.appliedMs; }

    static bool RetryDue(Entry const& entry, int64_t nowMs)
    {
        return !entry.attempts || nowMs < entry.failedMs || nowMs - entry.failedMs >= PlanRestoreRetryMs;
    }

    void RetireLive()
    {
        for (Entry& entry : _entries)
        {
            if (!entry.pending)
            {
                entry.pending = true;
                ++_counters.retired;
            }
        }
    }

    typename std::vector<Entry>::iterator FindPending(PlanKey const& key)
    {
        return std::find_if(_entries.begin(), _entries.end(),
                            [&key](Entry const& entry) { return entry.pending && entry.key == key; });
    }

    std::size_t const _capacity;
    uint64_t _generation = 0;
    std::vector<Entry> _entries;
    PlanCounters _counters;
};
}  // namespace OllamaBridge

#endif
