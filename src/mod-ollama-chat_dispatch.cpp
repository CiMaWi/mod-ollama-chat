#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_bridge_listener.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_persona_bridge.h"
#include "mod-ollama-chat_reply_split.h"
#include "mod-ollama-chat_response.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat-utilities.h"
#include "mod-ollama-chat_world.h"

#include "CellImpl.h"
#include "CryptoRandom.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Guild.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "ChatCommandLine.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "ThreadCheck.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

static_assert(OllamaBridge::MaxCardProposalBytes == OllamaPersona::MaxCardBytes);

namespace
{
    using Clock = std::chrono::steady_clock;

    struct Task
    {
        OllamaBridge::DispatchSlot bridgeSlot;
        OllamaChatRequest request;
    };

    struct Completion
    {
        OllamaBridge::DispatchSlot bridgeSlot;
        OllamaBridge::WorldOutcome bridgeOutcome = OllamaBridge::WorldOutcome::Cancelled;
        bool bridgePrepared = false;
        OllamaChatRequest request;
        std::string       text;
        std::string secondText;  // LC10: the bridge reply's second chat message, empty for one message.
        uint32_t          emoteId = 0;
        Clock::time_point deliverAt;
        std::optional<std::string> personaProposal;  // Bridge card proposal; never chat text.
    };

    // --- shared state -----------------------------------------------------

    std::mutex              g_queueMutex;
    std::condition_variable g_queueCv;
    std::deque<Task>        g_queue;
    bool                    g_running = false;

    std::mutex             g_doneMutex;
    std::deque<Completion> g_done;

    std::vector<std::thread> g_workers;

// Policy and selection are world-thread only; worker counters/tags carry no game references.
OllamaBridge::WorldPolicy g_bridgeWorld;
OllamaBridgeObserver g_bridgeObserver = nullptr;
bool g_channelEnabled = false;
OllamaBridge::CommandStats g_loggedCommands;  // World-thread: the counters in the last watchlist line.
uint64_t g_bridgeSilent = 0;  // World-thread: delivered action-only answers (DS.14), in the commands line.
uint64_t g_loggedSilent = 0;  // World-thread: the silent count in the last watchlist line.
uint64_t g_bridgeSplit = 0;   // World-thread: replies delivered as two chat messages (LC10), in the commands line.
uint64_t g_bridgeRefusedLong = 0;  // World-thread: replies refused for needing three messages (LC10), the same.
uint64_t g_loggedSplit = 0;
uint64_t g_loggedRefusedLong = 0;
Clock::time_point g_nextCommandsLog{};
OllamaBridge::PlanCounters g_loggedPlans;  // World-thread: the plan counters in the last watchlist line.
Clock::time_point g_nextPlansLog{};
OllamaBridge::WatchCounters g_loggedWatches;  // World-thread: the watch counters in the last watchlist line (J6e-c).
Clock::time_point g_nextWatchesLog{};
OllamaBridge::WorldChannel g_bridgeChannel(g_bridgeWorld,
                                           [](OllamaBridge::WorldTicket const& ticket,
                                              OllamaBridge::DispatchGuard& guard)
                                           {
                                               return g_bridgeObserver && g_bridgeObserver(ticket, guard);
        },
        OllamaBridge::Now, &OllamaChat_BridgeQuerySource(), &OllamaChat_BridgeCommandExecutor());
    std::atomic<uint64_t> g_bridgeGeneration{0};
    std::atomic<uint32_t> g_bridgeInFlight{0};

    std::atomic<uint32_t> g_inFlight{ 0 };
    std::atomic<uint64_t> g_totalSubmitted{ 0 };
    std::atomic<uint64_t> g_totalDelivered{ 0 };
    std::atomic<uint64_t> g_droppedQueueFull{ 0 };
    std::atomic<uint64_t> g_droppedEmpty{ 0 };
    std::atomic<uint64_t> g_droppedGovernor{ 0 };
    std::atomic<uint64_t> g_totalFailed{ 0 };

    std::mutex  g_errorMutex;
    std::string g_lastError;

    void RecordError(const std::string& what)
    {
        ++g_totalFailed;
        std::lock_guard<std::mutex> lock(g_errorMutex);
        g_lastError = what;
    }

    // --- worker -----------------------------------------------------------

    OllamaBridge::WorldOutcome BridgeOutcome(OllamaApiResult const& api)
    {
        using OllamaBridge::WorldOutcome;
        if (api.ok)
            return api.silent ? WorldOutcome::Silent : api.text.empty() ? WorldOutcome::Empty : WorldOutcome::Success;
        if (api.error == "bridge_expired_or_invalid_request")
            return WorldOutcome::Cancelled;
        if (api.error == "bridge_circuit_open_or_inactive")
            return WorldOutcome::CircuitOpen;
        if (api.error == "bridge_invalid_response")
            return WorldOutcome::Malformed;
        return WorldOutcome::Unavailable;
    }

void RunBridgeTask(Task& task)
{
    Completion completion;
    completion.bridgeSlot = std::move(task.bridgeSlot);
    completion.request = std::move(task.request);
    task.request = {};  // No retained payload can outlive publication under the transferred slot.
    completion.deliverAt = Clock::now();
    try
    {
        if (completion.request.bridgeGeneration != g_bridgeGeneration.load())
            return;
        auto api = QueryOllamaBridge(completion.request.bridgeSettings, completion.request.bridgeTicket.request);
        completion.bridgeOutcome = BridgeOutcome(api);
        completion.text = std::move(api.text);
        completion.personaProposal = std::move(api.personaProposal);
    }
    catch (...)
    {
        completion.bridgeOutcome = OllamaBridge::WorldOutcome::Unavailable;
    }
    // No cleanup, expression, filter, typing or reloadable config reads on this worker path.
    std::lock_guard<std::mutex> lock(g_doneMutex);
    if (completion.request.bridgeGeneration == g_bridgeGeneration.load())
        g_done.push_back(std::move(completion));
}

    void WorkerLoop()
    {
        for (;;)
        {
            bool bridge = false;
            {
                Task task;

                {
                    std::unique_lock<std::mutex> lock(g_queueMutex);
                    g_queueCv.wait(lock, [] { return !g_running || !g_queue.empty(); });

                    if (!g_running && g_queue.empty())
                        return;

                    task = std::move(g_queue.front());
                    g_queue.pop_front();
                    // Count before unlocking: retirement must not observe a false drain.
                    if (task.request.bridgeGeneration)
                        ++g_bridgeInFlight;
                }

                bridge = task.request.bridgeGeneration != 0;
                ++g_inFlight;

                try
                {
                    RunBridgeTask(task);
                }
                catch (std::exception const& e)
                {
                    RecordError(e.what());
                    LOG_ERROR("module.ollamachat", "[Ollama Chat] Worker exception: {}", e.what());
                }
                catch (...)
                {
                    RecordError("unknown exception");
                    LOG_ERROR("module.ollamachat", "[Ollama Chat] Unknown worker exception.");
                }
            }  // Destroy task payload before releasing its slot or reporting actual worker drain.
            --g_inFlight;
            if (bridge)
                --g_bridgeInFlight;  // Real worker cleanup, independent of policy cancellation.
        }
    }

    // --- delivery (world thread only) -------------------------------------

bool BridgeCurrent(Completion const& c)
{
    OllamaBridge::DispatchGuard current;
    if (!g_bridgeObserver || c.request.bridgeGeneration != g_bridgeGeneration.load() ||
        !g_bridgeObserver(c.request.bridgeTicket, current))
        return false;
    auto const now = OllamaBridge::Now();
    return c.request.bridgeGeneration == g_bridgeGeneration.load() &&
           g_bridgeWorld.Current(c.request.bridgeTicket, now) &&
           OllamaBridge::DispatchGuardCurrent(c.request.bridgeTicket, c.request.bridgeGuard, current, now);
}

bool PrepareBridge(Completion& c)
{
    using namespace OllamaBridge;
    // DS.14: a silent answer stands only when this route did something (an authorized attempt, Applied or Failed);
    // otherwise it is no answer: the outage line for an addressed human, silence otherwise.
    bool const acted = g_bridgeChannel.Close(c.request.bridgeTicket);
    if (!BridgeCurrent(c))
        return false;
    if (c.bridgeOutcome == WorldOutcome::Silent && !acted)
        c.bridgeOutcome = WorldOutcome::Empty;
    if (c.bridgeOutcome == WorldOutcome::Success)
    {
        // Config-dependent cleanup stays on the world thread, after fresh authority/perception checks. LC10: no clamp;
        // a bridge reply is never cut (LC.11), only split below.
        c.text = ProcessLlmResponse(c.text, c.request.botName, nullptr, 0);
        if (c.text.empty())
            c.bridgeOutcome = WorldOutcome::Empty;
        else
        {
            if (!BridgeCurrent(c))
                return false;
            c.text = Roleplay_FilterMetaTerms(c.text);
            if (c.text.empty())
                c.bridgeOutcome = WorldOutcome::Filtered;
            else  // Bot speech is never a playerbots command line, whoever hears it ("# Plan" -> "Plan").
            {
                PlayerbotBridge::NeutralizeCommandIndicator(c.text);
                // LC10: over one chat line, two messages split at a sentence end; a reply that needs three, or has
                // no fitting sentence end, is refused whole (a terminal no-line outcome) and counted.
                ReplySplit split = SplitReply(c.text);
                if (split.refused)
                {
                    c.bridgeOutcome = WorldOutcome::Filtered;
                    ++g_bridgeRefusedLong;
                }
                else
                {
                    c.text = std::move(split.first);
                    c.secondText = std::move(split.second);
                    PlayerbotBridge::NeutralizeCommandIndicator(c.secondText);
                }
            }
        }
    }
    auto const action = DispatchTerminal(c.bridgeOutcome, c.request.directAddress);
    bool const silent = c.bridgeOutcome == WorldOutcome::Silent;  // No line, yet completed and delivered.
    if ((action == TerminalAction::Silence && !silent) || !BridgeCurrent(c) ||
        !g_bridgeWorld.Complete(c.request.bridgeTicket, c.bridgeOutcome, Now()))
        return false;
    if (action == TerminalAction::Outage)
        c.text = DispatchOutageText();
    uint64_t delayMs = 0;
    if (g_EnableTypingSimulation && !silent)
    {
        // The whole reply is typed, both messages.
        std::size_t const typed = c.text.size() + (c.secondText.empty() ? 0 : 1 + c.secondText.size());
        delayMs = g_TypingSimulationBaseDelay + static_cast<uint64_t>(typed) * g_TypingSimulationDelayPerChar;
        if (g_TypingSimulationMaxDelay)
            delayMs = std::min(delayMs, static_cast<uint64_t>(g_TypingSimulationMaxDelay));
    }
    delayMs = std::min(delayMs, static_cast<uint64_t>(MaxLifetimeMs));
    c.deliverAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
    c.bridgePrepared = true;
    return true;
}

    Channel* ResolveChannel(Player* bot, std::string const& channelName)
    {
        if (!bot || channelName.empty())
            return nullptr;

        ChannelMgr* mgr = ChannelMgr::forTeam(bot->GetTeamId());
        if (!mgr)
            return nullptr;

        return mgr->GetChannel(channelName, bot);
    }

    bool AnyoneInRange(Player* bot, float distance)
    {
        if (!bot || !bot->IsInWorld() || distance <= 0.0f)
            return false;

        // Grid search: this runs on every Say/Yell delivery, and walking every
        // online character to answer "is anyone standing near me" is the kind
        // of thing that adds up on a bot-heavy realm.
        std::list<Player*> found;
        Acore::AnyPlayerInObjectRangeCheck check(bot, distance, false, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, found, check);
        Cell::VisitObjects(bot, searcher, distance);

        for (Player* other : found)
            if (other && other != bot && other->IsInWorld())
                return true;

        return false;
    }

    // Returns true when the line actually went out.
    bool RouteMessage(Player* bot, PlayerbotAI* botAI, const Completion& c, std::string const& text,
                      const OllamaWorldSnapshot& world, Channel*& outChannel)
    {
        outChannel = nullptr;

        switch (c.request.source)
        {
            case SRC_GENERAL_LOCAL:
            {
                Channel* channel = ResolveChannel(bot, c.request.channelName);
                if (!channel || !OllamaIsOnChannel(channel, bot->GetGUID()))
                    return false;

                // Checked before generating too; re-checked because the only
                // human in the channel can leave during the LLM round trip.
                if (!world.RealPlayerInChannel(channel))
                    return false;

                channel->Say(bot->GetGUID(), text, LANG_UNIVERSAL);
                outChannel = channel;
                return true;
            }

            // Guild, party and raid are re-checked here for the same reason
            // say and yell always were: the audience is validated at submit
            // time, and an LLM round trip is seconds long. Whoever the bot was
            // talking to can log out, leave the guild or drop group in that
            // window, and without this the bot announces to an empty channel.
            case SRC_GUILD_LOCAL:
            case SRC_OFFICER_LOCAL:
                if (g_DisableForGuild || !bot->GetGuild())
                    return false;
                if (!world.GuildHasRealPlayer(bot->GetGuildId()))
                    return false;
                return botAI->SayToGuild(text);

            case SRC_PARTY_LOCAL:
                if (g_DisableForParty || !bot->GetGroup())
                    return false;
                if (!OllamaGroupHasRealPlayer(bot))
                    return false;
                return botAI->SayToParty(text);

            case SRC_RAID_LOCAL:
                if (g_DisableForParty || !bot->GetGroup())
                    return false;
                if (!OllamaGroupHasRealPlayer(bot))
                    return false;
                return botAI->SayToRaid(text);

            case SRC_YELL_LOCAL:
                if (g_DisableForSayYell || !AnyoneInRange(bot, g_YellDistance))
                    return false;
                return botAI->Yell(text);

            case SRC_WHISPER_LOCAL:
            {
                Player* target = ObjectAccessor::FindConnectedPlayer(ObjectGuid(c.request.targetGuid));
                if (!target)
                    return false;
                return botAI->Whisper(text, target->GetName());
            }

            case SRC_SAY_LOCAL:
            default:
                if (g_DisableForSayYell || !AnyoneInRange(bot, g_SayDistance))
                    return false;
                return botAI->Say(text);
        }
    }

    // The chat line of a consumed bridge generation. Every exit returns to DeliverBridge, which holds the claim.
    void DeliverBridgeLine(Completion const& c, OllamaWorldSnapshot const& world)
    {
        if (c.bridgeOutcome == OllamaBridge::WorldOutcome::Silent)
        {
            // DS.14: the act was the answer: no line and no governor send; counted and remembered as wordless.
            ++g_bridgeSilent;
            OllamaChat_BridgeRemember(c.request.bridgeTicket, {});
            return;
        }
        // Fail closed for every text source (PrepareBridge strips generated text): a line that playerbots would parse
        // as a command is never sent, so no bot commands another bot or itself, and no send budget is spent on it.
        std::string commandLine;
        if (PlayerbotBridge::ExtractChatCommand(c.text, commandLine) ||
            PlayerbotBridge::ExtractChatCommand(c.secondText, commandLine))
            return;
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(c.request.botGuid));
        if (!bot || !bot->IsInWorld())
            return;
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!botAI || !BridgeCurrent(c))
            return;
        auto const botGuid = bot->GetGUID();
        // LC10: a reply in two messages is still one reply: one governor send, and the repetition check, the
        // utterance history and the one-exchange memory see what the bot said, both parts joined by a space.
        std::string said = c.secondText.empty() ? c.text : c.text + ' ' + c.secondText;
        if ((!c.request.directAddress && Governor_IsRepetitive(botGuid, c.request.scopeKey, said)) ||
            !Governor_TryConsumeSend(botGuid, c.request.scopeKey, c.request.directAddress))
        {
            ++g_droppedGovernor;
            return;
        }
        Channel* channel = nullptr;
        if (!BridgeCurrent(c) || !RouteMessage(bot, botAI, c, c.text, world, channel))
            return;
        if (!BridgeCurrent(c))
            return;
        if (!c.secondText.empty())
        {
            // Only after the first message went out on a route that is still current; once (DeliverBridge's claim).
            if (RouteMessage(bot, botAI, c, c.secondText, world, channel))
                ++g_bridgeSplit;
            else
                said = c.text;
        }
        Governor_RecordUtterance(botGuid, c.request.scopeKey, said);
        ++g_totalDelivered;
        if (c.bridgeOutcome == OllamaBridge::WorldOutcome::Success && BridgeCurrent(c))
            OllamaChat_BridgeRemember(c.request.bridgeTicket, said);
        // Neither outage nor success enters legacy memory, history, expression, sentiment or bot-chain paths.
    }

    // BEGIN BRIDGE CARD DELIVERY -- extracted verbatim by the offline persona bridge check.
    // DS.12: applies the provider's card of a successful generation that was offered one (no accepted card), against
    // the revision and stamp the request was built from. The handler re-captures the binding; a queued write is not
    // yet persistence.
    void ProposeBridgeCard(Completion const& c)
    {
        auto const& card = c.request.bridgeTicket.request.input.card;
        if (c.bridgeOutcome != OllamaBridge::WorldOutcome::Success || !c.personaProposal || !card.offered ||
            card.accepted)
            return;
        auto const status =
            OllamaChat_BridgeProposeCard(c.request.bridgeTicket, card.revision, card.stamp, *c.personaProposal);
        LOG_INFO("module.ollamachat", "[Ollama Chat] Bridge card for bot {}: {}", c.request.botGuid,
                 OllamaPersona::ProposalStatusName(status));
    }

    void DeliverBridge(Completion const& c, OllamaWorldSnapshot const& world)
    {
        if (!BridgeCurrent(c) || !g_bridgeWorld.Consume(c.request.bridgeTicket, OllamaBridge::Now()))
            return;
        // Keep policy authorization live through every effect, then dispose the one-shot claim on every exit.
        struct TerminalRelease
        {
            OllamaBridge::WorldTicket const& ticket;
            ~TerminalRelease() { g_bridgeWorld.Cancel(ticket); }
        } release{c.request.bridgeTicket};
        DeliverBridgeLine(c, world);
        // After the line, still under the claim: a staged write shows the card as loading, which would fail the
        // line's later captures. A delivered or dropped line does not decide the proposal, nor the proposal the line.
        ProposeBridgeCard(c);
    }
    // END BRIDGE CARD DELIVERY
}

// --------------------------------------------------------------------------

OllamaBridge::WorldPolicy& OllamaDispatch_BridgeWorld() { return g_bridgeWorld; }

OllamaBridge::ChannelError OllamaDispatch_ChannelReceive(std::string_view bearer, std::string_view route,
                                                         std::string_view body, uint64_t& receipt)
{
    return g_bridgeChannel.Receive(bearer, route, body, receipt);
}

OllamaBridge::ChannelError OllamaDispatch_ChannelTake(std::string_view bearer, uint64_t receipt,
                                                      std::unique_ptr<OllamaBridge::ChannelPacket>& packet)
{
    return g_bridgeChannel.Take(bearer, receipt, packet);
}

void OllamaDispatch_BridgeWatchLogout(uint64_t guid)
{
    // J5g: the watch table is world-thread only (no mutex). Off the world thread the edit is skipped and reported
    // once (playerbots' world-thread test, set on every world update); the 500 ms scan drops a watch whose bot or
    // partner has gone.
    if (!PlayerbotThreadCheck::IsWorldThread())
    {
        static std::atomic<bool> reported{false};
        PlayerbotThreadCheck::ReportWrongThread(reported, "OllamaChat watch logout (skipped)", __FILE__, __LINE__);
        return;
    }
    g_bridgeChannel.Watches().Cancel(guid);
}

namespace
{
// J5d transport on 127.0.0.1: its threads call only the I/O-safe Receive/Take above (C3). Defined after the
// channel, so static destruction stops it first.
OllamaBridge::ChannelListener g_bridgeListener(OllamaDispatch_ChannelReceive, OllamaDispatch_ChannelTake,
                                               [](bool warning, std::string const& line)
                                               {
                                                   if (warning)
                                                       LOG_WARN("module.ollamachat", "{}", line);
                                                   else
                                                       LOG_INFO("module.ollamachat", "{}", line);
                                               });
}  // namespace

void OllamaDispatch_BridgeListen(bool channelEnabled, uint32_t port)
{
    OllamaEndpointSettings const settings = OllamaConfig_Snapshot();
    bool const wanted = channelEnabled && OllamaBridge::BridgeUsable(g_Enable, settings.bridge);
    g_bridgeListener.Configure(wanted, port, wanted ? settings.bridge.token : std::string());
}

void OllamaDispatch_SetBridgeObserver(OllamaBridgeObserver observer)
{
    // Rebinding cannot make work captured by a previous adapter eligible.
    OllamaDispatch_BridgeConfigured(g_channelEnabled);
    g_bridgeObserver = observer;
}

static void RetireBridgeWork()
{
    g_bridgeGeneration = 0;
    // J12: lasting plans become restore-pending here (no game access); the next tick restores them, and no new
    // generation is selected until the ledger is empty.
    OllamaChat_BridgePlansRetire();
    g_bridgeChannel.Retire();
    OllamaChat_BridgeReset();
    OllamaBridge_SetActivation({});
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        std::erase_if(g_queue, [](Task const& task) { return task.request.bridgeGeneration != 0; });
    }
    {
        std::lock_guard<std::mutex> lock(g_doneMutex);
        std::erase_if(g_done, [](Completion const& item) { return item.request.bridgeGeneration != 0; });
    }
    g_bridgeWorld.Shutdown();
}

void OllamaDispatch_BridgeConfigured(bool channelEnabled)
{
    RetireBridgeWork();
    g_channelEnabled = channelEnabled;
    g_bridgeChannel.Configure(channelEnabled);
    OllamaEndpointSettings const settings = OllamaConfig_Snapshot();
    std::string epoch;
    bool const usable = OllamaBridge::BridgeUsable(g_Enable, settings.bridge);
    if (usable)
    {
        // Core cryptographic entropy; never process id/time/CLI/model-provided authority.
        auto const bytes = Acore::Crypto::GetRandomBytes<32>();
        char const hex[] = "0123456789abcdef";
        epoch.reserve(bytes.size() * 2);
        for (auto byte : bytes)
        {
            epoch.push_back(hex[byte >> 4]);
            epoch.push_back(hex[byte & 15]);
        }
    }
    g_bridgeWorld.Configured(g_Enable, settings.bridge, std::move(epoch));
    // Workers start with the first usable bridge settings (start-up or reload) and live until shutdown; an idle
    // module has none. Start is idempotent; reload never joins workers.
    if (usable)
        OllamaDispatch_Start();
}

void OllamaDispatch_Start()
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_running)
        return;

    g_running = true;
    g_bridgeObserver = OllamaChat_BridgeObserve;
    OllamaChat_BridgeInstallAbilities();

    uint32_t count = g_DispatchWorkerThreads;
    if (count == 0)
        count = 4;
    if (count > 64)
        count = 64;

    g_workers.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        g_workers.emplace_back(WorkerLoop);

    LOG_INFO("module.ollamachat", "[Ollama Chat] Dispatcher started with {} worker threads.", count);
}

void OllamaDispatch_Stop()
{
    // Revoke before the existing terminal legacy-pool join. Reload/tick never joins workers.
    g_bridgeGeneration = 0;
    g_bridgeListener.Stop();  // Bounded join of the 127.0.0.1 listener before the channel it feeds is revoked.
    g_bridgeChannel.Configure(false);
    OllamaChat_BridgeReset();
    OllamaBridge_SetActivation({});
    g_bridgeWorld.Shutdown();
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (!g_running)
            return;
        g_running = false;
        g_queue.clear();
    }

    g_queueCv.notify_all();

    for (auto& worker : g_workers)
        if (worker.joinable())
            worker.join();

    g_workers.clear();

    {
        std::lock_guard<std::mutex> lock(g_doneMutex);
        g_done.clear();
    }

    LOG_INFO("module.ollamachat", "[Ollama Chat] Dispatcher stopped.");
}

bool OllamaDispatch_SubmitBridge(OllamaChatRequest request, OllamaBridge::WorldScope const& scope,
                                 OllamaBridge::Context input, OllamaBridge::DispatchGuard guard, int64_t lifetimeMs)
{
    using namespace OllamaBridge;
    if (!g_bridgeObserver || request.botGuid != scope.botGuid || request.targetGuid != input.speakerGuid ||
        request.botName.size() > 128 || request.scopeKey.size() > 256 || request.directAddress != input.directAddress)
        return false;
    DispatchSource source = DispatchSource::Unsupported;
    switch (request.source)
    {
        case SRC_WHISPER_LOCAL:
            source = DispatchSource::Whisper;
            break;
        case SRC_SAY_LOCAL:
            source = DispatchSource::Say;
            break;
        case SRC_YELL_LOCAL:
            source = DispatchSource::Yell;
            break;
        case SRC_PARTY_LOCAL:
            source = DispatchSource::Party;
            break;
        default:
            return false;
    }
    if (guard.source != source || ((source == DispatchSource::Say || source == DispatchSource::Yell) &&
                                   !input.directAddress && input.wakeCondition.empty()))
        return false;
    auto settings = OllamaConfig_Snapshot();
    Identity const identity{settings.bridge.instance, settings.bridge.provider, settings.bridge.endpoint, scope.epoch};
    if (settings.bridge.token != g_bridgeWorld.Snapshot().token || !(identity == g_bridgeWorld.Selected()))
        return false;
    auto slot = DispatchSlot::Acquire();
    if (!slot)
        return false;  // No capacity to stage even a queue-full fallback.
    auto ticket = g_bridgeWorld.Reserve(scope, std::move(input), Now(), lifetimeMs);
    if (!ticket.generation)
        return false;
    request.bridgeGeneration = ticket.generation;
    request.bridgeSettings = std::move(settings);
    request.bridgeTicket = ticket;
    request.bridgeGuard = std::move(guard);
    request.channelName.clear();
    request.channelId = 0;
    Completion pending;
    pending.request = std::move(request);
    pending.bridgeSlot = std::move(slot);
    if (!BridgeCurrent(pending))
    {
        g_bridgeWorld.Cancel(ticket);
        return false;
    }
    if (g_channelEnabled && !g_bridgeChannel.Register(ticket, pending.request.bridgeGuard))
    {
        g_bridgeWorld.Cancel(ticket);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (!g_running)
        {
            g_bridgeChannel.Close(ticket);
            g_bridgeWorld.Cancel(ticket);
            return false;
        }
        if (g_MaxQueueDepth > 0 && g_queue.size() >= g_MaxQueueDepth)
        {
            ++g_droppedQueueFull;
            pending.bridgeOutcome = WorldOutcome::QueueFull;
            pending.deliverAt = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> doneLock(g_doneMutex);
            g_done.push_back(std::move(pending));
            return true;  // Accepted one terminal outage, not another provider attempt.
        }
        if (!g_bridgeWorld.MarkQueued(ticket, Now()))
        {
            g_bridgeChannel.Close(ticket);
            g_bridgeWorld.Cancel(ticket);
            return false;
        }
        Task task;
        task.request = std::move(pending.request);
        task.bridgeSlot = std::move(pending.bridgeSlot);
        g_queue.push_back(std::move(task));
    }
    ++g_totalSubmitted;
    g_queueCv.notify_one();
    return true;
}

// J12: generation intents and one-shot effects are synchronous; lasting plans are the only other obligation, so the
// retirement is proven only while no plan is live or restore-pending. Unknown obligations fail closed.
static OllamaBridge::RetirementProof BridgeRetirementProof()
{
    return OllamaChat_BridgePlansEmpty() ? OllamaBridge::RetirementProof::NoLiveCaptures
                                         : OllamaBridge::RetirementProof::Unknown;
}

static void ActivateBridge()
{
    OllamaBridge_SetActivation(g_bridgeWorld.Selected());
    g_bridgeGeneration = g_bridgeWorld.Generation();
    OllamaChat_BridgePlansOpen(g_bridgeWorld.Generation());
}

namespace
{
// Smoke-test watchlist: typed command counters as fixed fields, after a change and at most once a minute.
void LogBridgeCommands(Clock::time_point now)
{
    OllamaBridge::CommandStats const& stats = g_bridgeChannel.Commands();
    if (now < g_nextCommandsLog || (stats == g_loggedCommands && g_bridgeSilent == g_loggedSilent &&
                                    g_bridgeSplit == g_loggedSplit && g_bridgeRefusedLong == g_loggedRefusedLong))
        return;
    g_loggedCommands = stats;
    g_loggedSilent = g_bridgeSilent;
    g_loggedSplit = g_bridgeSplit;
    g_loggedRefusedLong = g_bridgeRefusedLong;
    g_nextCommandsLog = now + std::chrono::minutes(1);
    LOG_INFO("module.ollamachat",
             "[Ollama Chat] Bridge commands: proposed {} duplicates {} refused {} attempts {} applied {} "
             "failed {} withheld {} silent {} split {} toolong {}",
             stats.proposed, stats.duplicates, stats.refused, stats.attempts, stats.applied, stats.failed,
             stats.withheld, g_bridgeSilent, g_bridgeSplit, g_bridgeRefusedLong);
}

// Smoke-test watchlist (J12): lasting-plan counters, after a change and at most once a minute.
void LogBridgePlans(Clock::time_point now)
{
    OllamaBridge::PlanCounters const plans = OllamaChat_BridgePlanCounters();
    if (now < g_nextPlansLog || plans == g_loggedPlans)
        return;
    g_loggedPlans = plans;
    g_nextPlansLog = now + std::chrono::minutes(1);
    LOG_INFO("module.ollamachat",
             "[Ollama Chat] Bridge plans: applied {} renewed {} expired {} regrouped {} retired {} restored {} "
             "departed {} abandoned {} unrecorded {}",
             plans.applied, plans.renewed, plans.expired, plans.regrouped, plans.retired, plans.restored,
             plans.departed, plans.abandoned, plans.unrecorded);
}

// Smoke-test watchlist (J6e-c): watch counters beside the plan counters, after a change and at most once a minute.
void LogBridgeWatches(Clock::time_point now)
{
    OllamaBridge::WatchCounters const watches = g_bridgeChannel.Watches().Counters();
    if (now < g_nextWatchesLog || watches == g_loggedWatches)
        return;
    g_loggedWatches = watches;
    g_nextWatchesLog = now + std::chrono::minutes(1);
    LOG_INFO("module.ollamachat", "[Ollama Chat] Bridge watches: armed {} fired {} expired {} dropped {} rejected {}",
             watches.armed, watches.fired, watches.expired, watches.dropped, watches.rejected);
}
}  // namespace

// DS.14 (c), J6e-c: one watch scan. Each watch costs one fresh read of its own bot; a fired watch opens at most one
// wake, which gets no authority of its own (BridgeSubmit, then the full command path on its route).
static void ScanBridgeWatches()
{
    auto& watches = g_bridgeChannel.Watches();
    std::vector<OllamaBridge::Watch> due;
    watches.Scan(
        OllamaBridge::Now(), [](OllamaBridge::Watch const& watch, OllamaBridge::WatchFacts& facts)
        { return OllamaChat_BridgeQuerySource().ReadWatch(watch.botGuid, watch.partnerGuid, facts); }, due);
    for (auto const& watch : due)
    {
        bool const opened = OllamaChat_BridgeWake(watch);
        if (!opened)
            LOG_DEBUG("module.ollamachat", "[Ollama Chat] Bridge wake dropped: bot {} watch {}", watch.botGuid,
                      watch.id);
        watches.Woke(watch, opened, OllamaBridge::Now());
    }
}

// The bridge lifecycle part of the world tick, in order: restores of pending lasting plans first (J12), so a
// retirement made by a reload or the previous call can be proven drained in this one; then the selection, which
// waits for an empty plan ledger. With the module disabled only the restores and expiry run: the reload that
// disabled it retired every plan, and nothing else restores them.
static void UpdateBridgeLifecycle(OllamaBridge::EffectPhase const& phase)
{
    OllamaChat_BridgePlansUpdate(phase);
    if (!g_Enable)
        return;
    g_bridgeWorld.Expire(OllamaBridge::Now());
    OllamaChat_BridgeRefresh();
    if (g_bridgeWorld.ObserveDrain(
            g_bridgeInFlight.load() == 0 && OllamaBridge::DispatchSlot::Used() == 0 && g_bridgeChannel.Drained(),
            BridgeRetirementProof()))
        ActivateBridge();
}

void OllamaDispatch_Update(uint32_t /*diff*/, PlayerbotBridge::PostMapJoin const& join)
{
    // Typed effects and plan restores get the post-map-join phase for this call only.
    OllamaBridge::EffectPhase const phase(join);
    UpdateBridgeLifecycle(phase);
    // J5e: the plan restores above run while disabled too, so their counters line stays visible.
    LogBridgePlans(Clock::now());
    LogBridgeWatches(Clock::now());
    if (!g_Enable)
        return;  // Disabled: the plan restores above are the only work (OnUpdate calls this tick in that state too).
    // Bounded independent reverse lane; no HTTP, waits or generation-worker dependency.
    g_bridgeChannel.Update(&phase);
    // DS.14 (c), J6e-c: watches, post-map-join, at most every 500 ms and nothing at all while none exists.
    if (g_bridgeChannel.Watches().Due(OllamaBridge::Now()))
        ScanBridgeWatches();
    const auto now = Clock::now();
    LogBridgeCommands(now);

    // Move due completions out under the lock, then deliver without it: a completion that is not due yet goes
    // back under the lock.
    std::vector<Completion> due;

    {
        std::lock_guard<std::mutex> lock(g_doneMutex);

        for (auto it = g_done.begin(); it != g_done.end(); )
        {
            if (it->deliverAt <= now)
            {
                due.push_back(std::move(*it));
                it = g_done.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    if (due.empty())
        return;

    // One pass over the online player list for the whole tick, rather than one
    // per delivery.
    OllamaWorldSnapshot world;
    world.Build();

    for (Completion& c : due)
    {
        if (c.request.bridgeGeneration != g_bridgeGeneration.load())
            continue;
        try
        {
            if (!c.bridgePrepared && !PrepareBridge(c))
            {
                g_bridgeWorld.Cancel(c.request.bridgeTicket);
                continue;
            }
            if (!BridgeCurrent(c))
            {
                g_bridgeWorld.Cancel(c.request.bridgeTicket);
                continue;
            }
            if (c.deliverAt > Clock::now())
            {
                std::lock_guard<std::mutex> lock(g_doneMutex);
                g_done.push_back(std::move(c));
                continue;
            }
            DeliverBridge(c, world);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("module.ollamachat", "[Ollama Chat] Delivery exception: {}", e.what());
        }
    }
}

OllamaDispatchStats OllamaDispatch_GetStats()
{
    OllamaDispatchStats stats{};

    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        stats.queuedRequests = static_cast<uint32_t>(g_queue.size());
        stats.workers        = static_cast<uint32_t>(g_workers.size());
    }
    {
        std::lock_guard<std::mutex> lock(g_doneMutex);
        stats.pendingDeliveries = static_cast<uint32_t>(g_done.size());
    }
    {
        std::lock_guard<std::mutex> lock(g_errorMutex);
        stats.lastError = g_lastError;
    }

    stats.inFlight              = g_inFlight.load();
    stats.totalSubmitted        = g_totalSubmitted.load();
    stats.totalDelivered        = g_totalDelivered.load();
    stats.totalDroppedQueueFull = g_droppedQueueFull.load();
    stats.totalDroppedEmpty     = g_droppedEmpty.load();
    stats.totalDroppedGovernor  = g_droppedGovernor.load();
    stats.totalFailed           = g_totalFailed.load();

    return stats;
}

// Explicit template instantiations are exempt from access checking
// ([temp.spec.general]), so instantiating this with &Channel::IsOn is legal
// and hands the member pointer out through a friend function. If the core
// ever changes IsOn's signature, this stops compiling rather than misbehaving.
namespace
{
    using ChannelIsOnFn = bool (Channel::*)(ObjectGuid) const;

    ChannelIsOnFn ChannelIsOnPtr();

    template <ChannelIsOnFn Fn>
    struct ChannelIsOnAccess
    {
        friend ChannelIsOnFn ChannelIsOnPtr() { return Fn; }
    };

    template struct ChannelIsOnAccess<&Channel::IsOn>;
}

bool OllamaIsOnChannel(Channel const* channel, ObjectGuid guid)
{
    return channel && (channel->*ChannelIsOnPtr())(guid);
}

Channel* OllamaResolveZoneChannel(Player* bot, uint32_t chatChannelId)
{
    if (!bot)
        return nullptr;

    ChannelMgr* mgr = ChannelMgr::forTeam(bot->GetTeamId());
    if (!mgr)
        return nullptr;

    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    std::string zoneName;
    if (botAI)
        if (AreaTableEntry const* zone = botAI->GetCurrentZone())
            zoneName = PlayerbotAI::GetLocalizedAreaName(zone);

    for (auto const& [key, channel] : mgr->GetChannels())
    {
        if (!channel || channel->GetName().empty())
            continue;
        if (channel->GetChannelId() != chatChannelId)
            continue;

        // Global channels (LFG, WorldDefense) are not zone-scoped.
        const bool zoneScoped = (chatChannelId != uint32_t(ChatChannelId::LOOKING_FOR_GROUP) &&
                                 chatChannelId != uint32_t(ChatChannelId::WORLD_DEFENSE));

        if (zoneScoped)
        {
            if (zoneName.empty())
                continue;
            if (channel->GetName().find(zoneName) == std::string::npos)
                continue;
        }

        return channel;
    }

    return nullptr;
}
