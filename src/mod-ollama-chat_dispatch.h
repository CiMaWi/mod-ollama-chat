#ifndef MOD_OLLAMA_CHAT_DISPATCH_H
#define MOD_OLLAMA_CHAT_DISPATCH_H

#include "mod-ollama-chat_capability.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_bridge_world.h"
#include "mod-ollama-chat_bridge_query.h"
#include "mod-ollama-chat_api.h"

#include "ObjectGuid.h"
#include <string>
#include <cstdint>

class Player;
class Channel;

namespace PlayerbotBridge
{
class PostMapJoin;
}

// --------------------------------------------------------------------------
// Central dispatcher.
//
// THE RULE THIS FILE EXISTS TO ENFORCE:
//   worker threads do HTTP and string work only.
//   every read or write of a Player, Channel, Guild, Group or Map happens on
//   the world thread.
//
// Previously each candidate bot spawned its own detached std::thread which
// then called ObjectAccessor, Channel::Say, botAI->Say and re-entered the
// whole eligibility scan -- all racing Map::Update, and all unbounded.
//
// Now: the caller builds the prompt on the world thread and submits. A fixed
// pool of workers performs the generation. Completions come back through a
// queue that OllamaDispatch_Update() drains on the world thread, which is the
// only place delivery happens.
// --------------------------------------------------------------------------

// Resolve the zone-scoped instance of a numbered channel (General, Trade, ...)
// for this bot.
//
// ChannelMgr keys channels by their FULL name, and zone channels are named
// "General - Elwynn Forest", not "General" -- so GetChannel("General") always
// returns nullptr. Matching is by channel id plus zone-name containment, the
// same way PlayerbotAI::SayToChannel does it.
//
// World thread only.
Channel* OllamaResolveZoneChannel(Player* bot, uint32_t chatChannelId);

// Is this guid a member of this exact channel instance?
//
// The playerbots fork dropped Player::IsInChannel, and the core keeps
// Channel::IsOn private with no public replacement. This reaches IsOn itself
// so membership stays exact, including custom (unnumbered) channels.
//
// World thread only.
bool OllamaIsOnChannel(Channel const* channel, ObjectGuid guid);

struct OllamaChatRequest
{
    // Set only by SubmitBridge. Ordinary Submit rejects Bridge tags and unbound Bridge providers.
    uint64_t bridgeGeneration = 0;
    OllamaEndpointSettings bridgeSettings;
    OllamaBridge::WorldTicket bridgeTicket;
    OllamaBridge::DispatchGuard bridgeGuard;
    // Who speaks, and to whom.
    uint64_t botGuid    = 0;
    uint64_t targetGuid = 0;      // 0 when there is no specific addressee

    // Where the line goes.
    ChatChannelSourceLocal source = SRC_SAY_LOCAL;
    std::string            channelName;
    uint32_t               channelId = 0;

    // Loop control. Human-originated messages start at depth 0; each bot reply
    // to a bot increments it.
    uint8_t     chainDepth = 0;
    std::string scopeKey;

    // This line was aimed at this bot in particular -- whispered, named, or
    // said in a small party it belongs to -- rather than said to the room.
    // Decided on the world thread at submit time, where the Group is live, and
    // carried here so delivery does not have to guess it back from `source`.
    // Direct address skips the pacing cooldowns and repetition suppression.
    bool directAddress = false;

    // Generation.
    std::string       prompt;
    OllamaRequestKind kind = OllamaRequestKind::ChatReply;

    // Resolved on the world thread before submission so the worker never has
    // to touch a Player to know these.
    std::string botName;
    std::string originMessage;    // the message being replied to, if any

    // Post-delivery behaviour.
    bool triggerBotReplies = true;   // let other bots hear this line
    bool recordHistory     = false;  // append to conversation history
    bool updateSentiment   = false;  // run sentiment analysis on originMessage
};

// Submit a request. Returns false when the queue is at MaxQueueDepth, in which
// case the caller should simply skip this bot rather than pile up backlog.
bool OllamaDispatch_Submit(OllamaChatRequest request);

// World-only adapter. Null by default: b2a cannot produce effects until b2b supplies real game validation.
// Called afresh before cleanup and each effect; must not cache game pointers or accept wire-authored authority.
using OllamaBridgeObserver = bool (*)(OllamaBridge::WorldTicket const&, OllamaBridge::DispatchGuard&);
void OllamaDispatch_SetBridgeObserver(OllamaBridgeObserver observer);
bool OllamaDispatch_SubmitBridge(OllamaChatRequest request, OllamaBridge::WorldScope const& scope,
                                 OllamaBridge::Context input, OllamaBridge::DispatchGuard guard, int64_t lifetimeMs);

// World-thread game adapter and bounded, audience-scoped conversation memory. No legacy memory is imported.
bool OllamaChat_BridgeObserve(OllamaBridge::WorldTicket const& ticket, OllamaBridge::DispatchGuard& guard);
OllamaBridge::QueryWorldSource& OllamaChat_BridgeQuerySource();
// World-only typed effect adapter (bridge_effect.cpp). While kBridgeEffectsReleased is false it refuses
// every command without a game read.
OllamaBridge::CommandExecutor& OllamaChat_BridgeCommandExecutor();
void OllamaChat_BridgeRefresh();
void OllamaChat_BridgeReset();
void OllamaChat_BridgeRemember(OllamaBridge::WorldTicket const& ticket, std::string const& reply);

// Drain finished generations and deliver them. World thread only. join is the post-map-join token that only
// OllamaBotRandomChatter::OnUpdate constructs, on its stack; it is used during this call and never stored.
void OllamaDispatch_Update(uint32_t diff, PlayerbotBridge::PostMapJoin const& join);

namespace OllamaBridge
{
// The typed-effect phase. Only OllamaDispatch_Update builds one, from the post-map-join token, for its own
// WorldChannel::Update call; it is neither copied nor stored.
struct EffectPhase final
{
    EffectPhase(EffectPhase const&) = delete;
    EffectPhase& operator=(EffectPhase const&) = delete;

    PlayerbotBridge::PostMapJoin const& join;

private:
    friend void ::OllamaDispatch_Update(uint32_t, PlayerbotBridge::PostMapJoin const&);
    explicit EffectPhase(PlayerbotBridge::PostMapJoin const& token) : join(token) {}
};
}  // namespace OllamaBridge

void OllamaDispatch_Start();
void OllamaDispatch_Stop();

// Explicit world-thread lifecycle notifications. Never called by native/CLI selection or queued callbacks.
// Every publication retires old Bridge work, including bearer-only reloads. Legacy queues are untouched.
void OllamaDispatch_BridgeConfigured(bool channelEnabled = false);
OllamaBridge::WorldPolicy& OllamaDispatch_BridgeWorld();

// Shared native-adapter boundary; every I/O operation authenticates the local game-link bearer.
// route is an existing game-issued generation/event request id, never a wire-authored world scope.
// World-only event publication; J5c owns observed mark provenance and fresh capture.
bool OllamaDispatch_ChannelEvents(OllamaBridge::DispatchGuard const& guard, std::vector<uint8_t> const& icons);
// World-only route handoff for the later native adapter; contains no game pointer or authority grant.
bool OllamaDispatch_ChannelNextEvent(OllamaBridge::EventPublication& publication);
OllamaBridge::ChannelError OllamaDispatch_ChannelReceive(std::string_view bearer, std::string_view route,
                                                         std::string_view body, uint64_t& receipt);
OllamaBridge::ChannelError OllamaDispatch_ChannelTake(std::string_view bearer, uint64_t receipt,
                                                      std::unique_ptr<OllamaBridge::ChannelPacket>& packet);

// Fire-and-forget sentiment analysis. Runs entirely on a worker: the sentiment
// store is mutex-guarded in-memory state plus async DB writes, so it never
// needs the world thread -- and it must never run on it, because the analysis
// is a blocking HTTP call.
void OllamaDispatch_SubmitSentiment(uint64_t botGuid, uint64_t playerGuid,
                                    const std::string& message);

// Distil a bot's accumulated history into lasting memories, then clear it.
// Fire-and-forget; runs entirely on a worker.
void OllamaDispatch_SubmitCondensation(uint64_t botGuid, const std::string& prompt);

// Write or revise how a bot feels about someone.
void OllamaDispatch_SubmitRelationship(uint64_t botGuid, uint64_t otherGuid,
                                       const std::string& otherName,
                                       const std::string& prompt);

// A player emoted at a bot and the bot decided to answer in words.
// World thread only.
void OllamaChat_DispatchEmoteReaction(Player* bot, Player* player, uint32_t textEmote);

struct OllamaDispatchStats
{
    uint32_t queuedRequests;
    uint32_t inFlight;
    uint32_t pendingDeliveries;
    uint32_t workers;
    uint64_t totalSubmitted;
    uint64_t totalDelivered;
    uint64_t totalDroppedQueueFull;
    uint64_t totalDroppedEmpty;
    uint64_t totalDroppedGovernor;
    uint64_t totalFailed;
    std::string lastError;
};
OllamaDispatchStats OllamaDispatch_GetStats();

#endif // MOD_OLLAMA_CHAT_DISPATCH_H
