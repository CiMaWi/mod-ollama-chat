#include "mod-ollama-chat_bridge_world.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace OllamaBridge
{
std::atomic<std::size_t> DispatchSlot::_used{0};

DispatchSlot::~DispatchSlot() { Release(); }
DispatchSlot::DispatchSlot(DispatchSlot&& other) noexcept : _held(std::exchange(other._held, false)) {}

DispatchSlot& DispatchSlot::operator=(DispatchSlot&& other) noexcept
{
    if (this != &other)
    {
        Release();
        _held = std::exchange(other._held, false);
    }
    return *this;
}

void DispatchSlot::Release()
{
    if (std::exchange(_held, false))
        --_used;
}

DispatchSlot DispatchSlot::Acquire()
{
    DispatchSlot slot;
    auto used = _used.load();
    while (used < MaxWorldPending)
        if (_used.compare_exchange_weak(used, used + 1))
        {
            slot._held = true;
            break;
        }
    return slot;
}

std::size_t DispatchSlot::Used() { return _used.load(); }

bool DispatchGuardCurrent(WorldTicket const& ticket, DispatchGuard const& captured, DispatchGuard const& current,
                          Clock now)
{
    if (!ticket.generation || Expired(ticket.request.deadline, now) || !(ticket.scope == captured.scope) ||
        !(captured.scope == current.scope) || !current.humanSpeaker || !current.authority || !current.perceptible ||
        !current.audienceOnline || !captured.humanSpeaker || !captured.authority || !captured.perceptible ||
        !captured.audienceOnline || current.source == DispatchSource::Unsupported ||
        current.source != captured.source || !current.speakerGuid || current.speakerGuid != captured.speakerGuid ||
        current.speakerGuid != ticket.request.input.speakerGuid || current.groupGuid != captured.groupGuid ||
        current.mapId != captured.mapId || current.instanceId != captured.instanceId || !current.phaseMask ||
        current.phaseMask != captured.phaseMask || current.audience != captured.audience || current.audience.empty() ||
        current.audience.size() > 40 || !std::is_sorted(current.audience.begin(), current.audience.end()) ||
        current.audience.front() == 0 ||
        std::adjacent_find(current.audience.begin(), current.audience.end()) != current.audience.end() ||
        !std::binary_search(current.audience.begin(), current.audience.end(), current.speakerGuid) ||
        !std::binary_search(current.audience.begin(), current.audience.end(), ticket.request.botGuid))
        return false;
    if (current.source == DispatchSource::Party)
        return current.groupGuid && current.nonRaid && captured.nonRaid && ticket.scope.channel == "party";
    return ticket.scope.channel == "direct";
}

TerminalAction DispatchTerminal(WorldOutcome outcome, bool addressedHuman)
{
    switch (outcome)
    {
        case WorldOutcome::Success:
            return TerminalAction::Reply;
        case WorldOutcome::Unavailable:
        case WorldOutcome::Empty:
        case WorldOutcome::Filtered:
        case WorldOutcome::Malformed:
        case WorldOutcome::Timeout:
        case WorldOutcome::CircuitOpen:
        case WorldOutcome::QueueFull:
            return addressedHuman ? TerminalAction::Outage : TerminalAction::Silence;
        default:
            return TerminalAction::Silence;
    }
}

char const* DispatchOutageText() { return "Give me a moment to gather my thoughts."; }

bool WorldScope::operator==(WorldScope const& other) const
{
    return version == other.version && instance == other.instance && provider == other.provider &&
           epoch == other.epoch && botGuid == other.botGuid && controllerGuid == other.controllerGuid &&
           contextId == other.contextId && channel == other.channel && perceptionScope == other.perceptionScope &&
           contextRevision == other.contextRevision && membershipRevision == other.membershipRevision &&
           personaRevision == other.personaRevision;
}

void WorldPolicy::Retire()
{
    if (_observer)
        _observer->ForgetAll();
    _selected = {};
    _scopes.clear();
    _pending.clear();
    _retiring = true;
}

void WorldPolicy::Configured(bool enabled, Settings settings, std::string epoch)
{
    Retire();
    _enabled = false;
    _desired = {};
    _settings = {};
    if (_generation == std::numeric_limits<uint64_t>::max())
        return;
    ++_generation;
    _issued = 0;
    if (!enabled || !ValidSettings(settings) || !ValidIdentifier(epoch))
        return;
    _settings = std::move(settings);
    _desired = {_settings.instance, _settings.provider, _settings.endpoint, std::move(epoch)};
    _enabled = true;
}

void WorldPolicy::Shutdown() { Configured(false, {}, {}); }

bool WorldPolicy::ObserveDrain(bool drained, RetirementProof proof)
{
    if (!_retiring || !drained || proof != RetirementProof::GenerationOnlyNoCaptures)
        return false;
    _retiring = false;
    if (_enabled)
        _selected = _desired;
    return !_selected.epoch.empty();
}

bool WorldPolicy::ObserveScope(WorldScope scope, Clock now)
{
    // A heartbeat arriving after a gap must not resurrect requests bound to the expired observation.
    Expire(now);
    if (_selected.epoch.empty() || scope.version != 1 || scope.instance != _selected.instance ||
        scope.provider != _selected.provider || scope.epoch != _selected.epoch || !scope.botGuid ||
        !scope.controllerGuid || (scope.channel != "direct" && scope.channel != "party") ||
        !ValidIdentifier(scope.contextId) || !ValidIdentifier(scope.perceptionScope) ||
        !ValidIdentifier(scope.contextRevision) || !ValidIdentifier(scope.membershipRevision) ||
        !ValidIdentifier(scope.personaRevision))
        return false;
    Deadline const liveUntil = CaptureDeadline(now, WorldLivenessMs);
    if (Expired(liveUntil, now))
        return false;
    auto it =
        std::find_if(_scopes.begin(), _scopes.end(), [&](ScopeEntry const& entry)
                     { return entry.scope.botGuid == scope.botGuid && entry.scope.contextId == scope.contextId; });
    if (it != _scopes.end())
    {
        if (!(it->scope == scope))
        {
            InvalidateScope(scope.botGuid, scope.contextId);
            _scopes.push_back({std::move(scope), liveUntil});
        }
        else
            it->liveUntil = liveUntil;
        return true;
    }
    if (_scopes.size() >= MaxWorldScopes)
        return false;
    _scopes.push_back({std::move(scope), liveUntil});
    return true;
}

void WorldPolicy::InvalidateScope(uint64_t botGuid, std::string const& contextId)
{
    std::erase_if(_scopes,
                  [&](ScopeEntry const& entry)
                  {
                      bool const remove = entry.scope.botGuid == botGuid && entry.scope.contextId == contextId;
                      if (remove && _observer)
                          _observer->Forget(entry.scope);
                      return remove;
                  });
    std::erase_if(
        _pending, [&](auto const& entry)
        { return entry.second.ticket.scope.botGuid == botGuid && entry.second.ticket.scope.contextId == contextId; });
}

bool WorldPolicy::ScopeCurrent(WorldScope const& scope, Clock now) const
{
    return !_selected.epoch.empty() && std::any_of(_scopes.begin(), _scopes.end(), [&](ScopeEntry const& entry)
                                                   { return entry.scope == scope && !Expired(entry.liveUntil, now); });
}

void WorldPolicy::Expire(Clock now)
{
    std::erase_if(_scopes,
                  [&](ScopeEntry const& entry)
                  {
                      bool const remove = Expired(entry.liveUntil, now);
                      if (remove && _observer)
                          _observer->Forget(entry.scope);
                      return remove;
                  });
    std::erase_if(_pending,
                  [&](auto const& entry)
                  {
                      return Expired(entry.second.ticket.request.deadline, now) ||
                             !ScopeCurrent(entry.second.ticket.scope, now);
                  });
}

std::vector<WorldScope> WorldPolicy::Scopes(Clock now) const
{
    std::vector<WorldScope> scopes;
    for (auto const& entry : _scopes)
        if (!Expired(entry.liveUntil, now))
            scopes.push_back(entry.scope);
    return scopes;
}

WorldTicket WorldPolicy::Reserve(WorldScope const& scope, Context input, Clock now, int64_t lifetimeMs)
{
    Expire(now);
    if (!ScopeCurrent(scope, now) || _pending.size() >= MaxWorldPending || _issued >= MaxEpochRequests ||
        _sequence == std::numeric_limits<uint64_t>::max())
        return {};
    Deadline const deadline = CaptureDeadline(now, lifetimeMs);
    if (Expired(deadline, now) || input.channel != scope.channel)
        return {};
    WorldTicket ticket;
    ticket.generation = _generation;
    ticket.scope = scope;
    ticket.request = {_selected.epoch,
                      _selected.epoch + "-" + std::to_string(++_sequence),
                      deadline,
                      scope.controllerGuid,
                      scope.botGuid,
                      std::move(input),
                      scope};
    // Reuse the actual builder's bounds/encoding validation before allocating a reservation.
    std::string body;
    if (!Build(_settings, ticket.request, now, body))
        return {};
    ++_issued;
    _pending.emplace(ticket.request.requestId, PendingEntry{ticket, Stage::Reserved});
    return ticket;
}

bool WorldPolicy::Current(WorldTicket const& ticket, Clock now) const
{
    auto const it = _pending.find(ticket.request.requestId);
    return ticket.generation == _generation && ticket.request.epoch == _selected.epoch &&
           ticket.request.scope == ticket.scope && !Expired(ticket.request.deadline, now) &&
           ScopeCurrent(ticket.scope, now) && it != _pending.end() &&
           it->second.ticket.generation == ticket.generation && it->second.ticket.scope == ticket.scope &&
           it->second.ticket.request.deadline.unixMs == ticket.request.deadline.unixMs &&
           it->second.ticket.request.deadline.steadyMs == ticket.request.deadline.steadyMs &&
           it->second.ticket.request.botGuid == ticket.request.botGuid &&
           it->second.ticket.request.controllerGuid == ticket.request.controllerGuid;
}

bool WorldPolicy::MarkQueued(WorldTicket const& ticket, Clock now)
{
    if (!Current(ticket, now))
        return false;
    auto& entry = _pending.at(ticket.request.requestId);
    if (entry.stage != Stage::Reserved)
        return false;
    entry.stage = Stage::Queued;
    return true;
}

bool WorldPolicy::Complete(WorldTicket const& ticket, WorldOutcome outcome, Clock now)
{
    if (!Current(ticket, now))
        return false;
    auto& entry = _pending.at(ticket.request.requestId);
    if (entry.stage != Stage::Queued && !(entry.stage == Stage::Reserved && outcome == WorldOutcome::QueueFull))
        return false;
    if (outcome == WorldOutcome::Cancelled)
    {
        Cancel(ticket);
        return false;
    }
    entry.stage = Stage::Completed;
    return true;  // Caller may stage one reply/fallback; this grants no game effect.
}

bool WorldPolicy::Consume(WorldTicket const& ticket, Clock now)
{
    if (!Current(ticket, now) || _pending.at(ticket.request.requestId).stage != Stage::Completed)
        return false;
    _pending.at(ticket.request.requestId).stage = Stage::Consumed;
    return true;  // Retain scope freshness until caller finishes all effects and cancels this claim.
}

void WorldPolicy::Cancel(WorldTicket const& ticket)
{
    if (ticket.generation == _generation)
        _pending.erase(ticket.request.requestId);
}
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

}  // namespace OllamaBridge
