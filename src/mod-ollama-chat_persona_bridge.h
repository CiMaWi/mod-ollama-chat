#ifndef MOD_OLLAMA_CHAT_PERSONA_BRIDGE_H
#define MOD_OLLAMA_CHAT_PERSONA_BRIDGE_H

#include <cstdint>
#include <string>

#include "mod-ollama-chat_persona_store.h"

namespace OllamaBridge
{
struct WorldTicket;
}

// World thread only. Queues the authenticated provider's card alternative for the bot of a generation ticket that
// still authorizes: the caller holds its consumed claim, and a fresh capture must still match the captured view,
// including the card revision and stamp the generation was given. Provenance is the ticket's provider. Queued means
// staged, not persisted. Defined with the bridge world adapter in mod-ollama-chat_handler.cpp.
OllamaPersona::ProposalStatus OllamaChat_BridgeProposeCard(OllamaBridge::WorldTicket const& ticket,
                                                           uint64_t expectedRevision, uint64_t stamp, std::string body);

#endif
