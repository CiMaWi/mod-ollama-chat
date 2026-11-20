#ifndef MOD_OLLAMA_CHAT_BRIDGE_LISTENER_H
#define MOD_OLLAMA_CHAT_BRIDGE_LISTENER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "mod-ollama-chat_bridge_world.h"

namespace OllamaBridge
{
// J5d transport: the bridge posts one J5 query or command envelope to POST /v1/channel/{route} on 127.0.0.1 and
// gets the world thread's filtered result back in the same exchange. Listener threads only authenticate, bound
// bytes and call the I/O-safe Receive/Take; every game read stays in WorldChannel::Update on the world tick (C3).
// This header and its source include no game headers, so the offline test links the real listener standalone.
constexpr char const* ListenerHost = "127.0.0.1";       // Literal loopback only (C9); never configurable.
constexpr uint32_t DefaultListenerPort = 18732;         // The Claude bridge listens on 18731.
// LCB6b: the world answers every packet it took, at the latest as stale once it is this old; the cap bounds only a
// world tick that never comes.
constexpr int64_t ListenerWaitCapMs = WorldLivenessMs;

struct ListenerStats
{
    uint64_t accepted = 0;      // Requests handed to Receive and accepted by the mailbox.
    uint64_t unauthorized = 0;  // Requests refused by the bearer check before the body was read.
    uint64_t timeouts = 0;      // Accepted requests with no world answer by the wait deadline.
    uint64_t overloads = 0;     // Requests answered overload (packet slots or route ids; LC.38), logged.
};

// Pure rules, exposed for the offline test.
bool ListenerPortValid(uint32_t port);
bool ListenerTokenValid(std::string_view token);
bool ListenerPeerAllowed(std::string_view remoteAddr);
bool ListenerRouteValid(std::string_view route);
// A cheap shape gate (one JSON object) so plain junk never takes a mailbox slot; the mailbox decodes strictly.
bool ListenerBodyShape(std::string_view body);
// The one ChannelError -> HTTP status table; the response body is {"error":ChannelErrorName}.
int ListenerStatus(ChannelError error);

class ChannelListener
{
public:
    // Called only after BearerMatches passed (LCB6a: the one bearer check). LCB6b: None means the world answers the
    // packet through answer, which the handler waits on; any other value is the answer.
    using Receive = std::function<ChannelError(std::string_view route, std::string_view body,
                                               std::shared_ptr<ChannelAnswer> const& answer)>;
    // Lines hold fixed text, the port and a count only: no token, header, route or body ever reaches the log.
    using Log = std::function<void(bool warning, std::string const& line)>;

    explicit ChannelListener(Receive receive, Log log = {});
    ~ChannelListener();
    ChannelListener(ChannelListener const&) = delete;
    ChannelListener& operator=(ChannelListener const&) = delete;

    // World thread, on every config load: starts, keeps (same port and token) or stops the listener.
    // Returns whether it runs afterwards.
    bool Configure(bool enabled, uint32_t port, std::string token);
    // Bounded: waiting handlers are answered inactive at once, then every listener thread is joined.
    void Stop();
    bool Running() const;
    uint32_t Port() const;
    uint64_t Starts() const;
    ListenerStats Stats() const;

private:
    struct Run;
    bool StartLocked(uint32_t port, std::string token);
    void StopLocked(bool log);
    void Write(bool warning, std::string const& line) const;
    void Overload();

    Receive const _receive;
    Log const _log;
    mutable std::mutex _mutex;  // Lifecycle only; request threads never take it.
    std::unique_ptr<Run> _run;
    uint64_t _starts = 0;
    std::atomic<uint64_t> _accepted{0};
    std::atomic<uint64_t> _unauthorized{0};
    std::atomic<uint64_t> _timeouts{0};
    std::atomic<uint64_t> _overloads{0};
};
}  // namespace OllamaBridge

#endif
