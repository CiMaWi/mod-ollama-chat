#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_random.h"
#include "mod-ollama-chat_command.h"
#include "mod-ollama-chat_delete.h"
#include "mod-ollama-chat_expression.h"
#include "Log.h"

void Addmod_ollama_chatScripts()
{
    LOG_INFO("server.loading", "[Ollama Chat] Registering mod-ollama-chat scripts.");
    new OllamaChatConfigWorldScript();
    new PlayerBotChatHandler();
    new OllamaChatMaintenance();
    new OllamaBotRandomChatter();

    // Bots react when a player emotes at them.
    new ChatOnEmote();

    new OllamaChatConfigCommand();

    // A deleted character's rows (consolidation CS27c).
    AddOllamaChatDeleteScripts();
}
