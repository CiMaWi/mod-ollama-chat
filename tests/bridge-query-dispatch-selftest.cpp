// Exact production Player/Group adapter and dispatcher channel glue, with controlled world objects.
// Actual query/world/channel translation units and serialization; no server, native transport or provider.
#include <algorithm>
#include <array>
#include <deque>
#include <functional>
#include <iostream>
#include <list>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "mod-ollama-chat_bridge_query.h"

using namespace OllamaBridge;
using Json = nlohmann::json;

namespace
{
unsigned checks = 0;
unsigned resolutions = 0;
unsigned fixtureSerial = 0;
std::function<void(unsigned)> onResolve;
void Check(bool value, char const* label)
{
    ++checks;
    if (!value)
        throw std::runtime_error(label);
}

struct ObjectGuid
{
    uint64_t value = 0;
    explicit ObjectGuid(uint64_t id = 0) : value(id) {}
    uint64_t GetRawValue() const { return value; }
    bool operator==(ObjectGuid const&) const = default;
};
struct Group;
struct Player;
struct Unit
{
    virtual ~Unit() = default;
    uint64_t guid = 0;
    bool inWorld = true;
    bool visible = true;
    uint32_t map = 1;
    uint32_t instance = 1;
    uint32_t phase = 1;
    float distance = 10;
    float health = 75;
    std::string name = "Unit";
    bool IsInWorld() const { return inWorld; }
    ObjectGuid GetGUID() const { return ObjectGuid(guid); }
    uint32_t GetMapId() const { return map; }
    uint32_t GetInstanceId() const { return instance; }
    uint32_t GetPhaseMask() const { return phase; }
    std::string GetName() const { return name; }
    float GetHealthPct() const { return health; }
    virtual Player* ToPlayer() { return nullptr; }
    bool IsInMap(Unit* unit) const
    {
        return inWorld && unit->inWorld && map == unit->map && instance == unit->instance;
    }
    bool InSamePhase(Unit* unit) const { return (phase & unit->phase) != 0; }
    bool IsWithinDistInMap(Unit* unit, float radius) const { return IsInMap(unit) && unit->distance <= radius; }
    bool CanSeeOrDetect(Unit* unit, bool ignore, bool) const { return ignore || (visible && unit->visible); }
};
struct Player : Unit
{
    bool online = true;
    bool human = false;
    uint32_t team = 0;
    Group* group = nullptr;
    Group* GetGroup() const { return group; }
    uint32_t GetTeamId() const { return team; }
    Player* ToPlayer() override { return this; }
};
struct Group
{
    struct Slot
    {
        ObjectGuid guid;
    };
    std::list<Slot> members{{ObjectGuid(11)}, {ObjectGuid(22)}, {ObjectGuid(33)}};
    std::array<uint64_t, 8> marks{};
    bool raid = false;
    bool isRaidGroup() const { return raid; }
    auto const& GetMemberSlots() const { return members; }
    ObjectGuid GetGUID() const { return ObjectGuid(99); }
    ObjectGuid GetLeaderGUID() const { return ObjectGuid(22); }
    ObjectGuid GetTargetIcon(uint8_t icon) const { return ObjectGuid(marks.at(icon)); }
    bool IsMember(ObjectGuid guid) const
    {
        return std::any_of(members.begin(), members.end(), [&](Slot const& item) { return item.guid == guid; });
    }
};
Player players[3];
Unit enemy;
Group party;
struct PlayerbotAI
{
    Player* master = nullptr;
    Player* GetMaster() { return master; }
    bool IsBotAI() const { return true; }
} ai;
struct PlayerbotsMgr
{
    static PlayerbotsMgr& instance()
    {
        static PlayerbotsMgr result;
        return result;
    }
    PlayerbotAI* GetPlayerbotAI(Player* player) { return player == &players[0] ? &ai : nullptr; }
};
struct ObjectAccessor
{
    static Player* FindConnectedPlayer(ObjectGuid guid)
    {
        for (auto& player : players)
            if (player.guid == guid.value && player.online)
                return &player;
        return nullptr;
    }
    static Unit* GetUnit(Unit const&, ObjectGuid guid)
    {
        ++resolutions;
        if (onResolve)
            onResolve(resolutions);
        if (guid.value == enemy.guid)
            return &enemy;
        return FindConnectedPlayer(guid);
    }
};
bool g_Enable = true;
bool g_EnableWhisperReplies = true;
bool g_DisableForParty = false;
bool g_DisableForSayYell = false;
float g_SayDistance = 25;
float g_YellDistance = 100;
bool OllamaIsRealPlayer(Player* player) { return player && player->human; }
std::string Roleplay_BuildVoicePrompt(Player*) { return "A quiet traveler."; }

Clock clockNow;
WorldPolicy g_bridgeWorld;
}  // namespace

QueryWorldSource& OllamaChat_BridgeQuerySource();
bool OllamaChat_BridgeObserve(WorldTicket const&, DispatchGuard&);
void OllamaChat_BridgeRefresh();
WorldPolicy& OllamaDispatch_BridgeWorld() { return g_bridgeWorld; }
bool OllamaDispatch_ChannelEvents(DispatchGuard const&, std::vector<uint8_t> const&);
// The production channel names the world-only typed effect adapter, which needs game headers. This fixture stands in
// with the closed release gate: every command is refused without a game read.
CommandExecutor& OllamaChat_BridgeCommandExecutor()
{
    struct Closed final : CommandExecutor
    {
        EffectResult Execute(EffectPhase const&, WorldTicket const&, DispatchGuard const&, ChannelCommand const&,
                             std::function<bool()> const&) override
        {
            return EffectResult::Rejected;
        }
    };
    static Closed executor;
    return executor;
}

namespace
{
bool g_nativeSelected = false;
struct NativeObserver
{
    bool Current(WorldScope const&, Clock) const { return true; }
} g_nativeObserver;
auto g_bridgeObserver = ::OllamaChat_BridgeObserve;
}  // namespace

#include "dispatch-channel.inc"
#include "handler-query.inc"

namespace
{
Settings const settings{"fixture", "codex", "http://127.0.0.1:47800", "synthetic-game-link-token", false, "", ""};

struct Fixture
{
    WorldTicket ticket;
    DispatchGuard guard;
    Fixture()
    {
        onResolve = {};
        resolutions = 0;
        g_bridgeChannel.Retire();
        g_eventPublications.clear();
        OllamaChat_BridgeReset();
        g_bridgeWorld.Shutdown();
        clockNow = Now();
        g_Enable = true;
        g_EnableWhisperReplies = true;
        party = {};
        for (unsigned i = 0; i < 3; ++i)
        {
            players[i] = {};
            players[i].guid = (i + 1) * 11;
            players[i].human = i != 0;
            players[i].group = &party;
            players[i].name = "Player" + std::to_string(i);
        }
        enemy = {};
        enemy.guid = 77;
        enemy.name = "Visible enemy";
        ai.master = &players[1];
        g_bridgeWorld.Configured(true, settings, "epoch-" + std::to_string(++fixtureSerial));
        Check(g_bridgeWorld.ObserveDrain(true, RetirementProof::GenerationOnlyNoCaptures), "select epoch");
        g_bridgeChannel.Configure(true);
        Check(g_bridgeChannel.Activate(), "activate production dispatcher mailbox");
        BridgeView view;
        Check(BridgeCapture(11, 22, DispatchSource::Whisper, view), "capture production game scope");
        auto& scope = view.guard.scope;
        scope.contextRevision = scope.membershipRevision = scope.personaRevision = scope.perceptionScope = "rev1";
        guard = view.guard;
        Check(g_bridgeWorld.ObserveScope(scope, clockNow), "observe trusted scope");
        g_bridgeBindings.push_back({g_bridgeWorld.Generation(),
                                    view,
                                    CaptureDeadline(clockNow, WorldLivenessMs),
                                    CaptureDeadline(clockNow, MaxLifetimeMs),
                                    "",
                                    {}});
        auto input = view.context;
        input.text = "What can we see?";
        ticket = g_bridgeWorld.Reserve(scope, input, clockNow, 30000);
        Check(ticket.generation && g_bridgeChannel.Register(ticket, guard), "register game generation");
        resolutions = 0;
    }
    ~Fixture()
    {
        onResolve = {};
        g_bridgeChannel.Retire();
        g_eventPublications.clear();
        OllamaChat_BridgeReset();
        g_bridgeWorld.Shutdown();
    }
    std::string Query(std::string id = "query-1") const
    {
        return Json{{"version", 1},
                    {"type", "query"},
                    {"instance", "fixture"},
                    {"provider", "codex"},
                    {"epoch", ticket.scope.epoch},
                    {"requestId", id},
                    {"deadlineUnixMs", ticket.request.deadline.unixMs},
                    {"controllerGuid", "22"},
                    {"botGuid", "11"},
                    {"query", "observable_group"}}
            .dump();
    }
    uint64_t Receive()
    {
        uint64_t receipt = 0;
        Check(OllamaDispatch_ChannelReceive(settings.token, ticket.request.requestId, Query(), receipt) ==
                  ChannelError::None,
              "actual dispatcher receive");
        return receipt;
    }
    std::unique_ptr<ChannelPacket> Take(uint64_t receipt)
    {
        std::unique_ptr<ChannelPacket> packet;
        Check(OllamaDispatch_ChannelTake(settings.token, receipt, packet) == ChannelError::None && packet,
              "actual dispatcher take");
        return packet;
    }
};

void Queries()
{
    {
        Fixture f;
        players[2].visible = false;
        auto receipt = f.Receive();
        Check(g_bridgeChannel.Update() == 1, "dequeue actual query");
        auto packet = f.Take(receipt);
        auto value = Json::parse(packet->body);
        Check(value["type"] == "query.result" && value["units"].size() == 2, "production visible query result");
        for (auto const& unit : value["units"])
            Check(unit.contains("healthPercent") && !unit.contains("role") && !unit.contains("visibleTargetGuid"),
                  "UI health only; unknown role/target omitted");
        g_bridgeChannel.Retire();
        Check(g_bridgeChannel.Used() == 1, "asynchronous payload owns slot after retirement");
        packet.reset();
        Check(g_bridgeChannel.Drained(), "payload disposal releases slot");
    }
    for (unsigned change = 0; change < 8; ++change)
    {
        Fixture f;
        auto receipt = f.Receive();
        // CaptureQuery resolves three initial units and three validation units. The seventh lookup is
        // Current AFTER actual FinishQuery serialization, immediately before mailbox publication.
        onResolve = [change](unsigned count)
        {
            if (count != 7)
                return;
            switch (change)
            {
                case 0:
                    players[2].visible = false;
                    break;
                case 1:
                    players[2].online = false;
                    break;
                case 2:
                    players[2].health = 20;
                    break;
                case 3:
                    players[2].name = "Changed";
                    break;
                case 4:
                    players[2].map = 2;
                    break;
                case 5:
                    players[2].phase = 2;
                    break;
                case 6:
                    players[2].group = nullptr;
                    break;
                case 7:
                    party.members.pop_back();
                    break;
            }
        };
        Check(g_bridgeChannel.Update() == 1 && resolutions >= 7, "reach post-serialization world validation");
        std::unique_ptr<ChannelPacket> packet;
        Check(OllamaDispatch_ChannelTake(settings.token, receipt, packet) == ChannelError::Unknown && !packet &&
                  g_bridgeChannel.Drained(),
              "stale serialized query never becomes consumable");
    }
    for (unsigned change = 0; change < 5; ++change)
    {
        Fixture f;
        if (change == 0)
            players[2].online = false;
        if (change == 1)
            players[2].distance = 26;
        if (change == 2)
            players[2].map = 2;
        if (change == 3)
            players[2].instance = 2;
        if (change == 4)
            players[2].phase = 2;
        auto receipt = f.Receive();
        g_bridgeChannel.Update();
        auto packet = f.Take(receipt);
        Check(Json::parse(packet->body)["units"].size() == 2, "current missing/perception-filtered group units");
    }
}

void Events()
{
    Fixture f;
    party.marks[0] = 77;
    OllamaChat_BridgeRefresh();
    EventPublication publication;
    Check(OllamaDispatch_ChannelNextEvent(publication) && publication.receipt &&
              publication.route != f.ticket.request.requestId,
          "bounded existing refresh publishes distinct event route");
    Check(!OllamaDispatch_ChannelNextEvent(publication), "handoff is once only");
    // The false handoff clears its output. Publish another icon change to obtain a new receipt.
    party.marks[1] = 77;
    OllamaChat_BridgeRefresh();
    Check(OllamaDispatch_ChannelNextEvent(publication), "second mark observation");
    auto packet = f.Take(publication.receipt);
    auto value = Json::parse(packet->body);
    Check(value["type"] == "events" && value["requestId"] == publication.route && value["events"].size() == 2,
          "actual event serialization and route join");
    Check(value["deadlineUnixMs"].get<int64_t>() <= Now().unixMs + WorldLivenessMs, "event age capped at five seconds");
    Check(OllamaDispatch_ChannelReceive("wrong", publication.route, "{}", publication.receipt) ==
              ChannelError::Unauthorized,
          "every I/O operation authenticates");
    packet.reset();
    auto pending = g_bridgeWorld.Pending();
    EventPublication failed;
    Check(!g_bridgeChannel.ObserveEvents(f.guard, {8}, failed) && !failed.receipt && failed.route.empty() &&
              g_bridgeWorld.Pending() == pending,
          "malformed mark admission cancels game reservation");
    enemy.visible = false;
    Check(!g_bridgeChannel.ObserveEvents(f.guard, {0}, failed) && g_bridgeWorld.Pending() == pending,
          "hidden mark fails closed and cleans reservation");
    enemy.visible = true;
    Context input;
    input.speakerGuid = 22;
    input.channel = f.guard.scope.channel;
    input.text = "Observed marks.";
    auto ticket = g_bridgeWorld.Reserve(f.guard.scope, input, Now(), WorldLivenessMs);
    FilteredQueryAdapter adapter(g_bridgeWorld, OllamaChat_BridgeQuerySource());
    MarkObservation observed;
    Check(adapter.CaptureMarks(ticket, f.guard, {0}, observed), "capture final-event failure fixture");
    uint64_t receipt = 0;
    bool validated = false;
    Check(!g_bridgeChannel.RegisterEvents(ticket, f.guard, observed.marks, receipt,
                                          [&]
                                          {
                                              validated =
                                                  true;  // Invoked after OpenEvents serialized, before insertion.
                                              enemy.visible = false;
                                              return adapter.Current(observed);
                                          }) &&
              validated && !receipt,
          "post-serialization hidden mark never publishes receipt");
    g_bridgeChannel.Close(ticket);
    g_bridgeWorld.Cancel(ticket);
    enemy.visible = true;
    for (unsigned i = 0; i < WorldChannel::MaxPackets; ++i)
    {
        if (g_bridgeChannel.Used() >= WorldChannel::MaxPackets || g_bridgeWorld.Pending() >= MaxWorldPending)
            break;
        Check(g_bridgeChannel.ObserveEvents(f.guard, {0}, failed), "all events share physical packet capacity");
    }
    while (g_bridgeChannel.Used() < WorldChannel::MaxPackets)
        f.Receive();
    pending = g_bridgeWorld.Pending();
    Check(!g_bridgeChannel.ObserveEvents(f.guard, {0}, failed) && g_bridgeWorld.Pending() == pending,
          "full shared capacity rejects event and cancels reservation");
    g_bridgeChannel.Retire();
    Check(g_bridgeChannel.Drained(), "retirement disposes all queued event payloads");
}

void Lifetimes()
{
    {
        Fixture f;
        // Reproduce independent clock samples crossing a wall-clock millisecond boundary.
        // The policy's monotonic remainder must be tightened, preserving the original wire deadline.
        ++clockNow.unixMs;
        WorldChannel channel(
            g_bridgeWorld, OllamaChat_BridgeObserve, [] { return clockNow; }, &OllamaChat_BridgeQuerySource());
        channel.Configure(true);
        Check(channel.Activate() && channel.Register(f.ticket, f.guard),
              "one millisecond wall/steady skew cannot falsely reject captured generation");
        party.marks[0] = 77;
        EventPublication publication;
        Check(channel.ObserveEvents(f.guard, {0}, publication) && publication.receipt,
              "capture distinct controlled-clock event");
        auto const original = Json::parse(f.Query())["deadlineUnixMs"].get<int64_t>();
        std::unique_ptr<ChannelPacket> packet;
        clockNow.steadyMs += WorldLivenessMs;
        clockNow.unixMs -= 10000;
        Check(channel.Take(settings.token, publication.receipt, packet) == ChannelError::Stale && !packet &&
                  channel.Drained(),
              "event original monotonic five-second cap survives UTC rollback");
        Check(f.ticket.request.deadline.unixMs == original, "channel cap preserves original wire deadline");
    }
    for (unsigned clock = 0; clock < 3; ++clock)
    {
        Fixture f;
        WorldChannel channel(
            g_bridgeWorld, OllamaChat_BridgeObserve, [] { return clockNow; }, &OllamaChat_BridgeQuerySource());
        channel.Configure(true);
        Check(channel.Activate() && channel.Register(f.ticket, f.guard), "controlled-clock production channel");
        uint64_t receipt = 0;
        Check(channel.Receive(settings.token, f.ticket.request.requestId, f.Query(), receipt) == ChannelError::None,
              "capture bounded query packet age");
        if (clock == 0)
            clockNow.unixMs += WorldLivenessMs;
        else
        {
            clockNow.steadyMs += WorldLivenessMs;
            if (clock == 2)
                clockNow.unixMs -= 10000;
        }
        Check(g_bridgeWorld.ObserveScope(f.guard.scope, clockNow), "heartbeat cannot renew packet age");
        Check(channel.Update() == 1 && channel.Drained(), "either clock expires at equality, including rollback");
        std::unique_ptr<ChannelPacket> packet;
        Check(channel.Take(settings.token, receipt, packet) == ChannelError::Unknown && !packet,
              "expired query never enters publication map");
    }
    {
        Fixture f;
        for (unsigned i = 0; i < 12; ++i)
        {
            uint64_t receipt = 0;
            Check(OllamaDispatch_ChannelReceive(settings.token, f.ticket.request.requestId,
                                                f.Query("query-" + std::to_string(i)), receipt) == ChannelError::None,
                  "admit bounded receive batch");
        }
        Check(g_bridgeChannel.Update() == WorldChannel::TickBudget, "eight dequeues per world tick");
        Check(g_bridgeChannel.Used() == 12, "completions and remaining receives share physical slots");
        g_bridgeChannel.Retire();
        Check(g_bridgeChannel.Drained(), "reload disposes both receive and completion payloads");
        Check(!g_bridgeChannel.Activate(), "reconnect cannot reuse retired epoch");
        uint64_t receipt = 0;
        Check(OllamaDispatch_ChannelReceive(settings.token, f.ticket.request.requestId, f.Query(), receipt) ==
                  ChannelError::Unauthorized,
              "retired bearer cannot admit old routes");
    }
    {
        Fixture f;
        auto receipt = f.Receive();
        g_bridgeWorld.Configured(true, settings, "replacement-epoch");
        Check(g_bridgeChannel.Update() == 0, "reload invalidates selection before world work");
        g_bridgeChannel.Retire();
        std::unique_ptr<ChannelPacket> packet;
        Check(OllamaDispatch_ChannelTake(settings.token, receipt, packet) == ChannelError::Unauthorized &&
                  g_bridgeChannel.Drained(),
              "reload failure disposes old queued payload");
    }
}
}  // namespace

int main()
{
    try
    {
        Queries();
        Events();
        Lifetimes();
        std::cout << "PASS " << checks << " production query/dispatch/game-adapter checks\n";
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
