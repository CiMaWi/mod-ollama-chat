#ifndef MOD_OLLAMA_CHAT_BRIDGE_QUERY_H
#define MOD_OLLAMA_CHAT_BRIDGE_QUERY_H

#include "mod-ollama-chat_bridge_world.h"

namespace OllamaBridge
{
// Synchronous WORLD-THREAD adapter only. Implementations re-resolve GUIDs on every call, check current
// map/instance/phase/range/detection, and return false for missing or imperceptible units. Never wire input
// from a native request into this interface. It owns no game pointers and is never sent to an I/O worker.
struct VisibleUnit
{
    uint64_t guid = 0;
    uint64_t groupGuid = 0;
    std::string name;
    int32_t healthPercent = -1;  // Current observable UI health, or unknown. No role/target inference seam.
};

class QueryWorldSource
{
public:
    virtual ~QueryWorldSource() = default;
    virtual bool Observe(WorldTicket const& ticket, DispatchGuard& guard) = 0;
    virtual bool GroupMembers(WorldTicket const& ticket, uint64_t groupGuid, std::vector<uint64_t>& members) = 0;
    virtual bool ResolveVisible(WorldTicket const& ticket, uint64_t guid, VisibleUnit& unit) = 0;
    virtual bool ReadMark(WorldTicket const& ticket, uint8_t icon, uint64_t& targetGuid) = 0;
    // J5f-b: one bot_state topic of the ticket's own bot, raw (FinishTopic filters and bounds it); false when the
    // bot cannot be read. Never another unit's state.
    virtual bool ReadTopic(WorldTicket const& /*ticket*/, StateTopic /*topic*/, std::vector<StateEntry>& entries)
    {
        entries.clear();
        return false;
    }
    // J6e-c: what a watch needs of botGuid (ready, combat, activity code); false when the bot cannot be read or is not
    // ready (offline, out of the world, in a battleground or arena, partner offline). Never another unit's state.
    virtual bool ReadWatch(uint64_t /*botGuid*/, uint64_t /*partnerGuid*/, WatchFacts& facts)
    {
        facts = {};
        return false;
    }
};

// These are owned observations, NOT authority. A publisher must call Current immediately before exposing
// any serialized payload, and must separately validate its game-reserved route and selected channel epoch.
struct QueryObservation
{
    WorldTicket ticket;
    DispatchGuard guard;
    Deadline until;
    std::vector<Participant> units;
};

struct MarkObservation
{
    WorldTicket ticket;
    DispatchGuard guard;
    Deadline until;
    std::vector<ChannelObservedMark> marks;
};

class FilteredQueryAdapter
{
public:
    FilteredQueryAdapter(WorldPolicy& world, QueryWorldSource& source, ReadClock clock = Now);
    bool CaptureQuery(WorldTicket const& ticket, DispatchGuard const& guard, QueryObservation& result);
    bool Current(QueryObservation const& observation);
    // icons are supplied by a trusted game hook, never by a received model message. Only current visible
    // marks are captured; removals and other event kinds have no schema. One icon appears at most once.
    bool CaptureMarks(WorldTicket const& ticket, DispatchGuard const& guard, std::vector<uint8_t> const& icons,
                      MarkObservation& result);
    bool Current(MarkObservation const& observation);

private:
    bool Fresh(WorldTicket const& ticket, DispatchGuard const& guard);
    bool Units(WorldTicket const& ticket, DispatchGuard const& guard, std::vector<Participant>& result);
    bool Marks(WorldTicket const& ticket, std::vector<uint8_t> const& icons, std::vector<ChannelObservedMark>& result);
    WorldPolicy& _world;
    QueryWorldSource& _source;
    ReadClock const _clock;
};

// J5g: OllamaChat.DisableRepliesInCombat spares a local model in combat. A bridge reply to a human's line is exempt
// (DS.14: a fighting bot answers, often by acting alone); all other chatter keeps the check.
inline bool CombatSkipsReply(bool disableInCombat, bool bridgeHumanLine, bool inCombat)
{
    return disableInCombat && !bridgeHumanLine && inCombat;
}
}  // namespace OllamaBridge

#endif
