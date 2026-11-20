#include "mod-ollama-chat_api.h"

#include <mutex>
#include <utility>

#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_httpclient.h"

namespace
{
    std::mutex             g_settingsMutex;
    OllamaEndpointSettings g_settings;
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

OllamaApiResult QueryOllamaBridge(OllamaEndpointSettings const& settings, OllamaBridge::Request const& request)
{
    OllamaApiResult result;
    if (!OllamaBridge::ValidSettings(settings.bridge))
    {
        result.error = "bridge_invalid_configuration";
        return result;
    }
    auto bridge = OllamaBridge::Generate(
        settings.bridge, request, OllamaBridge::Now,
        [](OllamaBridge::Settings const& link, std::string const& body, int64_t remainingMs)
        { return OllamaHttp_PostBridge(link.endpoint, body, OllamaBridge::Headers(link), remainingMs); });
    result.ok = bridge.ok;
    result.text = std::move(bridge.text);
    result.error = std::move(bridge.error);
    result.silent = bridge.silent;
    return result;
}
