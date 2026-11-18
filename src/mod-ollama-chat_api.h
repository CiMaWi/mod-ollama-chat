#ifndef MOD_OLLAMA_CHAT_API_H
#define MOD_OLLAMA_CHAT_API_H

#include <optional>
#include <string>

#include "mod-ollama-chat_bridge.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_httpclient.h"

// Result of one bridge generation call.
struct OllamaApiResult
{
    std::string text;
    bool ok = false;
    std::string error;
    std::optional<std::string> personaProposal;  // bridge card proposal, never chat text
    bool silent = false;                         // bridge action-only answer (DS.14): ok with empty text

    bool empty() const { return text.empty(); }
};

// Immutable snapshot of what a bridge call needs from config. `.ollama reload` reassigns the settings strings on the
// world thread, which frees the old buffers underneath any worker mid-copy; config is therefore published once under
// a mutex and workers take a copy.
struct OllamaEndpointSettings
{
    // Owned local-link configuration. No native credential is read by this module.
    OllamaBridge::Settings bridge;
};

// Republish from the config globals. Call on the world thread after config load or reload.
void OllamaConfig_Publish();

// Thread-safe copy for a worker.
OllamaEndpointSettings OllamaConfig_Snapshot();

// Capture settings on world submission and carry that owned value with queued work.
// The returned text is a validated generation intent, never an execution acknowledgement.
// Delivery must still revalidate activation, audience, authority and expiry on the world thread.
// Blocking. Call from a worker thread, never from the world thread.
OllamaApiResult QueryOllamaBridge(OllamaEndpointSettings const& settings, OllamaBridge::Request const& request);

// Trusted world-owner lifecycle seam, never called by selection files or bridge input.
// Retirement/drain/restoration checks belong to that owner before activating an identity.
void OllamaBridge_SetActivation(OllamaBridge::Identity identity);

#endif // MOD_OLLAMA_CHAT_API_H
