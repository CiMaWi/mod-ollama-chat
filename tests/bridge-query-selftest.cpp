// Exact production filtered adapter with controlled current world objects, policy and physical mailboxes.
// This proves the helper contract, not the future Player/Group glue or live/native integration.
#include <array>
#include <functional>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "mod-ollama-chat_bridge_query.h"

namespace
{
using namespace OllamaBridge;
using Json = nlohmann::json;
unsigned checks = 0;
void Check(bool condition, char const* label)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(label);
}

struct Unit
{
    uint64_t guid = 0;
    uint64_t group = 33;
    std::string name;
    int32_t health = 70;
    bool online = true;
    bool visible = true;
    uint32_t map = 1;
    uint32_t instance = 0;
    uint32_t phase = 1;
    float distance = 10;
    // These deliberately have no corresponding observation adapter fields.
    std::string privateAiRole = "healer";
    uint64_t privateSelectedTarget = 999;
};

struct Source : QueryWorldSource
{
    DispatchGuard guard;
    std::map<uint64_t, Unit> objects;
    std::vector<uint64_t> members{11, 22, 44, 55};
    std::array<uint64_t, 8> marks{};
    bool authority = true;
    std::function<void()> afterResolve;
    unsigned resolves = 0;
    bool Observe(WorldTicket const&, DispatchGuard& current) override
    {
        current = guard;
        return authority;
    }
    bool GroupMembers(WorldTicket const&, uint64_t group, std::vector<uint64_t>& current) override
    {
        if (group != guard.groupGuid)
            return false;
        current = members;
        return true;
    }
    bool ResolveVisible(WorldTicket const&, uint64_t guid, VisibleUnit& result) override
    {
        ++resolves;
        auto const it = objects.find(guid);
        if (it == objects.end())
            return false;
        auto const& unit = it->second;
        if (!unit.online || !unit.visible || unit.map != guard.mapId || unit.instance != guard.instanceId ||
            !(unit.phase & guard.phaseMask) || unit.distance > 30)
            return false;
        result = {unit.guid, unit.group, unit.name, unit.health};
        if (afterResolve)
            afterResolve();
        return true;
    }
    bool ReadMark(WorldTicket const&, uint8_t icon, uint64_t& target) override
    {
        if (icon > 7)
            return false;
        target = marks[icon];
        return target != 0;
    }
};

struct Fixture
{
    Clock now{100000, 200000};
    Settings settings{"fixture", "codex", "http://127.0.0.1:47800", "synthetic-local-channel-token", false, "", ""};
    WorldPolicy world;
    Source source;
    FilteredQueryAdapter adapter{world, source, [this] { return now; }};
    WorldChannel channel{world, [this](WorldTicket const& ticket, DispatchGuard& guard)
                         { return source.Observe(ticket, guard); }, [this] { return now; }};
    WorldTicket ticket;
    Fixture()
    {
        world.Configured(true, settings, "epoch-1");
        Check(world.ObserveDrain(true, RetirementProof::GenerationOnlyNoCaptures), "select world epoch");
        channel.Configure(true);
        Check(channel.Activate(), "activate real channel");
        auto& guard = source.guard;
        guard.scope = {1, "fixture", "codex", "epoch-1", 11, 22, "ctx", "direct", "p1", "c1", "m1", "v1"};
        guard.source = DispatchSource::Whisper;
        guard.speakerGuid = 22;
        guard.groupGuid = 33;
        guard.mapId = 1;
        guard.phaseMask = 1;
        guard.audience = {11, 22};
        guard.humanSpeaker = guard.authority = guard.perceptible = guard.audienceOnline = guard.nonRaid = true;
        Check(world.ObserveScope(guard.scope, now), "capture actual world scope");
        Context input;
        input.speakerGuid = 22;
        input.channel = "direct";
        input.text = "What can we see?";
        ticket = world.Reserve(guard.scope, input, now, 30000);
        Check(ticket.generation != 0 && channel.Register(ticket, guard), "register game generation route");
        for (auto const guid : source.members)
        {
            Unit unit;
            unit.guid = guid;
            unit.name = "unit-" + std::to_string(guid);
            source.objects.emplace(guid, std::move(unit));
        }
        Unit enemy;
        enemy.guid = 77;
        enemy.group = 0;
        enemy.name = "visible-enemy";
        source.objects.emplace(77, enemy);
        source.objects[44].visible = false;
        source.objects.erase(55);  // Offline/missing group member.
        source.marks[0] = 77;
    }
    QueryObservation Capture()
    {
        QueryObservation out;
        Check(adapter.CaptureQuery(ticket, source.guard, out), "capture filtered production query");
        return out;
    }
    MarkObservation Marks(std::vector<uint8_t> icons = {0})
    {
        MarkObservation out;
        Check(adapter.CaptureMarks(ticket, source.guard, icons, out), "capture filtered production marks");
        return out;
    }
};

void QueryEncoding()
{
    Fixture f;
    auto observed = f.Capture();
    Check(observed.units.size() == 2 && observed.units[0].guid == 11 && observed.units[1].guid == 22,
          "only current visible group members survive");
    f.source.objects[22].health = -1;
    observed = f.Capture();
    Check(observed.units[0].role.empty() && !observed.units[0].visibleTargetGuid,
          "no AI role or selected-target provenance leak");
    ChannelPolicy policy;
    Check(policy.Bind(f.world.Selected(), f.settings.token) && policy.ObserveScope(f.ticket.scope, f.now),
          "real query serializer policy binding");
    Json wire = {
        {"version", 1},           {"instance", "fixture"},      {"provider", "codex"}, {"epoch", "epoch-1"},
        {"requestId", "query-1"}, {"deadlineUnixMs", 104000},   {"type", "query"},     {"controllerGuid", "22"},
        {"botGuid", "11"},        {"query", "observable_group"}};
    ChannelMessage query;
    Check(policy.Receive(f.settings.token, wire.dump(), f.ticket.scope, f.now, query, f.ticket.request.requestId) ==
              ChannelError::None,
          "independent query id against registered scope");
    std::string body;
    Check(f.adapter.Current(observed) &&
              policy.FinishQuery(query.envelope, f.ticket.scope, observed.units, f.now, body) == ChannelError::None,
          "filtered production values use actual query serializer");
    auto result = Json::parse(body);
    Check(result["type"] == "query.result" && result["units"].size() == 2, "typed query result");
    for (auto const& unit : result["units"])
        Check(!unit.contains("role") && !unit.contains("visibleTargetGuid"), "unknown provenance fields omitted");
    Check(result["units"][0]["healthPercent"] == 70 && !result["units"][1].contains("healthPercent"),
          "observed health encoded; unknown health omitted");
    Check(f.world.Pending() == 1, "query does not create game authority");
}

void Currentness()
{
    for (unsigned change = 0; change < 12; ++change)
    {
        Fixture f;
        auto observed = f.Capture();
        auto& unit = f.source.objects[22];
        switch (change)
        {
            case 0:
                unit.visible = false;
                break;
            case 1:
                unit.online = false;
                break;
            case 2:
                unit.map = 2;
                break;
            case 3:
                unit.phase = 2;
                break;
            case 4:
                unit.distance = 31;
                break;
            case 5:
                unit.instance = 2;
                break;
            case 6:
                unit.group = 34;
                break;
            case 7:
                unit.health = 10;
                break;
            case 8:
                unit.name = "renamed";
                break;
            case 9:
                f.source.authority = false;
                break;
            case 10:
                f.source.guard.audience = {11};
                break;
            case 11:
                f.source.guard.scope.membershipRevision = "m2";
                break;
        }
        Check(!f.adapter.Current(observed), "publication re-resolves changed world/provenance");
    }
    Fixture missing;
    auto out = missing.Capture();
    missing.source.objects.erase(22);
    Check(!missing.adapter.Current(out), "publication rejects removed units");
    Fixture expiry;
    out = expiry.Capture();
    expiry.now.steadyMs += 5000;
    expiry.now.unixMs -= 1;
    Check(!expiry.adapter.Current(out), "snapshot exact monotonic expiry survives wall rollback");
    Fixture reload;
    out = reload.Capture();
    reload.world.Configured(true, reload.settings, "epoch-2");
    Check(!reload.adapter.Current(out), "retirement/reconnect invalidates old snapshot");
    Fixture cancelled;
    out = cancelled.Capture();
    cancelled.world.Cancel(cancelled.ticket);
    Check(!cancelled.adapter.Current(out), "cancelled game ticket cannot publish");
    Fixture during;
    during.source.afterResolve = [&] { during.source.authority = false; };
    out = missing.Capture();
    Check(!during.adapter.CaptureQuery(during.ticket, during.source.guard, out) && out.units.empty() &&
              !out.ticket.generation,
          "mid-capture authority loss clears prior output");
    Fixture empty;
    for (auto& [guid, unit] : empty.source.objects)
    {
        (void)guid;
        unit.visible = false;
    }
    Check(empty.Capture().units.empty(), "no visible units yields empty filtered result");
}

void Bounds()
{
    Fixture f;
    QueryObservation query;
    f.source.members.push_back(11);
    Check(!f.adapter.CaptureQuery(f.ticket, f.source.guard, query), "duplicate group roster rejected");
    f.source.members = {0, 11};
    Check(!f.adapter.CaptureQuery(f.ticket, f.source.guard, query), "zero roster identity rejected");
    f.source.members = {22};
    Check(!f.adapter.CaptureQuery(f.ticket, f.source.guard, query), "bot must actually belong to group");
    f.source.members.assign(41, 11);
    auto const before = f.source.resolves;
    Check(!f.adapter.CaptureQuery(f.ticket, f.source.guard, query) && f.source.resolves == before,
          "oversize roster refused before object resolution");
    f.source.members = {11, 22};
    f.source.objects[22].name.assign(129, 'x');
    Check(!f.adapter.CaptureQuery(f.ticket, f.source.guard, query), "oversize observable name rejected");
    f.source.objects[22].name = std::string("x\0y", 3);
    Check(!f.adapter.CaptureQuery(f.ticket, f.source.guard, query), "NUL observable name rejected");
    f.source.objects[22].name = "name";
    f.source.objects[22].health = 101;
    Check(!f.adapter.CaptureQuery(f.ticket, f.source.guard, query), "invalid health rejected");
    f.source.guard.groupGuid = 0;
    Check(!f.adapter.CaptureQuery(f.ticket, f.source.guard, query), "no group cannot invent group observations");
}

void MarkPublication()
{
    Fixture f;
    auto observed = f.Marks();
    Check(observed.marks.size() == 1 && observed.marks[0].targetGuid == 77 && observed.marks[0].icon == 0,
          "visible non-group enemy mark is observable");
    auto events = f.world.Reserve(f.ticket.scope, f.ticket.request.input, f.now, WorldLivenessMs);
    Check(events.generation && events.request.requestId != f.ticket.request.requestId,
          "events require distinct game-owned reservation");
    uint64_t receipt = 0;
    Check(f.adapter.Current(observed) && f.channel.RegisterEvents(events, f.source.guard, observed.marks, receipt) &&
              receipt,
          "production filtered marks enter actual bounded channel");
    std::unique_ptr<ChannelPacket> packet;
    Check(f.channel.Take("wrong", receipt, packet) == ChannelError::Unauthorized && !packet,
          "receipt does not waive authentication");
    Check(f.channel.Take(f.settings.token, receipt, packet) == ChannelError::None && packet,
          "authenticated receipt handoff");
    auto body = Json::parse(packet->body);
    Check(body["requestId"] == events.request.requestId && body["type"] == "events" &&
              body["events"][0] == Json({{"kind", "observed_mark"}, {"targetGuid", "77"}, {"icon", 0}}),
          "actual event serializer exact typed payload and separate route");
    Check(f.channel.Used() == 1, "async packet retains shared physical slot");
    f.channel.Close(events);
    f.world.Cancel(events);
    f.channel.Retire();
    Check(f.channel.Used() == 1 && !f.channel.Drained(), "retirement does not release returned payload");
    packet.reset();
    Check(f.channel.Drained(), "write disposal releases physical slot");

    Fixture stale;
    observed = stale.Marks();
    stale.source.marks[0] = 22;
    Check(!stale.adapter.Current(observed), "changed icon target fails last-moment provenance");
    stale.source.marks[0] = 77;
    stale.source.objects[77].visible = false;
    Check(!stale.adapter.Current(observed), "hidden marked target fails last-moment provenance");
    MarkObservation invalid;
    Check(!stale.adapter.CaptureMarks(stale.ticket, stale.source.guard, {0}, invalid) && invalid.marks.empty(),
          "hidden marks never enter batch");
    Check(!stale.adapter.CaptureMarks(stale.ticket, stale.source.guard, {}, invalid), "empty marks rejected");
    Check(!stale.adapter.CaptureMarks(stale.ticket, stale.source.guard, {8}, invalid), "invalid icon rejected");
    Check(!stale.adapter.CaptureMarks(stale.ticket, stale.source.guard, {0, 0}, invalid), "duplicate icon rejected");
    Check(!stale.adapter.CaptureMarks(stale.ticket, stale.source.guard, std::vector<uint8_t>(33, 0), invalid),
          "more than32 marks rejected");
    Fixture all;
    for (uint8_t i = 0; i < 8; ++i)
        all.source.marks[i] = 77;
    Check(all.Marks({0, 1, 2, 3, 4, 5, 6, 7}).marks.size() == 8, "all supported icon values captured");
    auto mark = all.Marks();
    all.now.steadyMs += 5000;
    Check(!all.adapter.Current(mark), "mark snapshot expires at5 seconds");
}
}  // namespace

int main()
{
    try
    {
        QueryEncoding();
        Currentness();
        Bounds();
        MarkPublication();
        std::cout << "PASS " << checks << " production filtered query/event helper checks\n";
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
