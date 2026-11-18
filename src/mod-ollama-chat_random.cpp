#include "mod-ollama-chat_random.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_sentiment.h"
#include "mod-ollama-chat_memory.h"
#include "mod-ollama-chat-utilities.h"

#include "Channel.h"
#include "ChannelMgr.h"
#include "Guild.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "AiFactory.h"
#include "BridgeIntentEffect.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/core.h>

#include <ctime>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

OllamaBotRandomChatter::OllamaBotRandomChatter() : WorldScript("OllamaBotRandomChatter") { }

void OllamaBotRandomChatter::OnUpdate(uint32 diff)
{
    // The module's world tick. These run before any feature toggle can return
    // early, because pending replies still have to be delivered even when
    // random chatter itself is switched off.
    // World::Update has joined MapMgr::Update's map workers before
    // ScriptMgr::OnWorldUpdate calls this hook, so this frame is the
    // post-map-join phase typed bridge effects need. Only this stack
    // holds the token. It runs even with the module disabled: the reload
    // that disabled it retired the bridge, and only this tick restores the
    // bots' retired lasting plans (J12); the dispatcher does nothing else then.
    PlayerbotBridge::PostMapJoin const join;
    OllamaDispatch_Update(diff, join);

    if (!g_Enable)
        return;

    static uint32 maintenanceTimer = 0;
    if (maintenanceTimer <= diff)
    {
        maintenanceTimer = 30000;
        Governor_Update();
    }
    else
    {
        maintenanceTimer -= diff;
    }

    if (g_ConversationHistorySaveInterval > 0)
    {
        const time_t now = time(nullptr);
        if (difftime(now, g_LastHistorySaveTime) >= g_ConversationHistorySaveInterval * 60)
        {
            SaveBotConversationHistoryToDB();
            g_LastHistorySaveTime = now;
        }
    }

    if (g_MemoryEnable && g_MemorySaveInterval > 0)
    {
        const time_t now = time(nullptr);
        if (difftime(now, g_LastMemorySaveTime) >= g_MemorySaveInterval * 60)
        {
            Memory_SaveAll();
            g_LastMemorySaveTime = now;
        }
    }

    if (g_EnableSentimentTracking && g_SentimentSaveInterval > 0)
    {
        const time_t now = time(nullptr);
        if (difftime(now, g_LastSentimentSaveTime) >= g_SentimentSaveInterval * 60)
        {
            SaveBotPlayerSentimentsToDB();
            g_LastSentimentSaveTime = now;
        }
    }
}

