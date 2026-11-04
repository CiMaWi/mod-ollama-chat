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

bool FilteredQueryAdapter::Marks(WorldTicket const& ticket, std::vector<uint8_t> const& icons,
                                 std::vector<ChannelObservedMark>& result)
{
    result.clear();
    if (icons.empty() || icons.size() > ChannelMaxEvents)
        return false;
    bool seen[8] = {};
    for (auto const icon : icons)
    {
        if (icon > 7 || seen[icon])
            return false;
        seen[icon] = true;
        uint64_t target = 0;
        VisibleUnit unit;
        if (!_source.ReadMark(ticket, icon, target) || !target || !_source.ResolveVisible(ticket, target, unit) ||
            !ValidUnit(unit, target))
            return false;
        result.push_back({target, icon});
    }
    return true;
}

bool FilteredQueryAdapter::CaptureMarks(WorldTicket const& ticket, DispatchGuard const& guard,
                                        std::vector<uint8_t> const& icons, MarkObservation& result)
{
    result = {};
    MarkObservation candidate{ticket, guard, CaptureDeadline(_clock(), WorldLivenessMs), {}};
    if (!guard.nonRaid || !guard.groupGuid || !Fresh(ticket, guard) || !Marks(ticket, icons, candidate.marks) ||
        !Current(candidate))
        return false;
    result = std::move(candidate);
    return true;
}

bool FilteredQueryAdapter::Current(MarkObservation const& observation)
{
    if (Expired(observation.until, _clock()) || !observation.guard.nonRaid || !observation.guard.groupGuid ||
        !Fresh(observation.ticket, observation.guard))
        return false;
    std::vector<uint8_t> icons;
    for (auto const& mark : observation.marks)
        icons.push_back(mark.icon);
    std::vector<ChannelObservedMark> current;
    return Marks(observation.ticket, icons, current) && current.size() == observation.marks.size() &&
           std::equal(current.begin(), current.end(), observation.marks.begin(),
                      [](ChannelObservedMark const& a, ChannelObservedMark const& b)
                      { return a.targetGuid == b.targetGuid && a.icon == b.icon; }) &&
           Fresh(observation.ticket, observation.guard) && !Expired(observation.until, _clock());
}
}  // namespace OllamaBridge
