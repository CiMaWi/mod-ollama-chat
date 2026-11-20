#include "mod-ollama-chat_random.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"

#include "BridgeIntentEffect.h"

OllamaBotRandomChatter::OllamaBotRandomChatter() : WorldScript("OllamaBotRandomChatter") { }

void OllamaBotRandomChatter::OnUpdate(uint32 diff)
{
    // The module's world tick. The dispatcher runs before the enable check:
    // World::Update has joined MapMgr::Update's map workers before
    // ScriptMgr::OnWorldUpdate calls this hook, so this frame is the
    // post-map-join phase typed bridge effects need. Only this stack
    // holds the token. With the module disabled the dispatcher only logs
    // its watch counters and returns.
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

}

