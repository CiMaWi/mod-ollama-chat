#include "mod-ollama-chat_bridge_query.h"

#include <algorithm>
#include <utility>

namespace OllamaBridge
{
namespace
{
bool ValidUnit(VisibleUnit const& unit, uint64_t guid)
{
    return guid && unit.guid == guid && !unit.name.empty() && unit.name.size() <= 128 &&
           unit.name.find('\0') == std::string::npos && unit.healthPercent >= -1 && unit.healthPercent <= 100;
}

bool SameUnits(std::vector<Participant> const& left, std::vector<Participant> const& right)
{
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](Participant const& a, Participant const& b)
                      {
                          return a.guid == b.guid && a.name == b.name && a.healthPercent == b.healthPercent &&
                                 a.role == b.role && a.visibleTargetGuid == b.visibleTargetGuid;
                      });
}
}  // namespace

FilteredQueryAdapter::FilteredQueryAdapter(WorldPolicy& world, QueryWorldSource& source, ReadClock clock)
    : _world(world), _source(source), _clock(std::move(clock))
{
}

bool FilteredQueryAdapter::Fresh(WorldTicket const& ticket, DispatchGuard const& guard)
{
    DispatchGuard current;
    return _world.Current(ticket, _clock()) && _source.Observe(ticket, current) && _world.Current(ticket, _clock()) &&
           DispatchGuardCurrent(ticket, guard, current, _clock());
}

bool FilteredQueryAdapter::Units(WorldTicket const& ticket, DispatchGuard const& guard,
                                 std::vector<Participant>& result)
{
    result.clear();
    std::vector<uint64_t> members;
    if (!guard.nonRaid || !guard.groupGuid || !_source.GroupMembers(ticket, guard.groupGuid, members) ||
        members.empty() || members.size() > ChannelMaxUnits)
        return false;
    std::sort(members.begin(), members.end());
    if (!members.front() || std::adjacent_find(members.begin(), members.end()) != members.end() ||
        !std::binary_search(members.begin(), members.end(), ticket.scope.botGuid))
        return false;
    for (auto const guid : members)
    {
        VisibleUnit unit;
        if (!_source.ResolveVisible(ticket, guid, unit))
            continue;
        if (!ValidUnit(unit, guid))
            return false;
        if (unit.groupGuid != guard.groupGuid)
            continue;
        // Deliberately rebuild the allowlist: no AI role, selected target or private state enters the result.
        result.push_back({unit.guid, std::move(unit.name), "", unit.healthPercent, 0});
    }
    return true;
}

bool FilteredQueryAdapter::CaptureQuery(WorldTicket const& ticket, DispatchGuard const& guard, QueryObservation& result)
{
    result = {};
    QueryObservation candidate{ticket, guard, CaptureDeadline(_clock(), WorldLivenessMs), {}};
    if (!Fresh(ticket, guard) || !Units(ticket, guard, candidate.units) || !Current(candidate))
        return false;
    result = std::move(candidate);
    return true;
}

bool FilteredQueryAdapter::Current(QueryObservation const& observation)
{
    std::vector<Participant> current;
    return !Expired(observation.until, _clock()) && Fresh(observation.ticket, observation.guard) &&
           Units(observation.ticket, observation.guard, current) && SameUnits(observation.units, current) &&
           Fresh(observation.ticket, observation.guard) && !Expired(observation.until, _clock());
}

}  // namespace OllamaBridge
