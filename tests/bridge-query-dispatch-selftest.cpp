// The query route end to end: the world adapter and the dispatcher compiled whole (this file includes both production
// .cpp files after its own doubles of the game; tests/doubles names the game headers), with the query, world and
// channel units and serialization linked, and controlled world objects. No server, native transport or provider.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <list>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include "SharedDefines.h"
#include "mod-ollama-chat_bridge_query.h"

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
}  // namespace

// --- game doubles (tests/doubles/game-doubles.h) ---------------------------
struct ObjectGuid
{
    uint64_t value = 0;
    explicit ObjectGuid(uint64_t id = 0) : value(id) {}
    uint64_t GetRawValue() const { return value; }
    uint32_t GetCounter() const { return static_cast<uint32_t>(value); }
    bool operator==(ObjectGuid const&) const = default;
};
struct Group;
struct Player;
struct Guild
{
};
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
    uint8_t getRace() const { return RACE_HUMAN; }
    uint8_t getClass() const { return CLASS_WARRIOR; }
    uint8_t getGender() const { return GENDER_MALE; }
    uint8_t GetLevel() const { return 10; }
    uint32_t GetZoneId() const { return 12; }
    bool InBattleground() const { return false; }
    bool InArena() const { return false; }
    Guild* GetGuild() const { return nullptr; }
    uint32_t GetGuildId() const { return 0; }
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
// No check routes a chat line: every send is refused.
struct PlayerbotAI
{
    Player* master = nullptr;
    Player* GetMaster() { return master; }
    bool IsBotAI() const { return true; }
    bool Say(std::string const&) { return false; }
    bool Yell(std::string const&) { return false; }
    bool Whisper(std::string const&, std::string const&) { return false; }
    bool SayToParty(std::string const&) { return false; }
    bool SayToRaid(std::string const&) { return false; }
    bool SayToGuild(std::string const&) { return false; }
};
struct Channel
{
    bool IsOn(ObjectGuid) const { return false; }
    void Say(ObjectGuid, std::string const&, uint32_t) {}
};
struct ChannelMgr
{
    static ChannelMgr* forTeam(uint32_t) { return nullptr; }
    Channel* GetChannel(std::string const&, Player*) { return nullptr; }
};
namespace
{
Player players[3];
Unit enemy;
Group party;
PlayerbotAI ai;
}  // namespace
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
namespace Acore
{
struct AnyPlayerInObjectRangeCheck
{
    AnyPlayerInObjectRangeCheck(Player*, float, bool, bool) {}
};
template <typename Check>
struct PlayerListSearcher
{
    std::list<Player*>& found;
    PlayerListSearcher(Player*, std::list<Player*>& list, Check&) : found(list) {}
};
namespace Crypto
{
template <std::size_t Size>
std::array<uint8_t, Size> GetRandomBytes()
{
    return {};
}
}  // namespace Crypto
}  // namespace Acore
struct Cell
{
    template <typename Searcher>
    static void VisitObjects(Player*, Searcher&, float)
    {
    }
};
#define OLLAMA_SELFTEST_GAME_DOUBLES

// --- the production units ---------------------------------------------------
#include "../src/mod-ollama-chat_bridge_adapter.cpp"
#include "../src/mod-ollama-chat_dispatch.cpp"

// --- doubles of the module's other units ------------------------------------
bool g_DebugEnabled = false;
bool g_EnableTypingSimulation = false;
uint32_t g_TypingSimulationBaseDelay = 100;
uint32_t g_TypingSimulationDelayPerChar = 10;
uint32_t g_TypingSimulationMaxDelay = 1000;
uint32_t g_MaxQueueDepth = 64;
uint32_t g_DispatchWorkerThreads = 1;
uint32_t g_MaxReplyLength = 240;
bool g_Enable = true;
bool g_EnableWhisperReplies = true;
bool g_DisableForSayYell = false;
bool g_DisableForParty = false;
bool g_DisableForGuild = false;
float g_SayDistance = 25.0f;
float g_YellDistance = 100.0f;
const char* ChatChannelSourceLocalStr[] = {"Undefined", "Say", "Party", "Raid", "Guild", "Officer", "Yell", "Whisper"};

OllamaEndpointSettings OllamaConfig_Snapshot() { return {}; }
OllamaApiResult QueryOllamaBridge(OllamaEndpointSettings const&, OllamaBridge::Request const&) { return {}; }
std::string ProcessLlmResponse(std::string const& text, std::string const&, uint32_t*, uint32_t) { return text; }
std::string Roleplay_FilterMetaTerms(std::string const& text) { return text; }
bool OllamaIsBotPlayer(Player* player) { return !player->human; }
bool OllamaGroupHasRealPlayer(Player*) { return true; }
void OllamaWorldSnapshot::Build() {}
bool OllamaWorldSnapshot::RealPlayerInChannel(Channel*) const { return true; }
bool Governor_IsRepetitive(ObjectGuid, std::string const&, std::string const&) { return false; }
bool Governor_TryConsumeSend(ObjectGuid, std::string const&, bool) { return true; }
void Governor_RecordUtterance(ObjectGuid, std::string const&, std::string const&) {}
std::string Governor_MakeScopeKey(char const*, uint32_t, std::string const&, uint32_t, uint32_t) { return "scope"; }

// The production channel names the world-only typed effect adapter (bridge_effect.cpp). This fixture stands in with
// the closed release gate: every command is refused without a game read. No plan lives here.
OllamaBridge::CommandExecutor& OllamaChat_BridgeCommandExecutor()
{
    struct Closed final : OllamaBridge::CommandExecutor
    {
        OllamaBridge::EffectResult Execute(OllamaBridge::EffectPhase const&, OllamaBridge::WorldTicket const&,
                                           OllamaBridge::DispatchGuard const&, OllamaBridge::ChannelCommand const&,
                                           std::function<bool()> const&) override
        {
            return OllamaBridge::EffectResult::Rejected;
        }
    };
    static Closed executor;
    return executor;
}

// --- doubles of playerbots' readers: the state, facts and traits are fixed; topics are not part of these checks --
namespace PlayerbotBridge
{
bool ReadBotState(ObjectGuid const&, ObjectGuid const&, BotStateFacts& facts)
{
    facts.healthPct = 90.0f;
    facts.alive = true;
    return true;
}
bool ReadBotFacts(ObjectGuid const&, BotFacts&) { return true; }
bool ReadBotPersonality(ObjectGuid const&, BotPersonality&) { return true; }
bool ReadBotStateTopic(ObjectGuid const&, BotStateTopic, std::string const&, std::vector<BotStateItem>& items)
{
    items.clear();
    return false;
}
std::span<AbilityRow const> AbilityRows() { return {}; }
std::vector<AbilityData> Catalogue() { return {}; }
uint64_t CatalogueHash(std::vector<AbilityData> const&) { return 0; }
char const* KindWord(AbilityKind) { return ""; }
char const* KindWord(ParamKind) { return ""; }
char const* CompletionWord(Completion) { return ""; }
}  // namespace PlayerbotBridge

using namespace OllamaBridge;
using Json = nlohmann::json;

namespace
{
OllamaBridge::Clock clockNow;
}  // namespace

namespace
{
Settings const settings{"fixture", "codex", "http://127.0.0.1:47800", "synthetic-game-link-token"};

struct Fixture
{
    WorldTicket ticket;
    DispatchGuard guard;
    Fixture()
    {
        g_bridgeObserver = OllamaChat_BridgeObserve;  // OllamaDispatch_Start's observer.
        onResolve = {};
        resolutions = 0;
        g_bridgeChannel.Retire();
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
        Check(g_bridgeWorld.ObserveDrain(true), "select epoch");
        g_bridgeChannel.Configure(true);
        Check(g_bridgeChannel.Activate(), "activate production dispatcher mailbox");
        BindingCapture view;
        Check(BridgeCapture(11, 22, DispatchSource::Whisper, view), "capture production game scope");
        auto& scope = view.guard.scope;
        scope.contextRevision = scope.membershipRevision = scope.perceptionScope = "rev1";
        guard = view.guard;
        Check(g_bridgeWorld.ObserveScope(scope, clockNow), "observe trusted scope");
        g_bridgeBindings.push_back({g_bridgeWorld.Generation(), view, CaptureDeadline(clockNow, BindingHeartbeatMs),
                                    CaptureDeadline(clockNow, MaxLifetimeMs), ""});
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
        Check(OllamaDispatch_ChannelReceive(ticket.request.requestId, Query(), receipt) == ChannelError::None,
              "actual dispatcher receive");
        return receipt;
    }
    std::unique_ptr<ChannelPacket> Take(uint64_t receipt)
    {
        std::unique_ptr<ChannelPacket> packet;
        Check(OllamaDispatch_ChannelTake(receipt, packet) == ChannelError::None && packet, "actual dispatcher take");
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
        // CaptureQuery resolves three initial units and three validation units; after FinishQuery serialization,
        // Current lists the group again and resolves its units (lookups 7 to 9) before mailbox publication. A unit's
        // change lands at lookup 7, before its own resolve; the membership change at lookup 6, the last read before
        // serialization, since the recheck lists the members before its first lookup.
        onResolve = [change](unsigned count)
        {
            if (count != (change == 7 ? 6u : 7u))
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
        Check(OllamaDispatch_ChannelTake(receipt, packet) == ChannelError::Unknown && !packet &&
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
    // LC.35: there is no event route. A mark change opens no route, holds no packet slot and reserves nothing in the
    // world, however many changes come.
    auto const used = g_bridgeChannel.Used();
    auto const reserved = g_bridgeWorld.Pending();
    for (unsigned i = 0; i < 2 * WorldChannel::MaxPackets; ++i)
    {
        party.marks[0] = i % 2 ? 0 : 77;
        OllamaChat_BridgeRefresh();
    }
    Check(g_bridgeChannel.Used() == used && g_bridgeWorld.Pending() == reserved,
          "128 mark changes open no route and hold no packet slot");
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
        auto const original = Json::parse(f.Query())["deadlineUnixMs"].get<int64_t>();
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
        Check(channel.Receive(f.ticket.request.requestId, f.Query(), receipt) == ChannelError::None,
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
        Check(channel.Take(receipt, packet) == ChannelError::Unknown && !packet,
              "expired query never enters publication map");
    }
    {
        Fixture f;
        for (unsigned i = 0; i < 12; ++i)
        {
            uint64_t receipt = 0;
            Check(OllamaDispatch_ChannelReceive(f.ticket.request.requestId, f.Query("query-" + std::to_string(i)),
                                                receipt) == ChannelError::None,
                  "admit bounded receive batch");
        }
        Check(g_bridgeChannel.Update() == WorldChannel::TickBudget, "eight dequeues per world tick");
        Check(g_bridgeChannel.Used() == 12, "completions and remaining receives share physical slots");
        g_bridgeChannel.Retire();
        Check(g_bridgeChannel.Drained(), "reload disposes both receive and completion payloads");
        Check(!g_bridgeChannel.Activate(), "reconnect cannot reuse retired epoch");
        uint64_t receipt = 0;
        Check(OllamaDispatch_ChannelReceive(f.ticket.request.requestId, f.Query(), receipt) == ChannelError::Inactive,
              "a retired channel cannot admit old routes");
    }
    {
        Fixture f;
        auto receipt = f.Receive();
        g_bridgeWorld.Configured(true, settings, "replacement-epoch");
        Check(g_bridgeChannel.Update() == 0, "reload invalidates selection before world work");
        g_bridgeChannel.Retire();
        std::unique_ptr<ChannelPacket> packet;
        Check(OllamaDispatch_ChannelTake(receipt, packet) == ChannelError::Inactive && g_bridgeChannel.Drained(),
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
        std::cout << "PASS " << checks << " query dispatch checks (adapter and dispatcher compiled whole)\n";
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
