// Parked code of the retired Codex bridge's world observer (CS.20, LC.35). Not compiled, not tested.
// Cut verbatim from src/mod-ollama-chat_httpclient.h at b81bf8c by LCB0.

enum class OllamaObserverOperation
{
    Open,
    Snapshot
};

// Worker-only owned snapshot; exactly one observer bearer, fixed routes and <=5000 ms.
// The world owner supplies fresh serialized protocol data and handles challenges/retirement.
OllamaHttpResult OllamaHttp_PostObserver(OllamaBridge::Settings const& settings, OllamaObserverOperation operation,
                                         std::string const& jsonData, int64_t remainingMs);

