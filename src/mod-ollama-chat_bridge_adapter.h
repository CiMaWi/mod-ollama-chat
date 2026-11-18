#ifndef MOD_OLLAMA_CHAT_BRIDGE_ADAPTER_H
#define MOD_OLLAMA_CHAT_BRIDGE_ADAPTER_H

#include <string>

#include "mod-ollama-chat_handler.h"

class Player;
namespace OllamaBridge
{
struct Watch;
}

// A connected human in the world: only a human's line opens a bridge request.
bool BridgeHuman(Player* player);

// World thread: a human's line to a bot (or a fired watch's wake) opens a bridge generation on the conversation's
// binding; false when the bridge does not take it. The rest of the adapter is declared in mod-ollama-chat_dispatch.h.
bool BridgeSubmit(Player* bot, Player* speaker, ChatChannelSourceLocal source, std::string const& text, bool addressed,
                  std::string const& scopeKey, OllamaBridge::Watch const* wake = nullptr);

#endif  // MOD_OLLAMA_CHAT_BRIDGE_ADAPTER_H
