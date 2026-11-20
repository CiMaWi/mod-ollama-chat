#include <cstdint>

#include "BotPastMgr.h"
#include "BotRecordMgr.h"
#include "ObjectGuid.h"
#include "RandomPlayerbotMgr.h"
#include "mod-ollama-chat_bridge_channel.h"
#include "mod-ollama-chat_dispatch.h"

namespace
{
// LC20b: the store packet's world: copies of playerbots' in-memory past and record, writes through BotPastMgr (its
// own lock and an asynchronous write). World thread (WorldChannel::Update); no call queries the database.
class BridgeStoreWorld final : public OllamaBridge::StoreWorld
{
public:
    bool Known(uint64_t botGuid) override
    {
        ObjectGuid const guid(botGuid);
        return guid.IsPlayer() && (sRandomPlayerbotMgr.IsRandomBot(guid.GetCounter()) ||
                                   sRandomPlayerbotMgr.IsAddclassBot(guid.GetCounter()));
    }

    uint32_t SharedId(uint64_t botGuid, uint8_t ordinal) override
    {
        for (PastRules::Statement const& statement :
             BotPastMgr::Instance().Statements(ObjectGuid(botGuid).GetCounter()))
            if (statement.Ordinal == ordinal)
                return statement.SharedId;
        return 0;
    }

    bool InEvent(uint64_t botGuid, uint32_t eventId) override
    {
        for (RecordRules::Row const& row : BotRecordMgr::Instance().Rows(ObjectGuid(botGuid).GetCounter()))
            if (row.EventId == eventId)
                return true;
        return false;
    }

    PastRules::Refusal Apply(OllamaBridge::StoreOp op, uint32_t eventId, OllamaBridge::StoreRow const& row) override
    {
        uint32 const counter = ObjectGuid(row.botGuid).GetCounter();
        auto& past = BotPastMgr::Instance();
        switch (op)
        {
            case OllamaBridge::StoreOp::BackgroundSet:
                return past.SetStatement(counter, {row.ordinal, row.text, row.version, row.sharedId, row.source});
            case OllamaBridge::StoreOp::BackgroundRevise:
                return past.Revise(counter, row.ordinal, row.version, row.text);
            case OllamaBridge::StoreOp::ColourSet:
                return past.SetColour(counter, {eventId, row.text, row.version});
        }
        return PastRules::Refusal::Missing;
    }
};
}  // namespace

OllamaBridge::StoreWorld& OllamaChat_BridgeStoreWorld()
{
    static BridgeStoreWorld store;
    return store;
}
