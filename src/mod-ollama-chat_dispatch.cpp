#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_bridge_listener.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_memory.h"
#include "mod-ollama-chat_persona_bridge.h"
#include "mod-ollama-chat_response.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_sentiment.h"
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

    enum class TaskType : uint8_t { ChatReply, Sentiment, Condense, Relationship };

    struct Task
    {
        OllamaBridge::DispatchSlot bridgeSlot;
        TaskType type = TaskType::ChatReply;
        OllamaChatRequest request;

        // Sentiment-only payload.
        uint64_t    sentimentBotGuid    = 0;
        uint64_t    sentimentPlayerGuid = 0;
        std::string sentimentMessage;
        std::string sentimentPrompt;

        // Memory / relationship payload.
        uint64_t    memoryBotGuid   = 0;
        uint64_t    memoryOtherGuid = 0;
        std::string memoryOtherName;
        std::string memoryPrompt;
    };

    struct Completion
    {
        OllamaBridge::DispatchSlot bridgeSlot;
        OllamaBridge::WorldOutcome bridgeOutcome = OllamaBridge::WorldOutcome::Cancelled;
        bool bridgePrepared = false;
        OllamaChatRequest request;
        std::string       text;
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
OllamaBridge::WorldObserver g_nativeObserver(OllamaHttp_PostObserver);
OllamaBridge::WorldPolicy g_bridgeWorld(&g_nativeObserver);
bool g_nativeSelected = false;  // World-thread cached selection, including fail-closed invalid settings.
OllamaBridge::Settings g_nativeSettings;
OllamaBridgeObserver g_bridgeObserver = nullptr;
bool g_channelEnabled = false;
OllamaBridge::CommandStats g_loggedCommands;  // World-thread: the counters in the last watchlist line.
uint64_t g_bridgeSilent = 0;  // World-thread: delivered action-only answers (DS.14), in the commands line.
uint64_t g_loggedSilent = 0;  // World-thread: the silent count in the last watchlist line.
Clock::time_point g_nextCommandsLog{};
OllamaBridge::PlanCounters g_loggedPlans;  // World-thread: the plan counters in the last watchlist line.
Clock::time_point g_nextPlansLog{};
OllamaBridge::WatchCounters g_loggedWatches;  // World-thread: the watch counters in the last watchlist line (J6e-c).
Clock::time_point g_nextWatchesLog{};
OllamaBridge::WorldChannel g_bridgeChannel(g_bridgeWorld,
                                           [](OllamaBridge::WorldTicket const& ticket,
                                              OllamaBridge::DispatchGuard& guard)
                                           {
                                               return g_bridgeObserver && g_bridgeObserver(ticket, guard) &&
                                                      (!g_nativeSelected ||
                                                       g_nativeObserver.Current(ticket.scope, OllamaBridge::Now()));
        },
        OllamaBridge::Now, &OllamaChat_BridgeQuerySource(), &OllamaChat_BridgeCommandExecutor());
    std::deque<OllamaBridge::EventPublication> g_eventPublications;  // WORLD ONLY, bounded by MaxPackets.
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
        auto const& settings = completion.request.bridgeSettings.bridge;
        if (completion.request.bridgeGeneration != g_bridgeGeneration.load() ||
            (settings.provider == "codex" && settings.observerEnabled &&
             !g_nativeObserver.Current(completion.request.bridgeTicket.scope, OllamaBridge::Now())))
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

    void RunChatTask(Task& task)
    {
        if (task.request.bridgeGeneration)
        {
            RunBridgeTask(task);
            return;
        }
        OllamaApiResult api = QueryOllama(task.request.prompt, task.request.kind);

        if (!api.ok)
        {
            RecordError(api.error);
            return;
        }

        uint32_t emoteId = 0;
        std::string text = ProcessLlmResponse(api.text, task.request.botName, &emoteId);

        // Roleplay mode rejects lines carrying out-of-world vocabulary rather
        // than mangling the sentence around the offending word.
        if (!text.empty())
        {
            std::string filtered = Roleplay_FilterMetaTerms(text);
            if (filtered.empty() && !text.empty() && g_DebugEnabled)
            {
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Bot {} reply rejected by roleplay filter: '{}'",
                         task.request.botName, text);
            }
            text = std::move(filtered);
        }

        if (text.empty())
        {
            ++g_droppedEmpty;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Bot {} produced nothing usable after cleanup.",
                         task.request.botName);
            return;
        }

        Completion completion;
        completion.request = task.request;
        completion.text    = std::move(text);
        completion.emoteId = emoteId;

        uint32_t delayMs = 0;
        if (g_EnableTypingSimulation)
        {
            delayMs = g_TypingSimulationBaseDelay +
                      static_cast<uint32_t>(completion.text.length()) * g_TypingSimulationDelayPerChar;
            if (g_TypingSimulationMaxDelay > 0 && delayMs > g_TypingSimulationMaxDelay)
                delayMs = g_TypingSimulationMaxDelay;
        }

        // A delay, not a sleep. The old code held a whole thread hostage here.
        completion.deliverAt = Clock::now() + std::chrono::milliseconds(delayMs);

        std::lock_guard<std::mutex> lock(g_doneMutex);
        if (completion.request.bridgeGeneration && completion.request.bridgeGeneration != g_bridgeGeneration.load())
            return;
        g_done.push_back(std::move(completion));
    }

    void RunSentimentTask(const Task& task)
    {
        // Sentiment touches only mutex-guarded in-memory state and async DB
        // writes, so it completes here rather than round-tripping to the
        // world thread.
        ApplySentimentAnalysis(task.sentimentBotGuid, task.sentimentPlayerGuid,
                               task.sentimentMessage, task.sentimentPrompt);
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
                    switch (task.type)
                    {
                        case TaskType::Sentiment:
                            RunSentimentTask(task);
                            break;
                        case TaskType::Condense:
                            Memory_RunCondensation(task.memoryBotGuid, task.memoryPrompt);
                            break;
                        case TaskType::Relationship:
                            Memory_RunRelationshipUpdate(task.memoryBotGuid, task.memoryOtherGuid, task.memoryOtherName,
                                                         task.memoryPrompt);
                            break;
                        default:
                            RunChatTask(task);
                            break;
                    }
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
           (!(c.request.bridgeSettings.bridge.provider == "codex" && c.request.bridgeSettings.bridge.observerEnabled) ||
            g_nativeObserver.Current(c.request.bridgeTicket.scope, now)) &&
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
        // Config-dependent cleanup stays on the world thread, after fresh authority/perception checks.
        c.text = ProcessLlmResponse(c.text, c.request.botName, nullptr);
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
                PlayerbotBridge::NeutralizeCommandIndicator(c.text);
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
        delayMs = g_TypingSimulationBaseDelay + static_cast<uint64_t>(c.text.size()) * g_TypingSimulationDelayPerChar;
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
    bool RouteMessage(Player* bot, PlayerbotAI* botAI, const Completion& c,
                      const OllamaWorldSnapshot& world,
                      Channel*& outChannel)
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

                channel->Say(bot->GetGUID(), c.text, LANG_UNIVERSAL);
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
                return botAI->SayToGuild(c.text);

            case SRC_PARTY_LOCAL:
                if (g_DisableForParty || !bot->GetGroup())
                    return false;
                if (!OllamaGroupHasRealPlayer(bot))
                    return false;
                return botAI->SayToParty(c.text);

            case SRC_RAID_LOCAL:
                if (g_DisableForParty || !bot->GetGroup())
                    return false;
                if (!OllamaGroupHasRealPlayer(bot))
                    return false;
                return botAI->SayToRaid(c.text);

            case SRC_YELL_LOCAL:
                if (g_DisableForSayYell || !AnyoneInRange(bot, g_YellDistance))
                    return false;
                return botAI->Yell(c.text);

            case SRC_WHISPER_LOCAL:
            {
                Player* target = ObjectAccessor::FindConnectedPlayer(ObjectGuid(c.request.targetGuid));
                if (!target)
                    return false;
                return botAI->Whisper(c.text, target->GetName());
            }

            case SRC_SAY_LOCAL:
            default:
                if (g_DisableForSayYell || !AnyoneInRange(bot, g_SayDistance))
                    return false;
                return botAI->Say(c.text);
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
        if (PlayerbotBridge::ExtractChatCommand(c.text, commandLine))
            return;
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(c.request.botGuid));
        if (!bot || !bot->IsInWorld())
            return;
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!botAI || !BridgeCurrent(c))
            return;
        auto const botGuid = bot->GetGUID();
        if ((!c.request.directAddress && Governor_IsRepetitive(botGuid, c.request.scopeKey, c.text)) ||
            !Governor_TryConsumeSend(botGuid, c.request.scopeKey, c.request.directAddress))
        {
            ++g_droppedGovernor;
            return;
        }
        Channel* channel = nullptr;
        if (!BridgeCurrent(c) || !RouteMessage(bot, botAI, c, world, channel))
            return;
        if (!BridgeCurrent(c))
            return;
        Governor_RecordUtterance(botGuid, c.request.scopeKey, c.text);
        ++g_totalDelivered;
        if (c.bridgeOutcome == OllamaBridge::WorldOutcome::Success && BridgeCurrent(c))
            OllamaChat_BridgeRemember(c.request.bridgeTicket, c.text);
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

    void Deliver(Completion const& c, OllamaWorldSnapshot const& world)
    {
        if (c.request.bridgeGeneration)
        {
            DeliverBridge(c, world);
            return;
        }
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(c.request.botGuid));
        if (!bot || !bot->IsInWorld())
            return;

        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!botAI)
            return;

        const ObjectGuid botGuid = bot->GetGUID();

        // Direct address: the bot owes this person an answer, so it skips the
        // suppressions that exist to pace ambient chatter. Decided at submit
        // time in ProcessChat, where the Group was live to consult.
        const bool directAddress = c.request.directAddress;

        // Repetition is checked at delivery rather than at submission, because
        // we only know what the model actually said now.
        //
        // Skipped for direct address. Ask a bot the same question twice and
        // the same answer is correct -- suppressing it leaves the asker
        // staring at silence, which reads as the bot being broken rather than
        // as anti-repetition working.
        if (!directAddress && Governor_IsRepetitive(botGuid, c.request.scopeKey, c.text))
        {
            ++g_droppedGovernor;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Bot {} reply suppressed as repetitive: '{}'",
                         bot->GetName(), c.text);
            return;
        }

        if (!Governor_TryConsumeSend(botGuid, c.request.scopeKey, directAddress))
        {
            ++g_droppedGovernor;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Bot {} reply suppressed by cooldown/rate limit.",
                         bot->GetName());
            return;
        }

        Channel* channel = nullptr;
        if (!RouteMessage(bot, botAI, c, world, channel))
        {
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Bot {} had nowhere to send its reply ({}).",
                         bot->GetName(), ChatChannelSourceLocalStr[c.request.source]);
            return;
        }

        Governor_RecordUtterance(botGuid, c.request.scopeKey, c.text);
        ++g_totalDelivered;

        // This bot is now in a conversation with whoever it just answered, so
        // their next line in this scope is a turn in it rather than ambient
        // chatter. Only against a real person: an open conversation bypasses
        // pacing, and letting two bots open one with each other is how a
        // bot-to-bot loop would escape every brake in the governor.
        if (c.request.targetGuid)
        {
            Player* addressee = ObjectAccessor::FindConnectedPlayer(ObjectGuid(c.request.targetGuid));
            if (OllamaIsRealPlayer(addressee))
                Governor_NoteConversation(botGuid, ObjectGuid(c.request.targetGuid),
                                          c.request.scopeKey);
        }

        // Body language. Safe here and only here: this is the world thread.
        ScheduleBotExpression(bot, ObjectGuid(c.request.targetGuid), c.emoteId,
                              g_BotExpressionDelayMs);

        if (c.request.recordHistory && c.request.targetGuid)
        {
            AppendBotConversation(c.request.botGuid, c.request.targetGuid,
                                  c.request.originMessage, c.text);

            // Counts name mentions and, past a threshold, queues a
            // condensation or relationship revision. World thread.
            Player* target = ObjectAccessor::FindConnectedPlayer(ObjectGuid(c.request.targetGuid));
            Memory_NoteExchange(c.request.botGuid, c.request.targetGuid,
                                target ? target->GetName() : std::string(),
                                c.request.originMessage, c.text);
        }

        if (c.request.updateSentiment && c.request.targetGuid &&
            !c.request.originMessage.empty())
        {
            OllamaDispatch_SubmitSentiment(c.request.botGuid, c.request.targetGuid,
                                           c.request.originMessage);
        }

        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] {} ({}, depth {}): {}",
                     bot->GetName(), ChatChannelSourceLocalStr[c.request.source],
                     c.request.chainDepth, c.text);

        // Let other bots hear it -- with the chain depth advanced, which is
        // what stops the reply loop that had no brakes before.
        if (c.request.triggerBotReplies &&
            c.request.source != SRC_WHISPER_LOCAL)
        {
            ProcessBotChatMessage(bot, c.text, c.request.source, channel,
                                  static_cast<uint8_t>(c.request.chainDepth + 1));
        }
    }
}

// --------------------------------------------------------------------------

OllamaBridge::WorldPolicy& OllamaDispatch_BridgeWorld() { return g_bridgeWorld; }

bool OllamaDispatch_ChannelEvents(OllamaBridge::DispatchGuard const& guard, std::vector<uint8_t> const& icons)
{
    // J6e-c: only the native (Codex observer) adapter takes event routes. The generic 127.0.0.1 port has no events
    // endpoint, so an event route there held a packet slot until retirement (64 mark changes in an epoch refused every
    // later query and command with capacity) and a world reservation of the binding's scope for nothing.
    if (!g_nativeSelected || g_eventPublications.size() >= OllamaBridge::WorldChannel::MaxPackets)
        return false;
    OllamaBridge::EventPublication publication;
    if (!g_bridgeChannel.ObserveEvents(guard, icons, publication))
        return false;
    g_eventPublications.push_back(std::move(publication));
    return true;
}

bool OllamaDispatch_ChannelNextEvent(OllamaBridge::EventPublication& publication)
{
    publication = {};
    if (g_eventPublications.empty())
        return false;
    publication = std::move(g_eventPublications.front());
    g_eventPublications.pop_front();
    return true;
}

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

void OllamaDispatch_BridgeWatchLogout(uint64_t guid) { g_bridgeChannel.Watches().Cancel(guid); }

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
    bool const wanted = channelEnabled && g_Enable && settings.provider == OllamaProvider::Bridge &&
                        OllamaBridge::ValidSettings(settings.bridge);
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
    g_eventPublications.clear();
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
    g_nativeSelected = g_Enable && settings.provider == OllamaProvider::Bridge && settings.bridge.provider == "codex" &&
                       settings.bridge.observerEnabled;
    g_nativeSettings = g_nativeSelected ? settings.bridge : OllamaBridge::Settings{};
    g_nativeObserver.Configured(g_nativeSelected, g_nativeSettings, OllamaBridge::Now());
    if (g_nativeSelected)
    {
        g_nativeObserver.Start();
        return;  // Tick waits for actual retired workers and authenticated open before creating an epoch.
    }
    std::string epoch;
    if (g_Enable && settings.provider == OllamaProvider::Bridge && OllamaBridge::ValidSettings(settings.bridge))
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
    g_bridgeWorld.Configured(g_Enable && settings.provider == OllamaProvider::Bridge, settings.bridge,
                             std::move(epoch));
}

void OllamaDispatch_Start()
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_running)
        return;

    g_running = true;
    if (g_nativeSelected)
        g_nativeObserver.Start();
    g_bridgeObserver = OllamaChat_BridgeObserve;

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
    g_nativeSelected = false;
    g_nativeSettings = {};
    g_nativeObserver.Stop();  // Terminal shutdown only, bounded observer HTTP; no tick/reload joins.
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

bool OllamaDispatch_Submit(OllamaChatRequest request)
{
    if (request.bridgeGeneration || OllamaConfig_Snapshot().provider == OllamaProvider::Bridge)
        return false;
    if (request.prompt.empty() || request.botGuid == 0)
        return false;

    Task task;
    task.type    = TaskType::ChatReply;
    task.request = std::move(request);

    {
        std::lock_guard<std::mutex> lock(g_queueMutex);

        if (!g_running)
            return false;

        if (task.request.bridgeGeneration && task.request.bridgeGeneration != g_bridgeGeneration.load())
            return false;

        if (g_MaxQueueDepth > 0 && g_queue.size() >= g_MaxQueueDepth)
        {
            ++g_droppedQueueFull;
            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat",
                         "[Ollama Chat] Queue full ({}); dropping request for {}.",
                         g_queue.size(), task.request.botName);
            return false;
        }

        g_queue.push_back(std::move(task));
    }

    ++g_totalSubmitted;
    g_queueCv.notify_one();
    return true;
}

bool OllamaDispatch_SubmitBridge(OllamaChatRequest request, OllamaBridge::WorldScope const& scope,
                                 OllamaBridge::Context input, OllamaBridge::DispatchGuard guard, int64_t lifetimeMs)
{
    using namespace OllamaBridge;
    if (!g_bridgeObserver || request.chainDepth || request.kind != OllamaRequestKind::ChatReply ||
        request.botGuid != scope.botGuid || request.targetGuid != input.speakerGuid || request.botName.size() > 128 ||
        request.scopeKey.size() > 256 || request.directAddress != input.directAddress)
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
    if (settings.provider != OllamaProvider::Bridge || settings.bridge.token != g_bridgeWorld.Snapshot().token ||
        !(identity == g_bridgeWorld.Selected()) || (g_nativeSelected && !g_nativeObserver.Current(scope, Now())))
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
    request.prompt.clear();
    request.originMessage.clear();
    request.channelName.clear();
    request.channelId = 0;
    request.recordHistory = false;
    request.updateSentiment = false;
    request.triggerBotReplies = false;
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

void OllamaDispatch_SubmitSentiment(uint64_t botGuid, uint64_t playerGuid,
                                    const std::string& message)
{
    if (!g_EnableSentimentTracking || message.empty())
        return;

    // Built here, on the world thread: the template is config state that
    // reload rewrites, so a worker must never read it.
    std::string prompt = BuildSentimentPrompt(message);
    if (prompt.empty())
        return;

    Task task;
    task.type                = TaskType::Sentiment;
    task.sentimentBotGuid    = botGuid;
    task.sentimentPlayerGuid = playerGuid;
    task.sentimentMessage    = message;
    task.sentimentPrompt     = std::move(prompt);

    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (!g_running)
            return;

        // Sentiment is best-effort; never let it crowd out actual chat.
        if (g_MaxQueueDepth > 0 && g_queue.size() >= g_MaxQueueDepth / 2)
            return;

        g_queue.push_back(std::move(task));
    }

    g_queueCv.notify_one();
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

static void UpdateNativeObserver()
{
    using namespace OllamaBridge;
    if (!g_nativeSelected)
        return;
    auto const now = Now();
    g_nativeObserver.Poll(now);
    auto state = g_nativeObserver.State();
    if (state.epoch.empty() && !g_bridgeWorld.Selected().epoch.empty())
        RetireBridgeWork();
    // Count physical queued/completed slots as well as workers, never cancelled reservations, and wait for the
    // restores of retired lasting plans (J12) instead of retiring again.
    bool const drained = g_bridgeInFlight.load() == 0 && DispatchSlot::Used() == 0 && g_bridgeChannel.Drained() &&
                         OllamaChat_BridgePlansEmpty();
    if (state.enabled && state.epoch.empty())
    {
        if (state.incarnation.empty())
            g_nativeObserver.Open(drained, now);  // Native open independently checks its own actual work drain.
        else if (drained)
        {
            auto const bytes = Acore::Crypto::GetRandomBytes<16>();
            char const hex[] = "0123456789abcdef";
            std::string epoch = state.incarnation + ".";
            for (auto byte : bytes)
            {
                epoch.push_back(hex[byte >> 4]);
                epoch.push_back(hex[byte & 15]);
            }
            // Only this world-thread owner supplies entropy and selection after actual retirement.
            g_bridgeWorld.Configured(true, g_nativeSettings, epoch);
            if (g_bridgeWorld.ObserveDrain(drained, BridgeRetirementProof()) &&
                g_nativeObserver.BindEpoch(std::move(epoch), drained, now))
                ActivateBridge();
            else
                RetireBridgeWork();
        }
    }
}

namespace
{
// Smoke-test watchlist: typed command counters as fixed fields, after a change and at most once a minute.
void LogBridgeCommands(Clock::time_point now)
{
    OllamaBridge::CommandStats const& stats = g_bridgeChannel.Commands();
    if (now < g_nextCommandsLog || (stats == g_loggedCommands && g_bridgeSilent == g_loggedSilent))
        return;
    g_loggedCommands = stats;
    g_loggedSilent = g_bridgeSilent;
    g_nextCommandsLog = now + std::chrono::minutes(1);
    LOG_INFO("module.ollamachat",
             "[Ollama Chat] Bridge commands: proposed {} duplicates {} refused {} attempts {} applied {} "
             "failed {} withheld {} silent {}",
             stats.proposed, stats.duplicates, stats.refused, stats.attempts, stats.applied, stats.failed,
             stats.withheld, g_bridgeSilent);
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
// retirement made by a reload or the previous call can be proven drained in this one; then native and generic
// selection, which wait for an empty plan ledger. With the module disabled only the restores and expiry run: the
// reload that disabled it retired every plan, and nothing else restores them.
static void UpdateBridgeLifecycle(OllamaBridge::EffectPhase const& phase)
{
    OllamaChat_BridgePlansUpdate(phase);
    if (!g_Enable)
        return;
    UpdateNativeObserver();
    g_bridgeWorld.Expire(OllamaBridge::Now());
    OllamaChat_BridgeRefresh();
    if (g_nativeSelected)
        g_nativeObserver.Publish(
            g_bridgeWorld.Scopes(OllamaBridge::Now()),
            g_bridgeInFlight.load() == 0 && OllamaBridge::DispatchSlot::Used() == 0 && g_bridgeChannel.Drained(),
            OllamaBridge::Now());
    if (!g_nativeSelected &&
        g_bridgeWorld.ObserveDrain(
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

    // Move due completions out under the lock, then deliver without it: the
    // delivery path re-enters ProcessBotChatMessage, which submits new work.
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
        if (c.request.bridgeGeneration && c.request.bridgeGeneration != g_bridgeGeneration.load())
            continue;
        try
        {
            if (c.request.bridgeGeneration)
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
            }
            Deliver(c, world);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("module.ollamachat", "[Ollama Chat] Delivery exception: {}", e.what());
        }
    }
}

void OllamaChat_DispatchEmoteReaction(Player* bot, Player* player, uint32_t textEmote)
{
    if (!bot || !player)
        return;

    OllamaChatRequest request;
    request.botGuid    = bot->GetGUID().GetRawValue();
    request.targetGuid = player->GetGUID().GetRawValue();
    request.source     = SRC_SAY_LOCAL;
    request.chainDepth = 0;
    request.botName    = bot->GetName();
    request.kind       = OllamaRequestKind::EventChatter;
    request.scopeKey   = Governor_MakeScopeKey("Say", 0, "", 0, bot->GetZoneId());
    request.triggerBotReplies = false;
    // Someone emoted at this bot by name. Its own debounce paces this;
    // the ambient say cooldown has no business also silencing it.
    request.directAddress = true;

    request.prompt = BuildEmoteReactionPrompt(bot, player, textEmote);
    if (request.prompt.empty())
        return;

    OllamaDispatch_Submit(std::move(request));
}

namespace
{
    // Background upkeep must never crowd out actual chat, so it gets a
    // stricter queue allowance than a reply does.
    bool SubmitBackground(Task&& task)
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (!g_running)
            return false;
        if (g_MaxQueueDepth > 0 && g_queue.size() >= g_MaxQueueDepth / 2)
            return false;

        g_queue.push_back(std::move(task));
        return true;
    }
}

void OllamaDispatch_SubmitCondensation(uint64_t botGuid, const std::string& prompt)
{
    if (botGuid == 0 || prompt.empty())
        return;

    Task task;
    task.type          = TaskType::Condense;
    task.memoryBotGuid = botGuid;
    task.memoryPrompt  = prompt;

    if (SubmitBackground(std::move(task)))
        g_queueCv.notify_one();
}

void OllamaDispatch_SubmitRelationship(uint64_t botGuid, uint64_t otherGuid,
                                       const std::string& otherName,
                                       const std::string& prompt)
{
    if (botGuid == 0 || otherGuid == 0 || prompt.empty())
        return;

    Task task;
    task.type            = TaskType::Relationship;
    task.memoryBotGuid   = botGuid;
    task.memoryOtherGuid = otherGuid;
    task.memoryOtherName = otherName;
    task.memoryPrompt    = prompt;

    if (SubmitBackground(std::move(task)))
        g_queueCv.notify_one();
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
