#include "mod-ollama-chat_bridge_world.h"

#include <algorithm>
#include <limits>

namespace OllamaBridge
{
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
    std::erase_if(_scopes, [&](ScopeEntry const& entry)
                  { return entry.scope.botGuid == botGuid && entry.scope.contextId == contextId; });
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
    std::erase_if(_scopes, [&](ScopeEntry const& entry) { return Expired(entry.liveUntil, now); });
    std::erase_if(_pending,
                  [&](auto const& entry)
                  {
                      return Expired(entry.second.ticket.request.deadline, now) ||
                             !ScopeCurrent(entry.second.ticket.scope, now);
                  });
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
    ticket.request = {_selected.epoch, _selected.epoch + "-" + std::to_string(++_sequence),
                      deadline,        scope.controllerGuid,
                      scope.botGuid,   std::move(input)};
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
           !Expired(ticket.request.deadline, now) && ScopeCurrent(ticket.scope, now) && it != _pending.end() &&
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
    if (entry.stage == Stage::Completed || (entry.stage == Stage::Reserved && outcome != WorldOutcome::QueueFull))
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
    _pending.erase(ticket.request.requestId);
    return true;  // Caller still revalidates real game authority/perception immediately before effect.
}

void WorldPolicy::Cancel(WorldTicket const& ticket)
{
    if (ticket.generation == _generation)
        _pending.erase(ticket.request.requestId);
}
}  // namespace OllamaBridge
