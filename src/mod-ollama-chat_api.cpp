#include "mod-ollama-chat_api.h"

#include <mutex>
#include <utility>

#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_httpclient.h"

namespace
{
    std::mutex             g_settingsMutex;
    OllamaEndpointSettings g_settings;
    OllamaBridge::Circuit g_bridgeCircuit;
}

void OllamaConfig_Publish()
{
    OllamaEndpointSettings next;
    next.bridge = g_OllamaBridgeSettings;

    std::lock_guard<std::mutex> lock(g_settingsMutex);
    g_settings = std::move(next);
}

OllamaEndpointSettings OllamaConfig_Snapshot()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    return g_settings;
}

void OllamaBridge_SetActivation(OllamaBridge::Identity identity)
{
    // Trusted game owner only, after retirement/drain/restoration. Publishing config
    // deliberately does not call this, grant authority or acknowledge selection intent.
    g_bridgeCircuit.Reset(std::move(identity));
}

OllamaApiResult QueryOllamaBridge(OllamaEndpointSettings const& settings, OllamaBridge::Request const& request)
{
    OllamaApiResult result;
    if (!OllamaBridge::ValidSettings(settings.bridge))
    {
        result.error = "bridge_invalid_configuration";
        return result;
    }
    auto bridge = OllamaBridge::Generate(
        settings.bridge, request, g_bridgeCircuit, OllamaBridge::Now,
        [](OllamaBridge::Settings const& link, std::string const& body, int64_t remainingMs)
        { return OllamaHttp_PostBridge(link.endpoint, body, OllamaBridge::Headers(link), remainingMs); });
    result.ok = bridge.ok;
    result.text = std::move(bridge.text);
    result.error = std::move(bridge.error);
    result.personaProposal = std::move(bridge.personaProposal);
    result.silent = bridge.silent;
    return result;
}
