// Generated bridge speech never reaches playerbots or mod-ollama-chat order parsing (J6b4a, hub J6b4-check runner).
// Real: PrepareBridge, RouteMessage, DeliverBridgeLine, ProposeBridgeCard, DeliverBridge and
// OllamaDispatch_SubmitBridge (extracted verbatim), the bridge policy units and playerbots' ChatCommandLine.h. Models:
// the playerbots whisper hook (a whisper to a bot parses a command line) and the ProcessChat bridge gate (only a
// human's line opens a request). Game objects, the governor, the API and cleanup are stand-ins. No full dispatch TU or
// live-game proof.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <list>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "ChatCommandLine.h"
#include "mod-ollama-chat_bridge_query.h"
#include "mod-ollama-chat_bridge_world.h"
#include "mod-ollama-chat_persona_db.h"

enum class OllamaProvider
{
    Ollama,
    Bridge
};
enum class OllamaRequestKind
{
    ChatReply,
    EventChatter
};
enum ChatChannelSourceLocal
{
    SRC_SAY_LOCAL = 1,
    SRC_PARTY_LOCAL = 2,
    SRC_RAID_LOCAL = 3,
    SRC_GUILD_LOCAL = 4,
    SRC_OFFICER_LOCAL = 5,
    SRC_YELL_LOCAL = 6,
    SRC_WHISPER_LOCAL = 7,
    SRC_GENERAL_LOCAL = 17
};
using OllamaBridgeObserver = bool (*)(OllamaBridge::WorldTicket const&, OllamaBridge::DispatchGuard&);
#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)

// The channel glue the shared dispatch state names; this check opens no channel route (g_channelEnabled false).
OllamaBridge::QueryWorldSource& OllamaChat_BridgeQuerySource()
{
    struct Blind final : OllamaBridge::QueryWorldSource
    {
        bool Observe(OllamaBridge::WorldTicket const&, OllamaBridge::DispatchGuard&) override { return false; }
        bool GroupMembers(OllamaBridge::WorldTicket const&, uint64_t, std::vector<uint64_t>&) override { return false; }
        bool ResolveVisible(OllamaBridge::WorldTicket const&, uint64_t, OllamaBridge::VisibleUnit&) override
        {
            return false;
        }
        bool ReadMark(OllamaBridge::WorldTicket const&, uint8_t, uint64_t&) override { return false; }
    };
    static Blind source;
    return source;
}
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
OllamaHttpResult OllamaHttp_PostObserver(OllamaBridge::Settings const&, OllamaObserverOperation, std::string const&,
                                         int64_t)
{
    return {};
}
namespace OllamaPersona
{
void ForgetPersona(uint64_t) {}
void RetirePersonas(uint64_t) {}
}  // namespace OllamaPersona
bool OllamaDispatch_ChannelEvents(OllamaBridge::DispatchGuard const&, std::vector<uint8_t> const&) { return false; }

#include "dispatch-types.inc"

OllamaBridge::WorldPolicy& OllamaDispatch_BridgeWorld() { return g_bridgeWorld; }

namespace
{
unsigned checks = 0;
unsigned governorCalls = 0;
unsigned recordCalls = 0;
unsigned rememberCalls = 0;
bool g_DebugEnabled = false;
bool g_EnableTypingSimulation = false;
uint32_t g_TypingSimulationBaseDelay = 100;
uint32_t g_TypingSimulationDelayPerChar = 10;
uint32_t g_TypingSimulationMaxDelay = 1000;
uint32_t g_MaxQueueDepth = 64;
bool g_DisableForGuild = false;
bool g_DisableForParty = false;
bool g_DisableForSayYell = false;
float g_SayDistance = 25.0f;
float g_YellDistance = 100.0f;
constexpr uint32_t LANG_UNIVERSAL = 0;
OllamaEndpointSettings config;
OllamaApiResult apiResult;
OllamaBridge::DispatchGuard gameGuard;

void Check(bool condition, std::string const& message)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}

OllamaEndpointSettings OllamaConfig_Snapshot() { return config; }
OllamaApiResult QueryOllamaBridge(OllamaEndpointSettings const&, OllamaBridge::Request const&) { return apiResult; }
std::string ProcessLlmResponse(std::string const& text, std::string const&, uint32_t*) { return text; }
std::string Roleplay_FilterMetaTerms(std::string const& text) { return text; }

struct ObjectGuid
{
    uint64_t value;
    explicit ObjectGuid(uint64_t guid) : value(guid) {}
    uint64_t GetRawValue() const { return value; }
    bool operator==(ObjectGuid const&) const = default;
};
struct Group
{
};
struct Guild
{
};
struct Player;
struct PlayerbotAI
{
    Player* self = nullptr;
    bool SayToGuild(std::string const& text);
    bool SayToParty(std::string const& text);
    bool SayToRaid(std::string const& text);
    bool Yell(std::string const& text);
    bool Say(std::string const& text);
    bool Whisper(std::string const& text, std::string const& receiverName);
};
struct Player
{
    uint64_t guid = 0;
    std::string name;
    bool human = false;
    bool online = true;
    Group* group = nullptr;
    Guild* guild = nullptr;
    PlayerbotAI* ai = nullptr;  // Set for bots: the receivers whose playerbots whisper hook parses.
    bool IsInWorld() const { return online; }
    ObjectGuid GetGUID() const { return ObjectGuid(guid); }
    std::string GetName() const { return name; }
    Group* GetGroup() const { return group; }
    Guild* GetGuild() const { return guild; }
    uint32_t GetGuildId() const { return guild ? 5 : 0; }
};
Group party;
Guild guild;
PlayerbotAI botAI;
PlayerbotAI otherAI;
// The speaking bot, its human controller, the human speaker, and a second bot of the group.
Player players[4];
Player& bot = players[0];
Player& otherBot = players[3];

struct ObjectAccessor
{
    static Player* FindConnectedPlayer(ObjectGuid guid)
    {
        for (auto& player : players)
            if (player.guid == guid.value && player.online)
                return &player;
        return nullptr;
    }
};
struct PlayerbotsMgr
{
    static PlayerbotsMgr& instance()
    {
        static PlayerbotsMgr manager;
        return manager;
    }
    PlayerbotAI* GetPlayerbotAI(Player* player) { return player ? player->ai : nullptr; }
};
struct Channel;
struct OllamaWorldSnapshot
{
    bool RealPlayerInChannel(Channel*) const { return true; }
    bool GuildHasRealPlayer(uint32_t) const { return true; }
};
bool OllamaIsRealPlayer(Player* player) { return player && player->human; }
bool OllamaGroupHasRealPlayer(Player* player) { return player->group != nullptr; }
bool OllamaIsOnChannel(Channel*, ObjectGuid) { return true; }
bool AnyoneInRange(Player*, float) { return true; }

// Everything a bot line reaches: the playerbots whisper hook and the ProcessChat bridge gate.
struct Heard
{
    int source;
    uint64_t sender;
    uint64_t receiver;
    std::string text;
};
std::vector<Heard> heard;
unsigned commandLines = 0;  // Lines playerbots would parse on any channel that had a hook.
unsigned pbParses = 0;      // Lines the playerbots whisper hook really parses: a whisper to a bot receiver.
unsigned ocRequests = 0;    // Lines the ProcessChat bridge gate lets through to a new bridge request.
Completion* nested = nullptr;
void DeliverBridgeAgain(Completion const& c);

// handler.cpp ProcessChat: `!g_Enable || !BridgeHuman(player) || chainDepth || channel || source not in the four`.
bool OcBridgeGate(Player* sender, int source, bool channel)
{
    return sender && OllamaIsRealPlayer(sender) && !channel &&
           (source == SRC_WHISPER_LOCAL || source == SRC_PARTY_LOCAL || source == SRC_SAY_LOCAL ||
            source == SRC_YELL_LOCAL);
}

void Hear(Player* sender, Player* receiver, int source, std::string const& text)
{
    heard.push_back({source, sender->guid, receiver ? receiver->guid : 0, text});
    std::string command;
    if (PlayerbotBridge::ExtractChatCommand(text, command))
    {
        ++commandLines;
        // Playerbots.cpp whisper hook: a whisper to a bot receiver with a command line runs HandleCommand.
        if (source == SRC_WHISPER_LOCAL && PlayerbotsMgr::instance().GetPlayerbotAI(receiver))
            ++pbParses;
    }
    if (OcBridgeGate(sender, source, source == SRC_GENERAL_LOCAL))
        ++ocRequests;
    // A hook that fires during the send delivers the same completion again: it must find the claim taken.
    if (nested)
    {
        auto* again = nested;
        nested = nullptr;
        DeliverBridgeAgain(*again);
    }
}
struct Channel
{
    void Say(ObjectGuid sender, std::string const& text, uint32_t)
    {
        Hear(ObjectAccessor::FindConnectedPlayer(sender), nullptr, SRC_GENERAL_LOCAL, text);
    }
} general;
Channel* ResolveChannel(Player*, std::string const& name) { return name.empty() ? nullptr : &general; }

bool PlayerbotAI::SayToGuild(std::string const& text)
{
    Hear(self, nullptr, SRC_GUILD_LOCAL, text);
    return true;
}
bool PlayerbotAI::SayToParty(std::string const& text)
{
    Hear(self, nullptr, SRC_PARTY_LOCAL, text);
    return true;
}
bool PlayerbotAI::SayToRaid(std::string const& text)
{
    Hear(self, nullptr, SRC_RAID_LOCAL, text);
    return true;
}
bool PlayerbotAI::Yell(std::string const& text)
{
    Hear(self, nullptr, SRC_YELL_LOCAL, text);
    return true;
}
bool PlayerbotAI::Say(std::string const& text)
{
    Hear(self, nullptr, SRC_SAY_LOCAL, text);
    return true;
}
bool PlayerbotAI::Whisper(std::string const& text, std::string const& receiverName)
{
    for (auto& player : players)
        if (player.name == receiverName && player.online)
        {
            Hear(self, &player, SRC_WHISPER_LOCAL, text);
            return true;
        }
    return false;
}

bool Governor_IsRepetitive(ObjectGuid, std::string const&, std::string const&) { return false; }
bool Governor_TryConsumeSend(ObjectGuid, std::string const&, bool)
{
    ++governorCalls;
    return true;
}
void Governor_RecordUtterance(ObjectGuid, std::string const&, std::string const&) { ++recordCalls; }
void OllamaChat_BridgeRemember(OllamaBridge::WorldTicket const&, std::string const&) { ++rememberCalls; }
OllamaPersona::ProposalStatus OllamaChat_BridgeProposeCard(OllamaBridge::WorldTicket const&, uint64_t, uint64_t,
                                                           std::string)
{
    return {};
}
bool Observe(OllamaBridge::WorldTicket const&, OllamaBridge::DispatchGuard& guard)
{
    guard = gameGuard;
    return true;
}
}  // namespace

#include "speech-helpers.inc"

namespace
{
void DeliverBridgeAgain(Completion const& c) { DeliverBridge(c, {}); }

void Reset()
{
    g_queue.clear();
    g_done.clear();
    Check(OllamaBridge::DispatchSlot::Used() == 0, "all physical slots disposed");
    config = {};
    config.provider = OllamaProvider::Bridge;
    config.bridge = {"fixture", "codex", "http://127.0.0.1:47800", "synthetic-game-link-bearer-only"};
    g_bridgeWorld.Configured(true, config.bridge, "epoch-1");
    Check(g_bridgeWorld.ObserveDrain(true, OllamaBridge::RetirementProof::NoLiveCaptures), "selected");
    g_bridgeGeneration = g_bridgeWorld.Generation();
    gameGuard = {};
    gameGuard.scope = {1, "fixture", "codex", "epoch-1", 11, 22, "conversation", "party", "visible", "c1", "m1", "p1"};
    gameGuard.source = OllamaBridge::DispatchSource::Party;
    gameGuard.speakerGuid = 33;
    gameGuard.groupGuid = 99;
    gameGuard.phaseMask = 1;
    gameGuard.audience = {11, 22, 33};
    gameGuard.humanSpeaker = true;
    gameGuard.authority = true;
    gameGuard.perceptible = true;
    gameGuard.audienceOnline = true;
    gameGuard.nonRaid = true;
    Check(g_bridgeWorld.ObserveScope(gameGuard.scope, OllamaBridge::Now()), "scope observed");
    g_bridgeObserver = Observe;
    g_running = true;
    apiResult = {};
    apiResult.ok = true;
    g_DisableForGuild = g_DisableForParty = g_DisableForSayYell = false;
    players[0] = {11, "Bot", false, true, &party, &guild, &botAI};
    players[1] = {22, "Controller", true, true, &party, &guild, nullptr};
    players[2] = {33, "Speaker", true, true, &party, &guild, nullptr};
    players[3] = {44, "OtherBot", false, true, &party, &guild, &otherAI};
    botAI.self = &bot;
    otherAI.self = &otherBot;
    heard.clear();
    commandLines = pbParses = ocRequests = governorCalls = recordCalls = rememberCalls = 0;
    nested = nullptr;
}

// One party request from the human speaker, run through the real worker step; the route is chosen afterwards.
Completion Generate(std::string const& text, ChatChannelSourceLocal source, uint64_t target)
{
    apiResult.text = text;
    OllamaChatRequest request;
    request.botGuid = 11;
    request.targetGuid = 33;
    request.botName = "Bot";
    request.source = SRC_PARTY_LOCAL;
    request.directAddress = true;
    request.scopeKey = "party-99";
    OllamaBridge::Context input;
    input.speakerGuid = 33;
    input.channel = "party";
    input.text = "What now?";
    input.directAddress = true;
    Check(OllamaDispatch_SubmitBridge(std::move(request), gameGuard.scope, std::move(input), gameGuard, 1000),
          "human request admitted");
    Task task = std::move(g_queue.front());
    g_queue.pop_front();
    RunBridgeTask(task);
    Completion done = std::move(g_done.front());
    g_done.pop_front();
    done.request.source = source;
    done.request.targetGuid = target;
    done.request.channelName = source == SRC_GENERAL_LOCAL ? "General" : "";
    return done;
}

// The one-shot claim is gone, no request is open and no slot is left once the completion is disposed.
void ExpectReleased(Completion& done, std::string const& label)
{
    auto const ticket = done.request.bridgeTicket;
    auto const now = OllamaBridge::Now();
    Check(!g_bridgeWorld.Current(ticket, now) && !g_bridgeWorld.Pending() && !g_bridgeWorld.Consume(ticket, now),
          label + ": claim released");
    Check(g_queue.empty() && g_done.empty(), label + ": no nested request opened");
    done = {};
    Check(OllamaBridge::DispatchSlot::Used() == 0, label + ": no slot left");
}

struct Route
{
    ChatChannelSourceLocal source;
    uint64_t target;
    int heardAs;
    char const* label;
};
Route const routes[] = {
    {SRC_WHISPER_LOCAL, 33, SRC_WHISPER_LOCAL, "whisper to the human"},
    {SRC_WHISPER_LOCAL, 44, SRC_WHISPER_LOCAL, "whisper to a bot"},
    {SRC_SAY_LOCAL, 33, SRC_SAY_LOCAL, "say"},
    {SRC_YELL_LOCAL, 33, SRC_YELL_LOCAL, "yell"},
    {SRC_PARTY_LOCAL, 33, SRC_PARTY_LOCAL, "party"},
    {SRC_RAID_LOCAL, 33, SRC_RAID_LOCAL, "raid"},
    {SRC_GUILD_LOCAL, 33, SRC_GUILD_LOCAL, "guild"},
    {SRC_OFFICER_LOCAL, 33, SRC_GUILD_LOCAL, "officer"},
    {SRC_GENERAL_LOCAL, 33, SRC_GENERAL_LOCAL, "general"},
};

// (i, ii, iv, v) Every route: the line arrives neutralized, nothing parses it, a nested delivery finds the claim
// taken, and the delivered line fed back as the bot's own chat opens no request.
void SpeechTests()
{
    std::pair<std::string, std::string> const lines[] = {
        {"#follow", "follow"},
        {"##w follow", "w follow"},
        {"# logout", "logout"},
        {"#c food", "c food"},
        {"#\tattack", "attack"},
        {"##p ready", "p ready"},
        {"# Plan: we pull at dawn", "Plan: we pull at dawn"},
        {"#follow\\\\stay", "follow\\\\stay"},
        {std::string(4096, '#') + "follow", "follow"},
        {std::string(4096, '#'), "#"},
        {"# ", "# "},
        {"follow", "follow"},
        {"stay && follow", "stay && follow"},
        {"follow\\\\stay", "follow\\\\stay"},
    };
    for (auto const& route : routes)
        for (auto const& [generated, expected] : lines)
        {
            std::string const label = std::string(route.label) + " '" + generated.substr(0, 24) + "'";
            Reset();
            auto done = Generate(generated, route.source, route.target);
            Check(PrepareBridge(done) && done.text == expected, label + ": prepared text neutralized");
            nested = &done;
            DeliverBridge(done, {});
            Check(!nested, label + ": the send fired the hooks");
            Check(heard.size() == 1 && heard[0].text == expected && heard[0].source == route.heardAs &&
                      heard[0].sender == 11 &&
                      (route.heardAs != SRC_WHISPER_LOCAL || heard[0].receiver == route.target),
                  label + ": delivered once, neutralized, on its route");
            Check(!commandLines && !pbParses && !ocRequests, label + ": no parse and no bridge request");
            Check(governorCalls == 1 && recordCalls == 1 && rememberCalls == 1, label + ": delivered as chat");
            ExpectReleased(done, label);
            // (v) The bot's own line coming back through every chat hook opens no request and parses nothing.
            for (auto const& line : std::vector<Heard>(heard))
                for (int source : {SRC_WHISPER_LOCAL, SRC_PARTY_LOCAL, SRC_SAY_LOCAL, SRC_YELL_LOCAL})
                {
                    Hear(&bot, &otherBot, source, line.text);
                    Hear(&otherBot, &bot, source, line.text);
                }
            Check(!commandLines && !pbParses && !ocRequests, label + ": no feedback loop");
        }
    // The gate model is not vacuous: the same line from a human opens a request, a command line parses for a bot.
    Reset();
    Hear(&players[2], &bot, SRC_WHISPER_LOCAL, "#follow");
    Check(ocRequests == 1 && pbParses == 1 && commandLines == 1, "gate and hook models see a human's command line");
}

// (iii) A rejected route and a command line from any source leave no claim and send nothing.
void RejectionTests()
{
    struct Rejected
    {
        ChatChannelSourceLocal source;
        uint64_t target;
        bool noGroup;
        bool sayYellOff;
        char const* label;
    };
    Rejected const cases[] = {
        {SRC_WHISPER_LOCAL, 77, false, false, "whisper target gone"},
        {SRC_SAY_LOCAL, 33, false, true, "say disabled"},
        {SRC_YELL_LOCAL, 33, false, true, "yell disabled"},
        {SRC_PARTY_LOCAL, 33, true, false, "party without group"},
        {SRC_GENERAL_LOCAL, 33, false, false, "general channel unresolved"},
    };
    for (auto const& rejected : cases)
    {
        Reset();
        auto done = Generate("#follow", rejected.source, rejected.target);
        Check(PrepareBridge(done) && done.text == "follow", std::string(rejected.label) + ": prepared");
        if (rejected.noGroup)
            bot.group = nullptr;
        g_DisableForSayYell = rejected.sayYellOff;
        if (rejected.source == SRC_GENERAL_LOCAL)
            done.request.channelName.clear();
        DeliverBridge(done, {});
        Check(heard.empty() && !recordCalls && !rememberCalls, std::string(rejected.label) + ": nothing sent");
        ExpectReleased(done, rejected.label);
    }
    // Fail closed: a command line that reaches delivery by any other source is dropped before the send budget.
    for (auto const& route : routes)
        for (std::string const text : {"#follow", "##w follow", "# logout", "#c food"})
        {
            std::string const label = std::string(route.label) + " forced '" + text + "'";
            Reset();
            auto done = Generate("Ready.", route.source, route.target);
            Check(PrepareBridge(done) && done.text == "Ready.", label + ": prepared");
            done.text = text;
            DeliverBridge(done, {});
            Check(heard.empty() && !governorCalls && !recordCalls && !rememberCalls, label + ": dropped unsent");
            ExpectReleased(done, label);
        }
    // The fixed outage line is chat and still goes out.
    Reset();
    auto done = Generate("unused", SRC_WHISPER_LOCAL, 44);
    done.bridgeOutcome = OllamaBridge::WorldOutcome::Unavailable;
    Check(PrepareBridge(done) && done.text == OllamaBridge::DispatchOutageText(), "outage prepared");
    DeliverBridge(done, {});
    Check(heard.size() == 1 && heard[0].text == OllamaBridge::DispatchOutageText() && !pbParses, "outage delivered");
    ExpectReleased(done, "outage");
}
}  // namespace

int main()
{
    try
    {
        SpeechTests();
        RejectionTests();
        g_queue.clear();
        g_done.clear();
        Check(!OllamaBridge::DispatchSlot::Used(), "final no leaked slots");
        std::cout << checks << " bridge speech routing checks passed (extracted delivery; game/hook models)\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
