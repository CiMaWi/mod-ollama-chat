#ifndef MOD_OLLAMA_CHAT_COMMAND_H
#define MOD_OLLAMA_CHAT_COMMAND_H

#include "ScriptMgr.h"
#include "Chat.h"

class OllamaChatConfigCommand : public CommandScript
{
public:
    OllamaChatConfigCommand();
    Acore::ChatCommands::ChatCommandTable GetCommands() const override;

    static bool HandleOllamaReloadCommand(ChatHandler* handler);
    static bool HandleOllamaLabLogoutCommand(ChatHandler* handler, Optional<std::string> botName);

    // Diagnostics. Half of what was wrong with this module was invisible
    // because there was no way to ask it what it thought it was doing.
    static bool HandleOllamaStatusCommand(ChatHandler* handler);
};

#endif // MOD_OLLAMA_CHAT_COMMAND_H
