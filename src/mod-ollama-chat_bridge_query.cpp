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
}  // namespace

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

}  // namespace OllamaBridge
