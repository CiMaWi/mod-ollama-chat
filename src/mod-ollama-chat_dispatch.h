#ifndef MOD_OLLAMA_CHAT_DISPATCH_H
#define MOD_OLLAMA_CHAT_DISPATCH_H

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
    // Set by SubmitBridge, the only way in.
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

    // The governor's pacing scope.
    std::string scopeKey;

    // This line was aimed at this bot in particular -- whispered, named, or
    // said in a small party it belongs to -- rather than said to the room.
    // Decided on the world thread at submit time, where the Group is live, and
    // carried here so delivery does not have to guess it back from `source`.
    // Direct address skips the pacing cooldowns and repetition suppression.
    bool directAddress = false;

    // Resolved on the world thread before submission so the worker never has
    // to touch a Player to know these.
    std::string botName;
};

// World-only adapter. Null by default: b2a cannot produce effects until b2b supplies real game validation.
// Called afresh before cleanup and each effect; must not cache game pointers or accept wire-authored authority.
using OllamaBridgeObserver = bool (*)(OllamaBridge::WorldTicket const&, OllamaBridge::DispatchGuard&);
void OllamaDispatch_SetBridgeObserver(OllamaBridgeObserver observer);
bool OllamaDispatch_SubmitBridge(OllamaChatRequest request, OllamaBridge::WorldScope const& scope,
                                 OllamaBridge::Context input, OllamaBridge::DispatchGuard guard, int64_t lifetimeMs);

// World-thread game adapter and bounded, audience-scoped conversation memory. No legacy memory is imported.
bool OllamaChat_BridgeObserve(OllamaBridge::WorldTicket const& ticket, OllamaBridge::DispatchGuard& guard);
OllamaBridge::QueryWorldSource& OllamaChat_BridgeQuerySource();
// LC20b: the store packet's world (mod-ollama-chat_bridge_store.cpp), playerbots' past and record; world thread.
OllamaBridge::StoreWorld& OllamaChat_BridgeStoreWorld();
// LC3: installs the channel's ability index from playerbots' table (world thread, once at dispatcher start).
void OllamaChat_BridgeInstallAbilities();
// World-only typed effect adapter (bridge_effect.cpp).
OllamaBridge::CommandExecutor& OllamaChat_BridgeCommandExecutor();
void OllamaChat_BridgeRefresh();
void OllamaChat_BridgeReset();
void OllamaChat_BridgeRemember(OllamaBridge::WorldTicket const& ticket, std::string const& reply);
// J6e-c, world thread (post-map-join scan): opens the wake generation of a fired watch through BridgeSubmit on a fresh
// capture with the stored bot, partner and source; false when none opened (capture failed, context changed, refused).
bool OllamaChat_BridgeWake(OllamaBridge::Watch const& watch);
// J6e-c, world thread (logout hook): ends the watches whose bot or partner is guid.
void OllamaDispatch_BridgeWatchLogout(uint64_t guid);

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
void OllamaDispatch_BridgeConfigured();
// J5d, world thread on every config load: starts, keeps or stops the 127.0.0.1 channel listener. It runs whenever the
// Bridge settings are valid (LCB6b: no ChannelEnabled), and restarts only when the port or the token changed.
void OllamaDispatch_BridgeListen(uint32_t port);
OllamaBridge::WorldPolicy& OllamaDispatch_BridgeWorld();

// Shared native-adapter boundary; the listener checked the local game-link bearer before it calls this (LCB6a).
// route is an existing game-issued generation request id, never a wire-authored world scope. None: the world thread
// answers the packet through answer (LCB6b).
OllamaBridge::ChannelError OllamaDispatch_ChannelReceive(std::string_view route, std::string_view body,
                                                         std::shared_ptr<OllamaBridge::ChannelAnswer> const& answer);

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
