// Server-free API integration proof. Config excerpts are exact generated source;
// this is not a full config/worldserver compile or game authority/delivery proof.
#include <cctype>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "Log.h"
#include "mod-ollama-chat_api.h"
// Stand-in only for core config lookup; assignments/parser below are exact source excerpts.
struct FixtureConfig
{
    std::map<std::string, std::string> values;
    template <typename T>
    T GetOption(std::string const& key, T const& fallback)
    {
        auto const it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
} fixtureConfig;
FixtureConfig* sConfigMgr = &fixtureConfig;
#include "../src/mod-ollama-chat_api.cpp"
#include "config-excerpt.inc"

OllamaThinkRequest OllamaCapability_ResolveThink(OllamaRequestKind) { return {}; }
bool OllamaCapability_ReasonsUnconditionally() { return false; }
void OllamaCapability_NoteEffortLevelRejected() {}
bool OllamaCapability_IsThinkRejection(int, std::string const&) { return false; }
void OllamaCapability_NoteThinkRejected() {}
void OllamaCapability_NoteUnconditionalReasoning() {}
void OllamaCapability_NoteLatency(uint64_t, bool) {}

namespace
{
unsigned bridgeCalls = 0;
bool malformedReply = false;
OllamaBridge::Settings expectedLink;
}  // namespace

OllamaHttpClient::OllamaHttpClient() : m_timeout(0), m_available(true) {}
OllamaHttpClient::~OllamaHttpClient() = default;
OllamaHttpResult OllamaHttpClient::PostEx(std::string const&, std::string const&, int, OllamaHttpHeaders const&, bool)
{
    throw std::runtime_error("unexpected legacy HTTP call");
}
OllamaHttpResult OllamaHttp_PostBridge(std::string const& endpoint, std::string const& body,
                                       OllamaHttpHeaders const& headers, int64_t remainingMs)
{
    ++bridgeCalls;
    if (endpoint != expectedLink.endpoint || headers != OllamaBridge::Headers(expectedLink) || remainingMs <= 0)
        throw std::runtime_error("incorrect adapter transport arguments");
    if (malformedReply)
        return {200, "malformed synthetic response", "", 0};
    auto response = nlohmann::json::parse(body);
    if (response["controllerGuid"] != "9007199254740993" || response["botGuid"] != "42")
        throw std::runtime_error("incorrect adapter GUID identity");
    response.erase("controllerGuid");
    response.erase("botGuid");
    response.erase("input");
    response["type"] = "generation.result";
    response["text"] = "fixture reply";
    return {200, response.dump(), "", 0};
}

namespace
{
unsigned checks = 0;
void Check(bool condition, char const* label)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(label);
}

void LegacyChecks()
{
    OllamaEndpointSettings cfg;
    cfg.model = "fixture-model";
    cfg.systemPrompt = "fixture-system";
    cfg.numPredict = 40;
    OllamaThinkRequest think;
    auto body = BuildRequest(cfg, "hello", think, 0);
    Check(body["model"] == cfg.model && body["prompt"] == "hello", "ollama builder");
    Check(body["system"] == cfg.systemPrompt && body["stream"] == false, "ollama system/stream");
    cfg.apiKey = "synthetic-hosted-key";
    Check(OllamaBuildRequestHeaders(cfg) == OllamaHttpHeaders{{"Authorization", "Bearer " + cfg.apiKey}},
          "ollama header");
    std::string text, thinking, error;
    ParseGenerateBody(R"({"response":"fixture ollama","done":true})", text, thinking, error);
    Check(text == "fixture ollama" && error.empty(), "ollama parser");
    cfg.provider = OllamaProvider::OpenAI;
    body = BuildRequest(cfg, "hello", think, 0);
    Check(body["messages"][0]["role"] == "system" && body["messages"][1]["content"] == "hello", "openai builder");
    Check(OllamaBuildRequestHeaders(cfg) == OllamaHttpHeaders{{"Authorization", "Bearer " + cfg.apiKey}},
          "openai header");
    text.clear();
    error.clear();
    ParseOpenAIBody(R"({"choices":[{"message":{"content":"fixture openai"}}]})", text, thinking, error);
    Check(text == "fixture openai" && error.empty(), "openai parser");
    cfg.provider = OllamaProvider::Anthropic;
    body = BuildRequest(cfg, "hello", think, 0);
    Check(body["system"] == cfg.systemPrompt && body["messages"][0]["content"] == "hello", "anthropic builder");
    Check(OllamaBuildRequestHeaders(cfg) ==
              OllamaHttpHeaders{{"anthropic-version", "2023-06-01"}, {"x-api-key", cfg.apiKey}},
          "anthropic header");
    text.clear();
    error.clear();
    ParseAnthropicBody(R"({"content":[{"type":"text","text":"fixture anthropic"}]})", text, thinking, error);
    Check(text == "fixture anthropic" && error.empty(), "anthropic parser");
    cfg.apiKeyHeader = "api-key";
    Check(OllamaBuildRequestHeaders(cfg).back() == std::make_pair(std::string("api-key"), cfg.apiKey),
          "legacy custom header");
    Check(QueryOllama("", OllamaRequestKind::ChatReply, cfg).error == "empty prompt", "legacy empty gate");
    Check(OllamaProvider_Parse("openrouter") == OllamaProvider::OpenAI &&
              OllamaProvider_Parse("claude") == OllamaProvider::Anthropic &&
              OllamaProvider_Parse("") == OllamaProvider::Ollama,
          "legacy provider aliases/default");
}

void BridgeChecks()
{
    Check(OllamaProvider_Parse(" BRIDGE ") == OllamaProvider::Bridge, "bridge parse");
    Check(std::string(OllamaProvider_Name(OllamaProvider::Bridge)) == "bridge", "bridge name");
    g_OllamaProvider = OllamaProvider::Bridge;
    g_OllamaApiKey = "synthetic-hosted-key-must-not-leak";
    g_OllamaApiKeyHeader = "synthetic-hosted-header";
    g_OllamaBridgeSettings = {"fixture-main", "codex", "http://127.0.0.1:47800", "synthetic-not-a-real-link-token"};
    g_OllamaSystemPrompt = "old-system";
    OllamaConfig_Publish();
    auto const queued = OllamaConfig_Snapshot();
    g_OllamaBridgeSettings = {"fixture-other", "claude", "http://127.0.0.1:47801", "another-synthetic-link-token"};
    g_OllamaSystemPrompt = "new-system";
    OllamaConfig_Publish();
    auto const latest = OllamaConfig_Snapshot();
    Check(queued.bridge.instance == "fixture-main" && queued.bridge.provider == "codex" &&
              queued.bridge.endpoint == "http://127.0.0.1:47800" && queued.systemPrompt == "old-system",
          "queued owned settings survive reload");
    Check(latest.bridge.provider == "claude" && latest.systemPrompt == "new-system", "reload publishes next snapshot");
    Check(OllamaBuildRequestHeaders(queued) == OllamaBridge::Headers(queued.bridge), "bridge uses link bearer only");
    Check(QueryOllama("hello", OllamaRequestKind::Sentiment, queued).error == "bridge_requires_bound_request",
          "background bridge prompt rejected");
    Check(QueryOllama("hello", OllamaRequestKind::ChatReply).error == "bridge_requires_bound_request",
          "legacy bridge wrapper rejected");
    OllamaBridge::Request request;
    request.epoch = "fixture-epoch";
    request.requestId = "fixture-request";
    request.deadline = OllamaBridge::CaptureDeadline(OllamaBridge::Now(), 10000);
    request.controllerGuid = 9007199254740993ULL;
    request.botGuid = 42;
    request.input.speakerGuid = request.controllerGuid;
    request.input.channel = "party";
    request.input.text = "hello";
    Check(QueryOllamaBridge(queued, request).error == "bridge_circuit_open_or_inactive",
          "config publish grants no activation");
    auto bad = queued;
    for (auto const* endpoint : {"http://localhost:47800", "https://127.0.0.1:47800", "http://127.0.0.1:47800/"})
    {
        bad.bridge.endpoint = endpoint;
        Check(QueryOllamaBridge(bad, request).error == "bridge_invalid_configuration", "invalid loopback fails closed");
    }
    bad = queued;
    bad.bridge.token.clear();
    Check(QueryOllamaBridge(bad, request).error == "bridge_invalid_configuration", "missing bearer fails closed");
    bad = queued;
    bad.provider = OllamaProvider::Ollama;
    Check(QueryOllamaBridge(bad, request).error == "bridge_invalid_configuration",
          "old provider cannot use bridge API");
    // Synthetic owner exercises only the circuit identity seam, not game authority.
    expectedLink = queued.bridge;
    OllamaBridge_SetActivation(OllamaBridge::MakeIdentity(queued.bridge, request));
    Check(QueryOllamaBridge(queued, request).ok, "queued codex settings retain original link after publish");
    Check(bridgeCalls == 1, "refusals made no transport call");
    OllamaBridge_SetActivation(OllamaBridge::MakeIdentity(latest.bridge, request));
    Check(QueryOllamaBridge(queued, request).error == "bridge_circuit_open_or_inactive",
          "new activation rejects queued prior identity");
    expectedLink = latest.bridge;
    Check(QueryOllamaBridge(latest, request).ok, "actual API forwards validated values to transport");
    Check(bridgeCalls == 2, "claude activation forwards once");
    malformedReply = true;
    Check(QueryOllamaBridge(latest, request).error == "bridge_invalid_response", "fixed parser failure propagated");
    Check(QueryOllamaBridge(latest, request).error == "bridge_circuit_open_or_inactive",
          "failure opens shared breaker");
    Check(bridgeCalls == 3, "open breaker performs no transport or retry");
    malformedReply = false;
    request.deadline = {};
    Check(QueryOllamaBridge(latest, request).error == "bridge_expired_or_invalid_request", "expired request rejected");
    OllamaBridge_SetActivation({});
    fixtureConfig.values = {{"OllamaChat.Bridge.Instance", "fixture-main"},
                            {"OllamaChat.Bridge.Provider", "codex"},
                            {"OllamaChat.Bridge.Endpoint", "http://127.0.0.1:47800"},
                            {"OllamaChat.Bridge.Token", "synthetic-not-a-real-link-token"}};
    FixtureLoadBridge();
    Check(OllamaBridge::ValidSettings(OllamaConfig_Snapshot().bridge), "exact config lookup assignments");
    fixtureConfig.values.erase("OllamaChat.Bridge.Token");
    FixtureLoadBridge();
    Check(!OllamaBridge::ValidSettings(OllamaConfig_Snapshot().bridge), "config missing token remains invalid");
}
}  // namespace

int main()
{
    try
    {
        LegacyChecks();
        BridgeChecks();
        std::cout << "PASS " << checks << " API integration checks\n";
        return 0;
    }
    catch (std::exception const& e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
