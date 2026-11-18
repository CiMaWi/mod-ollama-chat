#include "mod-ollama-chat_command.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_response.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat-utilities.h"
#include "mod-ollama-chat_lab.h"
#include "Log.h"
#include "DatabaseEnv.h"
#include "Chat.h"
#include "Config.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "PlayerbotAI.h"
#include "RandomPlayerbotMgr.h"
#include <fmt/core.h>

using namespace Acore::ChatCommands;

OllamaChatConfigCommand::OllamaChatConfigCommand()
    : CommandScript("OllamaChatConfigCommand")
{
}

ChatCommandTable OllamaChatConfigCommand::GetCommands() const
{
    // J5e: lab-only test helpers (no config option; administrators and the console).
    static ChatCommandTable ollamaLabCommandTable =
    {
        { "logout", HandleOllamaLabLogoutCommand, SEC_ADMINISTRATOR, Console::Yes }
    };

    static ChatCommandTable ollamaReloadCommandTable =
    {
        { "reload",      HandleOllamaReloadCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "status",      HandleOllamaStatusCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "lab",         ollamaLabCommandTable }
    };

    static ChatCommandTable commandTable =
    {
        { "ollama", ollamaReloadCommandTable }
    };

    return commandTable;
}

// J5e: `.ollama lab logout <botName>` logs one bot out through the holder that owns it (its master's, else the
// random-bot holder). Commands run on the world thread; the result goes to the console or the issuing GM only.
bool OllamaChatConfigCommand::HandleOllamaLabLogoutCommand(ChatHandler* handler, Optional<std::string> botName)
{
    std::string const name = botName ? *botName : std::string();
    Player* player = name.empty() ? nullptr : ObjectAccessor::FindPlayerByName(name);
    PlayerbotAI* ai = player ? PlayerbotsMgr::instance().GetPlayerbotAI(player) : nullptr;
    bool const bot = ai && ai->IsBotAI();
    PlayerbotHolder* holder = nullptr;
    if (bot)
    {
        ObjectGuid const guid = player->GetGUID();
        Player* master = ai->GetMaster();
        PlayerbotMgr* owner = master ? PlayerbotsMgr::instance().GetPlayerbotMgr(master) : nullptr;
        if (owner && owner->GetPlayerBot(guid))
            holder = owner;
        else if (sRandomPlayerbotMgr.GetPlayerBot(guid))
            holder = &sRandomPlayerbotMgr;
    }
    auto const step = OllamaLab::DecideLogout(!name.empty(), player != nullptr, bot, holder != nullptr);
    std::string const shown = player ? player->GetName() : name;
    if (step == OllamaLab::LogoutStep::LoggedOut)
        holder->LogoutPlayerBot(player->GetGUID());
    std::string const message = OllamaLab::LogoutMessage(step, shown);
    LOG_INFO("module.ollamachat", "{}", message);
    handler->SendSysMessage(message);
    return true;
}

bool OllamaChatConfigCommand::HandleOllamaReloadCommand(ChatHandler* handler)
{
    sConfigMgr->Reload();
    LoadOllamaChatConfig();
    Roleplay_Load();

    handler->SendSysMessage("OllamaChat: Configuration reloaded from conf!");
    return true;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

bool OllamaChatConfigCommand::HandleOllamaStatusCommand(ChatHandler* handler)
{
    const OllamaDispatchStats dispatch = OllamaDispatch_GetStats();
    const GovernorStats       gov      = Governor_GetStats();

    handler->PSendSysMessage("|cff00ff00[Ollama Chat] Status|r");
    handler->PSendSysMessage("Module: {}   Bridge: {}", g_Enable ? "enabled" : "DISABLED",
                             OllamaBridge::BridgeUsable(g_Enable, g_OllamaBridgeSettings)
                                 ? "configured"
                                 : "idle (no valid OllamaChat.Bridge.Instance, Endpoint, Token)");

    handler->PSendSysMessage("Dispatcher: {} workers, {} queued, {} in flight, {} awaiting delivery",
                             dispatch.workers, dispatch.queuedRequests,
                             dispatch.inFlight, dispatch.pendingDeliveries);
    handler->PSendSysMessage("Totals: {} submitted, {} delivered, {} failed",
                             (unsigned long long)dispatch.totalSubmitted,
                             (unsigned long long)dispatch.totalDelivered,
                             (unsigned long long)dispatch.totalFailed);
    handler->PSendSysMessage("Dropped: {} queue-full, {} empty-after-cleanup, {} by governor",
                             (unsigned long long)dispatch.totalDroppedQueueFull,
                             (unsigned long long)dispatch.totalDroppedEmpty,
                             (unsigned long long)dispatch.totalDroppedGovernor);

    handler->PSendSysMessage("Governor: {} bots, {} scopes tracked, {} sends in the last minute",
                             gov.trackedBots, gov.trackedScopes, gov.sendsLastMinute);
    handler->PSendSysMessage("Blocked: {} cooldown, {} rate, {} repetition, {} chain-depth, {} no-audience",
                             gov.blockedCooldown, gov.blockedRate, gov.blockedRepetition,
                             gov.blockedChainDepth, gov.blockedNoAudience);

    handler->PSendSysMessage("Roleplay: {} (strictness {})   Emote reactions: {}",
                             g_RoleplayEnable ? "on" : "off",
                             (uint32)g_RoleplayStrictness,
                             g_EnableEmoteReactions ? "on" : "off");

    if (!dispatch.lastError.empty())
        handler->PSendSysMessage("|cffff0000Last error:|r {}", dispatch.lastError);
    else
        handler->PSendSysMessage("Last error: none");

    return true;
}
