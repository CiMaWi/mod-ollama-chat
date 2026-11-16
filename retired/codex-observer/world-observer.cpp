// Parked code of the retired Codex bridge's world observer (CS.20, LC.35). Not compiled, not tested.
// Cut verbatim from src/mod-ollama-chat_bridge_world.cpp at b81bf8c by LCB0.

// WorldPolicy::Retire() called _observer->ForgetAll(); InvalidateScope and Expire called _observer->Forget(scope).

WorldObserver::WorldObserver(Transport transport, ReadClock clock)
    : _transport(std::move(transport)), _clock(std::move(clock))
{
}

WorldObserver::~WorldObserver() { Stop(); }

void WorldObserver::Start()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_running)
        return;
    _running = true;
    _worker = std::thread(&WorldObserver::Run, this);
}

void WorldObserver::Reset(Clock now)
{
    // Serial tags discard a previous settings/epoch's reply, even if its HTTP finishes after retirement.
    if (_serial == std::numeric_limits<uint64_t>::max())
        _enabled = false;
    else
        ++_serial;
    _challenge = {};
    _epoch.clear();
    _accepted.clear();
    _observed.clear();
    _sent.clear();
    _challengeUntil = _liveUntil = {};
    _job.reset();
    _reply.reset();
    _retryAt = now.steadyMs + 1000;
    _refreshAt = 0;
}

void WorldObserver::RetirePacket(Clock now)
{
    // Best effort only. An occupied/ambiguous lane cannot invent the next challenge; native expiry is the bound.
    if (!_running || _busy || _job || _reply || _epoch.empty() || Expired(_challengeUntil, now))
        return;
    std::string body;
    if (BuildObserverSnapshot(_settings, _challenge, _epoch, false, false, {}, body))
        _job = Job{0, _settings, OllamaObserverOperation::Snapshot, std::move(body), _challengeUntil, _challenge, {}};
}

void WorldObserver::Configured(bool enabled, Settings settings, Clock now)
{
    std::lock_guard<std::mutex> lock(_mutex);
    RetirePacket(now);
    auto retirement = _job && _job->serial == 0 ? std::move(_job) : std::optional<Job>{};
    Reset(now);
    _enabled = enabled && ValidObserverSettings(settings) && _serial != std::numeric_limits<uint64_t>::max();
    _settings = _enabled ? std::move(settings) : Settings{};
    _lastSteady = now.steadyMs;
    _retryAt = now.steadyMs;
    _job = std::move(retirement);
    _wake.notify_one();
}

void WorldObserver::Stop()
{
    Configured(false, {}, _clock());
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _running = false;
    }
    _wake.notify_one();
    if (_worker.joinable())
        _worker.join();
}

WorldObserver::View WorldObserver::State() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return {_enabled, _challenge.incarnation, _epoch, _busy || _job.has_value() || _reply.has_value()};
}

void WorldObserver::Poll(Clock now)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (now.steadyMs < _lastSteady || (_liveUntil.steadyMs && Expired(_liveUntil, now)))
        Reset(now);
    _lastSteady = now.steadyMs;
    if (_reply)
    {
        Reply reply = std::move(*_reply);
        _reply.reset();
        if (reply.job.serial == _serial)
        {
            if (!reply.valid || Expired(reply.job.deadline, now) || Expired(reply.nextDeadline, now))
                Reset(now);
            else
            {
                if (reply.job.operation == OllamaObserverOperation::Snapshot)
                {
                    _accepted = std::move(reply.job.scopes);
                    std::erase_if(_accepted, [&](WorldScope const& scope)
                                  { return std::find(_observed.begin(), _observed.end(), scope) == _observed.end(); });
                    _liveUntil = reply.job.deadline;
                }
                _challenge = std::move(reply.challenge);
                _challengeUntil = reply.nextDeadline;
            }
        }
    }
    if ((!_challenge.incarnation.empty() && Expired(_challengeUntil, now)) ||
        (_liveUntil.steadyMs && Expired(_liveUntil, now)))
        Reset(now);
}

void WorldObserver::Open(bool worldDrained, Clock now)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_running || !_enabled || !worldDrained || _busy || _job || _reply || !_challenge.incarnation.empty() ||
        now.steadyMs < _retryAt)
        return;
    std::string body;
    if (!BuildObserverOpen(_settings, body))
        return;
    _job =
        Job{_serial, _settings, OllamaObserverOperation::Open, std::move(body), CaptureDeadline(now, WorldLivenessMs),
            {},      {}};
    _wake.notify_one();
}

bool WorldObserver::BindEpoch(std::string epoch, bool worldDrained, Clock now)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::string body;
    if (!_enabled || !worldDrained || !_epoch.empty() || Expired(_challengeUntil, now) ||
        !BuildObserverSnapshot(_settings, _challenge, epoch, true, true, {}, body))
        return false;
    _epoch = std::move(epoch);
    return true;
}

void WorldObserver::Publish(std::vector<WorldScope> scopes, bool worldDrained, Clock now)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (scopes.size() > MaxWorldScopes)
    {
        Reset(now);
        return;
    }
    _observed = scopes;
    // Removal is immediate locally, including while a previous snapshot is in flight.
    std::erase_if(_accepted, [&](WorldScope const& scope)
                  { return std::find(scopes.begin(), scopes.end(), scope) == scopes.end(); });
    if (!_enabled || _epoch.empty() || _busy || _job || _reply || (now.steadyMs < _refreshAt && scopes == _sent))
        return;
    std::string body;
    if (Expired(_challengeUntil, now) ||
        !BuildObserverSnapshot(_settings, _challenge, _epoch, true, worldDrained, scopes, body))
    {
        Reset(now);
        return;
    }
    _sent = scopes;
    _job = Job{_serial,    _settings,        OllamaObserverOperation::Snapshot, std::move(body), _challengeUntil,
               _challenge, std::move(scopes)};
    _refreshAt = now.steadyMs + 1000;
    _wake.notify_one();
}

bool WorldObserver::Current(WorldScope const& scope, Clock now) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _enabled && now.steadyMs >= _lastSteady && !Expired(_liveUntil, now) && scope.epoch == _epoch &&
           std::find(_accepted.begin(), _accepted.end(), scope) != _accepted.end();
}

void WorldObserver::Forget(WorldScope const& scope)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::erase(_accepted, scope);
    std::erase(_observed, scope);
    // A delayed acknowledgement must not resurrect this invalidated exact scope.
    if (_reply)
        std::erase(_reply->job.scopes, scope);
    if (_job && std::find(_job->scopes.begin(), _job->scopes.end(), scope) != _job->scopes.end())
    {
        // Unsent bytes no longer describe the world. Keep the unused challenge for a new world capture.
        _job.reset();
        _sent.clear();
        _refreshAt = 0;
    }
}

void WorldObserver::ForgetAll()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _accepted.clear();
    _observed.clear();
    if (_reply)
        _reply->job.scopes.clear();
    if (_job && _job->operation == OllamaObserverOperation::Snapshot && _job->serial)
    {
        _job.reset();
        _sent.clear();
        _refreshAt = 0;
    }
}

void WorldObserver::Run()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _wake.wait(lock, [&] { return !_running || _job.has_value(); });
            if (!_job)
                return;
            job = std::move(*_job);
            _job.reset();
            _busy = true;
        }
        Reply reply;
        reply.job = std::move(job);
        try
        {
            Clock const started = _clock();
            int64_t const remaining = RemainingMs(reply.job.deadline, started);
            if (remaining > 0 && remaining <= WorldLivenessMs)
            {
                auto const result = _transport(reply.job.settings, reply.job.operation, reply.job.body, remaining);
                auto const* previous =
                    reply.job.operation == OllamaObserverOperation::Snapshot ? &reply.job.previous : nullptr;
                reply.valid = result.ok() && result.error.empty() &&
                              ParseObserverChallenge(reply.job.settings, result.body, previous, reply.challenge);
                // Request start precedes native challenge issuance, so this is a conservative local bound.
                reply.nextDeadline = CaptureDeadline(started, WorldLivenessMs);
            }
        }
        catch (...)
        {
            reply.valid = false;
        }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (reply.job.serial == _serial)
            {
                if (!reply.valid)
                    _accepted.clear();
                _reply = std::move(reply);
            }
            _busy = false;
        }
    }
}
