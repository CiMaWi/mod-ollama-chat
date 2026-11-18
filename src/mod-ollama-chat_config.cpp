#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_expression.h"
#include "Config.h"
#include "Log.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_bridge_listener.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_persona_db.h"
#include "mod-ollama-chat_roleplay.h"

// --------------------------------------------
// Distance/Range Configuration
// --------------------------------------------
float      g_SayDistance       = 30.0f;
float      g_YellDistance      = 100.0f;

// --------------------------------------------
// Bot/Player Chatter Probability & Limits
// --------------------------------------------
// Per-channel-type reply chances
uint32_t   g_PlayerReplyChance_Say     = 90;
uint32_t   g_BotReplyChance_Say        = 10;
uint32_t   g_PlayerReplyChance_Channel = 50;
uint32_t   g_BotReplyChance_Channel    = 5;
uint32_t   g_PlayerReplyChance_Party   = 90;
uint32_t   g_BotReplyChance_Party      = 10;
uint32_t   g_PlayerReplyChance_Guild   = 70;
uint32_t   g_BotReplyChance_Guild      = 5;

uint32_t   g_MaxBotsToPick     = 2;

// --------------------------------------------
// Bridge link
// --------------------------------------------
OllamaBridge::Settings g_OllamaBridgeSettings;

// --------------------------------------------
// Feature Toggles & Core Settings
// --------------------------------------------
bool        g_Enable                          = true;
bool        g_DisableRepliesInCombat          = true;
bool        g_EnableWhisperReplies            = false;
bool        g_DebugEnabled                    = false;

uint32_t g_DispatchWorkerThreads  = 4;
uint32_t g_MaxQueueDepth          = 64;

// --------------------------------------------
// Response post-processing
// --------------------------------------------
uint32_t g_MaxReplyLength                = 240;
bool     g_ResponseStripMarkdown         = true;
bool     g_ResponseStripDecorativeUnicode = true;

// --------------------------------------------
// Conversation governor
// --------------------------------------------
uint8_t  g_MaxChainDepth                 = 3;
uint32_t g_ChainChanceDecayPct           = 50;
bool     g_RequireRecentHuman            = true;
uint32_t g_HumanWindowSeconds            = 120;
uint32_t g_BotCooldownSeconds            = 45;
uint32_t g_ScopeCooldownSeconds          = 15;
uint32_t g_DirectAddressGroupSize        = 5;
uint32_t g_ConversationWindowSeconds     = 180;
uint32_t g_DirectAddressReplyChance      = 95;
uint32_t g_ScopeMessagesPerMinute        = 8;
uint32_t g_GlobalMessagesPerMinute       = 40;
uint32_t g_BotHistorySize                = 12;
uint32_t g_ScopeHistorySize              = 30;
float    g_RepetitionSimilarityThreshold = 0.72f;
uint32_t g_RepetitionWindowSeconds       = 1800;
uint32_t g_OpenerHistorySize             = 8;

// --------------------------------------------
// Roleplay mode
// --------------------------------------------
bool        g_RoleplayEnable               = false;
uint8_t     g_RoleplayStrictness           = 1;
bool        g_RoleplayUseRaceVoice         = true;
bool        g_RoleplayUseClassVoice        = true;
bool        g_RoleplayFactionAttitude      = true;
bool        g_RoleplayBlockMetaTerms       = true;
std::string g_RoleplayMetaTermList;
bool        g_RoleplayCrossFactionGibberish = true;

// --------------------------------------------
// Embodiment
// --------------------------------------------
bool        g_EnableBotFacing              = true;
bool        g_EnableBotEmotes              = true;
uint32_t    g_StateEmoteDurationMs         = 15000;
uint32_t    g_EmoteFallbackId              = 0;
uint32_t    g_BotExpressionDelayMs         = 400;
float       g_BotFacingMaxDistance         = 40.0f;
bool        g_EnableEmoteReactions         = true;
uint32_t    g_EmoteReplyChance             = 60;
uint32_t    g_EmoteReplyMirrorWeight       = 55;
uint32_t    g_EmoteReplyCounterWeight      = 30;
uint32_t    g_EmoteReactionCooldownSeconds = 20;

// --------------------------------------------
// Channel Disable Settings
// --------------------------------------------
bool g_DisableForCustomChannels = false;
bool g_DisableForSayYell = false;
bool g_DisableForGuild = false;
bool g_DisableForParty = false;

// --------------------------------------------
// Typing Simulation Settings
// --------------------------------------------
bool g_EnableTypingSimulation = false;
uint32_t g_TypingSimulationBaseDelay = 1000;     // 1000ms base delay
uint32_t g_TypingSimulationDelayPerChar = 250;
uint32_t g_TypingSimulationMaxDelay     = 8000;   // 250ms per character (4 chars/sec)


void LoadOllamaChatConfig()
{
    g_SayDistance                     = sConfigMgr->GetOption<float>("OllamaChat.SayDistance", 30.0f);
    g_YellDistance                    = sConfigMgr->GetOption<float>("OllamaChat.YellDistance", 100.0f);
    
    // Load per-channel-type reply chances
    g_PlayerReplyChance_Say           = sConfigMgr->GetOption<uint32_t>("OllamaChat.PlayerReplyChance.Say", 90);
    g_BotReplyChance_Say              = sConfigMgr->GetOption<uint32_t>("OllamaChat.BotReplyChance.Say", 10);
    g_PlayerReplyChance_Channel       = sConfigMgr->GetOption<uint32_t>("OllamaChat.PlayerReplyChance.Channel", 50);
    g_BotReplyChance_Channel          = sConfigMgr->GetOption<uint32_t>("OllamaChat.BotReplyChance.Channel", 5);
    g_PlayerReplyChance_Party         = sConfigMgr->GetOption<uint32_t>("OllamaChat.PlayerReplyChance.Party", 90);
    g_BotReplyChance_Party            = sConfigMgr->GetOption<uint32_t>("OllamaChat.BotReplyChance.Party", 10);
    g_PlayerReplyChance_Guild         = sConfigMgr->GetOption<uint32_t>("OllamaChat.PlayerReplyChance.Guild", 70);
    g_BotReplyChance_Guild            = sConfigMgr->GetOption<uint32_t>("OllamaChat.BotReplyChance.Guild", 5);
    
    g_MaxBotsToPick                   = sConfigMgr->GetOption<uint32_t>("OllamaChat.MaxBotsToPick", 2);

    g_OllamaBridgeSettings.instance = sConfigMgr->GetOption<std::string>("OllamaChat.Bridge.Instance", "");
    g_OllamaBridgeSettings.provider = "claude";  // The wire's provider field: the one bridge there is.
    g_OllamaBridgeSettings.endpoint = sConfigMgr->GetOption<std::string>("OllamaChat.Bridge.Endpoint", "");
    g_OllamaBridgeSettings.token = sConfigMgr->GetOption<std::string>("OllamaChat.Bridge.Token", "");

    g_Enable                          = sConfigMgr->GetOption<bool>("OllamaChat.Enable", true);
    g_DisableRepliesInCombat          = sConfigMgr->GetOption<bool>("OllamaChat.DisableRepliesInCombat", true);
    g_EnableWhisperReplies            = sConfigMgr->GetOption<bool>("OllamaChat.EnableWhisperReplies", false);

    g_DebugEnabled                    = sConfigMgr->GetOption<bool>("OllamaChat.DebugEnabled", false);

    g_DispatchWorkerThreads = sConfigMgr->GetOption<uint32_t>("OllamaChat.WorkerThreads", 4);
    g_MaxQueueDepth                   = sConfigMgr->GetOption<uint32_t>("OllamaChat.MaxQueueDepth", 64);

    // --- Response pipeline ----------------------------------------------
    g_MaxReplyLength                  = sConfigMgr->GetOption<uint32_t>("OllamaChat.MaxReplyLength", 240);
    g_ResponseStripMarkdown           = sConfigMgr->GetOption<bool>("OllamaChat.StripMarkdown", true);
    g_ResponseStripDecorativeUnicode  = sConfigMgr->GetOption<bool>("OllamaChat.StripDecorativeUnicode", true);

    // --- Conversation governor -------------------------------------------
    g_MaxChainDepth                   = static_cast<uint8_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.BotConversation.MaxChainDepth", 3));
    g_ChainChanceDecayPct             = sConfigMgr->GetOption<uint32_t>("OllamaChat.BotConversation.ChanceDecayPct", 50);
    g_RequireRecentHuman              = sConfigMgr->GetOption<bool>("OllamaChat.BotConversation.RequireRecentHuman", true);
    g_HumanWindowSeconds              = sConfigMgr->GetOption<uint32_t>("OllamaChat.BotConversation.HumanWindowSeconds", 120);
    g_BotCooldownSeconds              = sConfigMgr->GetOption<uint32_t>("OllamaChat.Cooldown.PerBotSeconds", 45);
    g_ScopeCooldownSeconds            = sConfigMgr->GetOption<uint32_t>("OllamaChat.Cooldown.PerScopeSeconds", 15);
    g_DirectAddressGroupSize          = sConfigMgr->GetOption<uint32_t>("OllamaChat.Cooldown.DirectAddressGroupSize", 5);
    g_ConversationWindowSeconds       = sConfigMgr->GetOption<uint32_t>("OllamaChat.Cooldown.ConversationWindowSeconds", 180);
    g_DirectAddressReplyChance        = sConfigMgr->GetOption<uint32_t>("OllamaChat.DirectAddressReplyChance", 95);
    g_ScopeMessagesPerMinute          = sConfigMgr->GetOption<uint32_t>("OllamaChat.RateLimit.ScopePerMinute", 8);
    g_GlobalMessagesPerMinute         = sConfigMgr->GetOption<uint32_t>("OllamaChat.RateLimit.GlobalPerMinute", 40);
    g_BotHistorySize                  = sConfigMgr->GetOption<uint32_t>("OllamaChat.Repetition.BotHistorySize", 12);
    g_ScopeHistorySize                = sConfigMgr->GetOption<uint32_t>("OllamaChat.Repetition.ScopeHistorySize", 30);
    g_RepetitionSimilarityThreshold   = sConfigMgr->GetOption<float>("OllamaChat.Repetition.SimilarityThreshold", 0.72f);
    g_RepetitionWindowSeconds         = sConfigMgr->GetOption<uint32_t>("OllamaChat.Repetition.WindowSeconds", 1800);
    g_OpenerHistorySize               = sConfigMgr->GetOption<uint32_t>("OllamaChat.Repetition.OpenerHistorySize", 8);

    // --- Roleplay --------------------------------------------------------
    g_RoleplayEnable                  = sConfigMgr->GetOption<bool>("OllamaChat.Roleplay.Enable", false);
    g_RoleplayStrictness              = static_cast<uint8_t>(sConfigMgr->GetOption<uint32_t>("OllamaChat.Roleplay.Strictness", 1));
    g_RoleplayUseRaceVoice            = sConfigMgr->GetOption<bool>("OllamaChat.Roleplay.UseRaceVoice", true);
    g_RoleplayUseClassVoice           = sConfigMgr->GetOption<bool>("OllamaChat.Roleplay.UseClassVoice", true);
    g_RoleplayFactionAttitude         = sConfigMgr->GetOption<bool>("OllamaChat.Roleplay.FactionAttitude", true);
    g_RoleplayBlockMetaTerms          = sConfigMgr->GetOption<bool>("OllamaChat.Roleplay.BlockMetaTerms", true);
    g_RoleplayMetaTermList            = sConfigMgr->GetOption<std::string>("OllamaChat.Roleplay.MetaTermList",
        "dps|nerf|proc|patch|server|realm|respec|mmo|npc|toon|irl|lfg|afk|noob|meta|aggro|dot|hot|aoe|wipe|hitbox|hotfix|expansion");
    g_RoleplayCrossFactionGibberish   = sConfigMgr->GetOption<bool>("OllamaChat.Roleplay.CrossFactionGibberish", true);

    // --- Embodiment ------------------------------------------------------
    g_EnableBotFacing                 = sConfigMgr->GetOption<bool>("OllamaChat.EnableBotFacing", true);
    g_EnableBotEmotes                 = sConfigMgr->GetOption<bool>("OllamaChat.EnableBotEmotes", true);
    g_StateEmoteDurationMs            = sConfigMgr->GetOption<uint32_t>("OllamaChat.StateEmoteDurationMs", 15000);

    // Resolved to an id here, on the world thread. The worker that parses a
    // reply must never touch a config string -- reload reassigns them.
    {
        const std::string fallbackName =
            sConfigMgr->GetOption<std::string>("OllamaChat.EmoteFallback", "talk");
        g_EmoteFallbackId = fallbackName.empty() ? 0 : LookupTextEmoteId(fallbackName);

        if (!fallbackName.empty() && g_EmoteFallbackId == 0)
        {
            LOG_INFO("module.ollamachat",
                     "[Ollama Chat] OllamaChat.EmoteFallback '{}' is not a known emote name; "
                     "unresolved gesture tags will play nothing.", fallbackName);
        }
    }
    g_BotExpressionDelayMs            = sConfigMgr->GetOption<uint32_t>("OllamaChat.BotExpressionDelayMs", 400);
    g_BotFacingMaxDistance            = sConfigMgr->GetOption<float>("OllamaChat.Facing.MaxDistance", 40.0f);
    g_EnableEmoteReactions            = sConfigMgr->GetOption<bool>("OllamaChat.EnableEmoteReactions", true);
    g_EmoteReplyChance                = sConfigMgr->GetOption<uint32_t>("OllamaChat.EmoteReplyChance", 60);
    g_EmoteReplyMirrorWeight          = sConfigMgr->GetOption<uint32_t>("OllamaChat.EmoteReply.MirrorWeight", 55);
    g_EmoteReplyCounterWeight         = sConfigMgr->GetOption<uint32_t>("OllamaChat.EmoteReply.CounterWeight", 30);
    g_EmoteReactionCooldownSeconds    = sConfigMgr->GetOption<uint32_t>("OllamaChat.EmoteReply.CooldownSeconds", 20);

    // Typing Simulation
    g_EnableTypingSimulation          = sConfigMgr->GetOption<bool>("OllamaChat.EnableTypingSimulation", false);
    g_TypingSimulationBaseDelay       = sConfigMgr->GetOption<uint32_t>("OllamaChat.TypingSimulationBaseDelay", 1000);
    g_TypingSimulationDelayPerChar    = sConfigMgr->GetOption<uint32_t>("OllamaChat.TypingSimulationDelayPerChar", 250);
    g_TypingSimulationMaxDelay        = sConfigMgr->GetOption<uint32_t>("OllamaChat.TypingSimulationMaxDelay", 8000);

    // Publish the bridge settings for worker threads. They must never read
    // g_OllamaBridgeSettings directly -- reassigning it here would free the
    // buffers under a worker mid-request.
    OllamaConfig_Publish();
    bool const personaCardsEnabled = sConfigMgr->GetOption<bool>("OllamaChat.Bridge.PersonaCardsEnabled", false);
    OllamaPersona::DiscardStagedPersonaWrites();
    OllamaPersona::WorldStore().Configure(g_Enable && personaCardsEnabled);
    // Explicit world-thread load/reload notification also retires work when the module is disabled.
    // In that state the world tick still restores the retired lasting plans and does nothing else.
    bool const channelEnabled = sConfigMgr->GetOption<bool>("OllamaChat.Bridge.ChannelEnabled", false);
    OllamaDispatch_BridgeConfigured(channelEnabled);
    // J5d: the mailbox's 127.0.0.1 transport; a reload restarts it only when the port or the token changed.
    OllamaDispatch_BridgeListen(channelEnabled, sConfigMgr->GetOption<uint32_t>("OllamaChat.Bridge.ChannelPort",
                                                                                OllamaBridge::DefaultListenerPort));
    if (g_Enable && !OllamaBridge::BridgeUsable(g_Enable, g_OllamaBridgeSettings))
        LOG_INFO("module.ollamachat",
                 "[Ollama Chat] Enabled without valid bridge settings (OllamaChat.Bridge.Instance, "
                 "Endpoint, Token): idle, no listener, no workers, nothing is generated.");

    // Channel disable settings
    g_DisableForCustomChannels = sConfigMgr->GetOption<bool>("OllamaChat.DisableForCustomChannels", false);
    g_DisableForSayYell = sConfigMgr->GetOption<bool>("OllamaChat.DisableForSayYell", false);
    g_DisableForGuild = sConfigMgr->GetOption<bool>("OllamaChat.DisableForGuild", false);
    g_DisableForParty = sConfigMgr->GetOption<bool>("OllamaChat.DisableForParty", false);

}

// Definition of the configuration WorldScript.
OllamaChatConfigWorldScript::OllamaChatConfigWorldScript() : WorldScript("OllamaChatConfigWorldScript") { }

void OllamaChatConfigWorldScript::OnStartup()
{
    LoadOllamaChatConfig();
    OllamaPersona::StartPersonaDb();
    Roleplay_Load();
}

void OllamaChatConfigWorldScript::OnUpdate(uint32 /*diff*/) { OllamaPersona::PumpPersonaDb(); }

void OllamaChatConfigWorldScript::OnShutdown()
{
    // Stop accepting work and join the workers before anything they touch goes
    // away. Detached threads had no such guarantee.
    OllamaDispatch_Stop();
    OllamaPersona::StopPersonaDb();
}
