// Parked code of the retired Codex bridge's world observer (CS.20, LC.35). Not compiled, not tested.
// Cut verbatim from src/mod-ollama-chat_bridge.h at b81bf8c by LCB0.

// Settings members (struct OllamaBridge::Settings):
    bool observerEnabled = false;
    std::string observerTokenRef;  // Absolute protected observer.json path; resolved only on config load.
    std::string observerToken;     // Owned local observer bearer, distinct from the game-link bearer.

// Windows-only, matching the native service. Refusal always clears the cached secret.
bool LoadObserverToken(Settings& settings);
bool ValidObserverSettings(Settings const& settings);

struct ObserverChallenge
{
    std::string serviceId;
    std::string incarnation;
    std::string challenge;
    uint32_t sequence = 0;
};

bool BuildObserverOpen(Settings const& settings, std::string& body);
bool BuildObserverSnapshot(Settings const& settings, ObserverChallenge const& challenge, std::string const& epoch,
                           bool active, bool worldDrained, std::vector<WorldScope> const& scopes, std::string& body);
bool ParseObserverChallenge(Settings const& settings, std::string const& body, ObserverChallenge const* previous,
                            ObserverChallenge& challenge);

