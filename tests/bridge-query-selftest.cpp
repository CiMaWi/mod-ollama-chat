// Exact production filtered adapter with controlled current world objects, policy and physical mailboxes.
// This proves the helper contract, not the future Player/Group glue or live/native integration.
#include <array>
#include <functional>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "bridge-ability-double.h"
#include "mod-ollama-chat_bridge_query.h"

// The production phase holds the post-map-join token (game headers); the channel only asks whether one is given.
namespace OllamaBridge
{
struct EffectPhase
{
};
}  // namespace OllamaBridge

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
    std::vector<StateEntry> topicEntries;
    bool topicReadable = true;
    unsigned topicReads = 0;
    uint64_t topicBot = 0;
    StateTopic topicAsked = StateTopic::Inventory;
    std::string topicPlace = "unread";
    bool ReadTopic(WorldTicket const& ticket, StateTopic topic, std::string const& place,
                   std::vector<StateEntry>& entries) override
    {
        ++topicReads;
        topicBot = ticket.scope.botGuid;
        topicAsked = topic;
        topicPlace = place;
        entries = topicEntries;
        return topicReadable;
    }
    // J6e-c: the bot's watch facts (J5f reader), read only with the post-map-join phase.
    WatchFacts watchFacts{true, true, 2};
    bool watchReadable = true;
    unsigned watchReads = 0;
    uint64_t watchBot = 0;
    uint64_t watchPartner = 0;
    bool ReadWatch(uint64_t botGuid, uint64_t partnerGuid, WatchFacts& facts) override
    {
        ++watchReads;
        watchBot = botGuid;
        watchPartner = partnerGuid;
        facts = watchFacts;
        return watchReadable;
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
        Check(world.ObserveDrain(true, RetirementProof::NoLiveCaptures), "select world epoch");
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

// J5f-b: a bot_state query on the real channel reads the route's own bot through the source, on the world update.
void TopicPublication()
{
    Fixture f;
    WorldChannel channel{f.world, [&f](WorldTicket const& ticket, DispatchGuard& guard)
                         { return f.source.Observe(ticket, guard); }, [&f] { return f.now; }, &f.source};
    channel.Configure(true);
    Check(channel.Activate() && channel.Register(f.ticket, f.source.guard), "topic channel route");
    auto const ask = [&](std::string const& id, std::string const& topic, std::string const& place = {})
    {
        Json wire = {{"version", 1},    {"instance", "fixture"},    {"provider", "codex"}, {"epoch", "epoch-1"},
                     {"requestId", id}, {"deadlineUnixMs", 104000}, {"type", "query"},     {"controllerGuid", "22"},
                     {"botGuid", "11"}, {"query", "bot_state"},     {"topic", topic}};
        if (!place.empty())
            wire["place"] = place;  // WT18b: route names its place.
        uint64_t receipt = 0;
        Check(channel.Receive(f.settings.token, f.ticket.request.requestId, wire.dump(), receipt) == ChannelError::None,
              "topic query received");
        channel.Update();
        std::unique_ptr<ChannelPacket> packet;
        channel.Take(f.settings.token, receipt, packet);
        return packet;
    };
    f.source.topicEntries = {{"Hearthstone", "", 1800, 0}, {"Ready", "", 0, 0}};
    auto packet = ask("topic-1", "cooldowns");
    Check(packet && packet->error == ChannelError::None, "topic result published");
    auto json = Json::parse(packet->body);
    Check(json.at("type") == "query.result" && json.at("topic") == "cooldowns" && json.at("entries").size() == 1 &&
              json.at("entries").at(0).at("seconds") == 1800,
          "source entries filtered into the topic result");
    Check(f.source.topicReads == 1 && f.source.topicBot == 11 && f.source.topicAsked == StateTopic::Cooldowns &&
              f.source.topicPlace.empty(),
          "the route's own bot read once for the asked topic, with no place");
    Check(f.source.resolves == 0, "a topic reads no other unit");
    f.source.topicReadable = false;
    packet = ask("topic-2", "spells");
    Check(packet && Json::parse(packet->body).at("code") == "unavailable", "unreadable bot answers unavailable");
    f.source.topicReadable = true;
    packet = ask("topic-3", "bank");
    Check(packet && packet->error == ChannelError::Invalid && packet->body.empty() && f.source.topicReads == 2,
          "unknown topic invalid_input without a read");
    // WT18b: a route query hands its place to the source; the planner rows come back with their row words.
    f.source.topicEntries = {{"goldshire", "total", 95, 10}, {"walk", "leg", 95, 0}, {"walk", "", 1, 1}};
    packet = ask("topic-5", "route", "Goldshire");
    json = packet ? Json::parse(packet->body) : Json();
    Check(packet && packet->error == ChannelError::None && json.at("topic") == "route" &&
              json.at("entries").size() == 2 && json.at("entries").at(0).at("row") == "total" &&
              json.at("entries").at(1).at("row") == "leg" && json.at("entries").at(0).at("value") == 95,
          "a route result carries its rows");
    Check(f.source.topicReads == 3 && f.source.topicAsked == StateTopic::Route && f.source.topicPlace == "Goldshire",
          "the route place reaches the source read");
    packet = ask("topic-6", "route");
    Check(packet && packet->error == ChannelError::Invalid && f.source.topicReads == 3,
          "a route without its place is invalid_input without a read");
    f.source.authority = false;
    packet = ask("topic-4", "quests");
    Check(!packet && f.source.topicReads == 3, "a stale route reads nothing and publishes nothing");
}

// LC3: a catalogue query on the real channel answers the installed index on the world update and reads no unit;
// without an index it answers unavailable.
void CataloguePublication()
{
    Fixture f;
    WorldChannel channel{f.world, [&f](WorldTicket const& ticket, DispatchGuard& guard)
                         { return f.source.Observe(ticket, guard); }, [&f] { return f.now; }, &f.source};
    channel.Configure(true);
    Check(channel.Activate() && channel.Register(f.ticket, f.source.guard), "catalogue channel route");
    auto const ask = [&](std::string const& id)
    {
        Json wire = {{"version", 1},    {"instance", "fixture"},    {"provider", "codex"}, {"epoch", "epoch-1"},
                     {"requestId", id}, {"deadlineUnixMs", 104000}, {"type", "query"},     {"controllerGuid", "22"},
                     {"botGuid", "11"}, {"query", "catalogue"}};
        uint64_t receipt = 0;
        Check(channel.Receive(f.settings.token, f.ticket.request.requestId, wire.dump(), receipt) == ChannelError::None,
              "catalogue query received");
        channel.Update();
        std::unique_ptr<ChannelPacket> packet;
        channel.Take(f.settings.token, receipt, packet);
        return packet;
    };
    auto packet = ask("catalogue-1");
    auto json = packet ? Json::parse(packet->body) : Json();
    Check(packet && packet->error == ChannelError::None && json.at("type") == "query.result" &&
              json.at("catalogue").at("hash") == "0a1b2c3d4e5f6071" && json.at("catalogue").at("rows").size() == 17,
          "the installed catalogue published");
    Check(f.source.topicReads == 0 && f.source.resolves == 0, "a catalogue reads no bot and no unit");
    SetAbilityIndex({});
    packet = ask("catalogue-2");
    Check(packet && packet->error == ChannelError::None && Json::parse(packet->body).at("code") == "unavailable",
          "without an index the catalogue is unavailable");
    Check(InstallTestAbilities(), "the double installs again");
    f.source.authority = false;
    packet = ask("catalogue-3");
    Check(!packet && f.source.topicReads == 0, "a stale route publishes no catalogue");
}

// J6e-c (DS.14 (c)): a watch on a real generation route: admitted on the world thread with the phase, from the
// route's own binding; an armed watch counts as acting on the route (a silent answer may stand), a refused or cleared
// one does not; epoch retirement ends it.
void WatchArming()
{
    Fixture f;
    WorldChannel channel{f.world, [&f](WorldTicket const& ticket, DispatchGuard& guard)
                         { return f.source.Observe(ticket, guard); }, [&f] { return f.now; }, &f.source};
    channel.Configure(true);
    Check(channel.Activate(), "watch channel");
    f.world.Cancel(f.ticket);
    EffectPhase const phase{};
    auto const open = [&](uint8_t depth)
    {
        Context input;
        input.speakerGuid = 22;
        input.channel = "direct";
        input.text = "Come to me once this fight is over.";
        input.wakeDepth = depth;
        input.originUnixMs = f.now.unixMs - 1000;
        auto ticket = f.world.Reserve(f.source.guard.scope, input, f.now, 30000);
        Check(ticket.generation != 0 && channel.Register(ticket, f.source.guard), "watch route registered");
        return ticket;
    };
    auto const watch =
        [&](WorldTicket const& ticket, std::string const& id, std::string const& condition, EffectPhase const* with)
    {
        Json wire = {{"version", 1},
                     {"instance", "fixture"},
                     {"provider", "codex"},
                     {"epoch", "epoch-1"},
                     {"requestId", ticket.request.requestId},
                     {"deadlineUnixMs", ticket.request.deadline.unixMs},
                     {"type", "watch"},
                     {"commandId", id},
                     {"botGuid", "11"},
                     {"condition", condition},
                     {"untilUnixMs", f.now.unixMs + 60000}};
        uint64_t receipt = 0;
        if (channel.Receive(f.settings.token, ticket.request.requestId, wire.dump(), receipt) != ChannelError::None)
            return Json();
        channel.Update(with);
        std::unique_ptr<ChannelPacket> packet;
        channel.Take(f.settings.token, receipt, packet);
        return packet && packet->error == ChannelError::None ? Json::parse(packet->body) : Json();
    };
    auto ticket = open(0);
    auto result = watch(ticket, "watch-1", "combat_end", &phase);
    Check(result.at("type") == "watch.result" && result.at("commandId") == "watch-1" &&
              result.at("outcome") == "armed" && !result.contains("reason"),
          "watch: an armed result is published");
    auto const* armed = channel.Watches().Find(11);
    Check(armed && armed->id == "watch-1" && armed->partnerGuid == 22 && armed->source == DispatchSource::Whisper &&
              armed->channel == "direct" && armed->contextId == "ctx" && armed->depth == 1 &&
              armed->originUnixMs == f.now.unixMs - 1000 && armed->condition == WatchCondition::CombatEnd &&
              armed->activity == 2 && armed->untilUnixMs == f.now.unixMs + 60000,
          "watch: the route's bot, partner, conversation and chain stored");
    Check(f.source.watchReads == 1 && f.source.watchBot == 11 && f.source.watchPartner == 22,
          "watch: the bot's facts read once, with its partner");
    Check(channel.Close(ticket), "watch: an armed watch counts as acting on the route");
    f.world.Cancel(ticket);

    ticket = open(0);
    f.source.watchFacts = {true, false, 0};
    result = watch(ticket, "watch-2", "combat_end", &phase);
    Check(result.at("outcome") == "rejected" && result.at("reason") == "not_fighting" &&
              result.at("reasonText") == "I am not fighting anything right now.",
          "watch: combat_end outside a fight is not_fighting");
    result = watch(ticket, "watch-3", "activity_done", &phase);
    Check(result.at("outcome") == "rejected" && result.at("reason") == "not_busy" &&
              result.at("reasonText") == "I am not busy with anything.",
          "watch: activity_done while idle is not_busy");
    Check(channel.Watches().Find(11) && channel.Watches().Find(11)->id == "watch-1",
          "watch: a refused watch leaves the bot's watch alone");
    Check(!channel.Close(ticket), "watch: a refused watch is no act");
    f.world.Cancel(ticket);

    ticket = open(0);
    result = watch(ticket, "watch-4", "none", &phase);
    Check(result.at("outcome") == "cleared" && !channel.Watches().Find(11) && !channel.Close(ticket),
          "watch: none clears the bot's watch and is no act");
    f.world.Cancel(ticket);

    ticket = open(0);
    f.source.watchFacts = {true, true, 2};
    unsigned const reads = f.source.watchReads;
    result = watch(ticket, "watch-5", "combat_end", nullptr);
    Check(result.at("outcome") == "rejected" && result.at("reason") == "cannot_now" && f.source.watchReads == reads &&
              channel.Watches().Empty(),
          "watch: without the post-map-join phase nothing is read or armed");
    f.source.watchReadable = false;
    result = watch(ticket, "watch-6", "time", &phase);
    Check(result.at("reason") == "cannot_now" && channel.Watches().Empty(), "watch: an unreadable bot is not ready");
    f.source.watchReadable = true;
    channel.Close(ticket);
    f.world.Cancel(ticket);

    ticket = open(3);
    result = watch(ticket, "watch-7", "time", &phase);
    Check(result.at("reason") == "put_off" && channel.Watches().Empty(), "watch: a third wake cannot arm a fourth");
    channel.Close(ticket);
    f.world.Cancel(ticket);
    ticket = open(2);
    result = watch(ticket, "watch-8", "time", &phase);
    Check(result.at("outcome") == "armed" && channel.Watches().Find(11) && channel.Watches().Find(11)->depth == 3,
          "watch: the second wake arms the third");
    channel.Close(ticket);
    f.world.Cancel(ticket);

    ticket = open(0);
    Check(!channel.Close(ticket), "watch: route closed before its watch");
    unsigned const before = f.source.watchReads;
    Check(watch(ticket, "watch-9", "combat_end", &phase).is_null() && channel.Watches().Find(11)->id == "watch-8" &&
              f.source.watchReads == before,
          "watch: a closed route arms nothing");
    f.world.Cancel(ticket);

    auto const dropped = channel.Watches().Counters().dropped;
    channel.Retire();
    Check(channel.Watches().Empty() && channel.Watches().Counters().dropped == dropped + 1,
          "watch: epoch retirement ends every watch");
}

int main()
{
    if (!InstallTestAbilities())  // LC3: the verbs and topics are the installed index's.
    {
        std::cerr << "FAIL: the ability double was refused\n";
        return 1;
    }
    try
    {
        QueryEncoding();
        Currentness();
        Bounds();
        MarkPublication();
        TopicPublication();
        CataloguePublication();
        WatchArming();
        std::cout << "PASS " << checks << " production filtered query/event helper checks\n";
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
