#ifndef MOD_OLLAMA_CHAT_HTTPCLIENT_H
#define MOD_OLLAMA_CHAT_HTTPCLIENT_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Extra request headers, e.g. the Authorization / x-api-key a hosted provider
// needs. Carried per call so the value comes from the caller's config
// snapshot rather than from a string global a worker must not read.
using OllamaHttpHeaders = std::vector<std::pair<std::string, std::string>>;

// Full result of an HTTP call, so callers can distinguish "model refused" from
// "server unreachable" -- the old client returned an empty string for both,
// which is why an unsupported think mode looked identical to a dead Ollama.
struct OllamaHttpResult
{
    int         status = 0;      // HTTP status, 0 when the request never completed
    std::string body;
    std::string error;           // transport-level error text, empty on success

    bool ok() const { return status == 200; }
};

// Dedicated silent local-link operation. A fresh nonredirecting socket, a 250 ms
// connection cap, a total monotonic deadline, and a streaming response byte cap.
// Does not consult config globals, use hosted keys, retry, or log a remote body.
OllamaHttpResult OllamaHttp_PostBridge(std::string const& baseUrl, std::string const& jsonData,
                                       OllamaHttpHeaders const& headers, int64_t remainingMs);

#endif // MOD_OLLAMA_CHAT_HTTPCLIENT_H
