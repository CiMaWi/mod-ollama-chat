// Parked code of the retired Codex bridge's world observer (CS.20, LC.35). Not compiled, not tested.
// Cut verbatim from src/mod-ollama-chat_bridge_world.h at b81bf8c by LCB0.

// One independent owned HTTP lane. Only Poll/BindEpoch/Publish on the world thread can accept authority.
// Each challenge consumes a new complete capture; no sender retry or cached heartbeat exists.
class WorldObserver
{
public:
    using Transport =
        std::function<OllamaHttpResult(Settings const&, OllamaObserverOperation, std::string const&, int64_t)>;
    struct View
    {
        bool enabled = false;
        std::string incarnation;
        std::string epoch;
        bool pending = false;
    };

    explicit WorldObserver(Transport transport, ReadClock clock = Now);
    ~WorldObserver();
    void Start();
    void Stop();  // Terminal shutdown only; never called on tick/reload.
    void Configured(bool enabled, Settings settings, Clock now);
    void Poll(Clock now);
    View State() const;
    void Open(bool worldDrained, Clock now);
    bool BindEpoch(std::string epoch, bool worldDrained, Clock now);
    void Publish(std::vector<WorldScope> scopes, bool worldDrained, Clock now);
    bool Current(WorldScope const& scope, Clock now) const;
    void Forget(WorldScope const& scope);
    void ForgetAll();

private:
    struct Job
    {
        uint64_t serial = 0;
        Settings settings;
        OllamaObserverOperation operation = OllamaObserverOperation::Open;
        std::string body;
        Deadline deadline;
        ObserverChallenge previous;
        std::vector<WorldScope> scopes;
    };
    struct Reply
    {
        Job job;
        bool valid = false;
        ObserverChallenge challenge;
        Deadline nextDeadline;
    };
    void Run();
    void Reset(Clock now);
    void RetirePacket(Clock now);

    Transport const _transport;
    ReadClock const _clock;
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    std::thread _worker;
    bool _running = false;
    bool _busy = false;
    bool _enabled = false;
    uint64_t _serial = 1;
    Settings _settings;
    ObserverChallenge _challenge;
    std::string _epoch;
    Deadline _challengeUntil;
    Deadline _liveUntil;
    int64_t _lastSteady = 0;
    int64_t _retryAt = 0;
    int64_t _refreshAt = 0;
    std::vector<WorldScope> _accepted;
    std::vector<WorldScope> _observed;
    std::vector<WorldScope> _sent;
    std::optional<Job> _job;
    std::optional<Reply> _reply;
};

// WorldPolicy held it as: WorldObserver* const _observer;  (ctor WorldPolicy(WorldObserver* observer = nullptr))
