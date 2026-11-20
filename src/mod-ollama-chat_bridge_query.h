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
    virtual bool GroupMembers(WorldTicket const& ticket, uint64_t groupGuid, std::vector<uint64_t>& members) = 0;
    virtual bool ResolveVisible(WorldTicket const& ticket, uint64_t guid, VisibleUnit& unit) = 0;
    // J5f-b: one bot_state topic of the ticket's own bot, raw (FinishTopic filters and bounds it); false when the
    // bot cannot be read. Never another unit's state. WT18b: place is the route topic's place ("" for the others).
    virtual bool ReadTopic(WorldTicket const& /*ticket*/, StateTopic /*topic*/, std::string const& /*place*/,
                           std::vector<StateEntry>& entries)
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
    // LC20b: the bots' past the store packet writes; null: the module has no store (every write refused unavailable).
    virtual StoreWorld* Store() { return nullptr; }
};

// The observable_group read: the ticket's group members the bot can perceive, with UI health only. LCB6b: no
// currentness check of its own; the channel checks the conversation once just before it (WorldChannel::Live).
class FilteredQueryAdapter
{
public:
    explicit FilteredQueryAdapter(QueryWorldSource& source) : _source(source) {}
    bool Units(WorldTicket const& ticket, DispatchGuard const& guard, std::vector<Participant>& result);

private:
    QueryWorldSource& _source;
};

// J5g: OllamaChat.DisableRepliesInCombat spares a local model in combat. A bridge reply to a human's line is exempt
// (DS.14: a fighting bot answers, often by acting alone); all other chatter keeps the check.
inline bool CombatSkipsReply(bool disableInCombat, bool bridgeHumanLine, bool inCombat)
{
    return disableInCombat && !bridgeHumanLine && inCombat;
}
}  // namespace OllamaBridge

#endif
