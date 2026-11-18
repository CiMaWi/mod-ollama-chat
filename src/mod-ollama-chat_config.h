#ifndef MOD_OLLAMA_CHAT_CONFIG_H
#define MOD_OLLAMA_CHAT_CONFIG_H

#include <string>
#include <cstdint>
#include <vector>
#include <deque>
#include <unordered_map>
#include <set>
#include <utility>
#include <mutex>
#include <ctime>
#include "mod-ollama-chat_bridge.h"
#include "ScriptMgr.h"  // Ensure WorldScript is defined

// --------------------------------------------
// Distance/Range Configuration
// --------------------------------------------
extern float      g_SayDistance;
extern float      g_YellDistance;

// --------------------------------------------
// Bot/Player Chatter Probability & Limits
// --------------------------------------------
// Per-channel-type reply chances
extern uint32_t   g_PlayerReplyChance_Say;
extern uint32_t   g_BotReplyChance_Say;
extern uint32_t   g_PlayerReplyChance_Channel;
extern uint32_t   g_BotReplyChance_Channel;
extern uint32_t   g_PlayerReplyChance_Party;
extern uint32_t   g_BotReplyChance_Party;
extern uint32_t   g_PlayerReplyChance_Guild;
extern uint32_t   g_BotReplyChance_Guild;

extern uint32_t   g_MaxBotsToPick;

// --------------------------------------------
// Bridge link
// --------------------------------------------

extern OllamaBridge::Settings g_OllamaBridgeSettings;  // World-thread load/publish only.

// --------------------------------------------
// Feature Toggles & Core Settings
// --------------------------------------------
extern bool        g_Enable;
extern bool        g_DisableRepliesInCombat;
extern bool        g_EnableWhisperReplies;
extern bool        g_DebugEnabled;

extern uint32_t g_DispatchWorkerThreads;
extern uint32_t g_MaxQueueDepth;

// --------------------------------------------
// Response post-processing
// --------------------------------------------
extern uint32_t g_MaxReplyLength;
extern bool     g_ResponseStripMarkdown;
extern bool     g_ResponseStripDecorativeUnicode;

// --------------------------------------------
// Conversation governor
// --------------------------------------------
extern uint8_t  g_MaxChainDepth;
extern uint32_t g_ChainChanceDecayPct;
extern bool     g_RequireRecentHuman;
extern uint32_t g_HumanWindowSeconds;
extern uint32_t g_BotCooldownSeconds;
extern uint32_t g_ScopeCooldownSeconds;
// Party/raid groups at or below this many members count as direct address,
// so a bot answering a person in them is not held back by the ambient
// pacing cooldowns. 0 disables the group case entirely.
extern uint32_t g_DirectAddressGroupSize;
// How long after a bot answers someone that person keeps its attention in
// that scope. 0 disables the open-conversation rule.
extern uint32_t g_ConversationWindowSeconds;
// Reply chance used instead of the ambient per-channel chance when the line
// is aimed at this bot.
extern uint32_t g_DirectAddressReplyChance;
extern uint32_t g_ScopeMessagesPerMinute;
extern uint32_t g_GlobalMessagesPerMinute;
extern uint32_t g_BotHistorySize;
extern uint32_t g_ScopeHistorySize;
extern float    g_RepetitionSimilarityThreshold;
extern uint32_t g_RepetitionWindowSeconds;
extern uint32_t g_OpenerHistorySize;

// --------------------------------------------
// Roleplay mode
// --------------------------------------------
extern bool        g_RoleplayEnable;
extern uint8_t     g_RoleplayStrictness;
extern bool        g_RoleplayUseRaceVoice;
extern bool        g_RoleplayUseClassVoice;
extern bool        g_RoleplayFactionAttitude;
extern bool        g_RoleplayBlockMetaTerms;
extern std::string g_RoleplayMetaTermList;
extern bool        g_RoleplayCrossFactionGibberish;

// --------------------------------------------
// Embodiment: facing, gestures, emote reactions
// --------------------------------------------
// How long a looping emote state (dance) is allowed to run before the module
// clears it. Nothing else clears it for a bot: the core only does so on a
// movement packet from a real client. 0 leaves it set forever.
extern uint32_t g_StateEmoteDurationMs;

// TEXT_EMOTE_* to play when the model supplies a gesture name the table does
// not know. Resolved from its config name on the world thread at load, because
// ExtractEmoteTag runs on a worker and must not read a config string. 0 = none.
extern uint32_t g_EmoteFallbackId;

extern bool     g_EnableBotFacing;
extern bool     g_EnableBotEmotes;
extern uint32_t g_BotExpressionDelayMs;
extern float    g_BotFacingMaxDistance;
extern bool     g_EnableEmoteReactions;
extern uint32_t g_EmoteReplyChance;
extern uint32_t g_EmoteReplyMirrorWeight;
extern uint32_t g_EmoteReplyCounterWeight;
extern uint32_t g_EmoteReactionCooldownSeconds;

// --------------------------------------------
// Channel Disable Settings
// --------------------------------------------
extern bool g_DisableForCustomChannels;
extern bool g_DisableForSayYell;
extern bool g_DisableForGuild;
extern bool g_DisableForParty;

// --------------------------------------------
// Typing Simulation Settings
// --------------------------------------------
extern bool g_EnableTypingSimulation;
extern uint32_t g_TypingSimulationBaseDelay;      // Base delay in milliseconds
extern uint32_t g_TypingSimulationDelayPerChar;   // Delay per character in milliseconds
extern uint32_t g_TypingSimulationMaxDelay;       // Ceiling, so a long reply is not lost

// --------------------------------------------
// Loader Functions
// --------------------------------------------
void LoadOllamaChatConfig();

// --------------------------------------------
// Declaration of the configuration WorldScript.
// --------------------------------------------
class OllamaChatConfigWorldScript : public WorldScript
{
public:
    OllamaChatConfigWorldScript();
    void OnStartup() override;
    void OnShutdown() override;
};

#endif // MOD_OLLAMA_CHAT_CONFIG_H
