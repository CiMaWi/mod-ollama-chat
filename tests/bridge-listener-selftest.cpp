// J5d offline proof of the 127.0.0.1 bridge channel listener: token, bind address, malformed and oversize input,
// thread handoff, deadline, shutdown and configuration. It links the real listener (and its cpp-httplib) against a
// counting fake mailbox and against the production WorldChannel, with this thread playing the world tick. Raw
// Winsock clients control every byte on the wire. Windows only (the module's build host).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
// After the Winsock headers, which supply its types.
#include <iphlpapi.h>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

#include "mod-ollama-chat_bridge_listener.h"
#include "mod-ollama-chat_bridge_query.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace
{
using namespace OllamaBridge;
using Json = nlohmann::json;
using Steady = std::chrono::steady_clock;

std::atomic<unsigned> checks{0};
void Check(bool condition, char const* label)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(label);
}

// Obvious fakes; never a real link token.
std::string const Token = "fake-listener-token-for-selftest-only";
std::string const Token2 = "second-fake-listener-token-for-selftest";

int64_t Since(Steady::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Steady::now() - start).count();
}

int64_t UnixNow()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

in_addr Loopback()
{
    in_addr address{};
    address.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

uint16_t FreePort()
{
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr = Loopback();
    int length = sizeof(address);
    bool const ok = probe != INVALID_SOCKET && bind(probe, reinterpret_cast<sockaddr*>(&address), length) == 0 &&
                    getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    closesocket(probe);
    Check(ok && ntohs(address.sin_port) >= 1024, "probe socket picks a free loopback port");
    return ntohs(address.sin_port);
}

SOCKET ConnectTo(in_addr host, uint16_t port, int timeoutMs)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return s;
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr = host;
    address.sin_port = htons(port);
    if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR &&
        WSAGetLastError() != WSAEWOULDBLOCK)
    {
        closesocket(s);
        return INVALID_SOCKET;
    }
    fd_set writable;
    fd_set failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(s, &writable);
    FD_SET(s, &failed);
    timeval wait{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int error = 0;
    int length = sizeof(error);
    if (select(0, nullptr, &writable, &failed, &wait) <= 0 || FD_ISSET(s, &failed) ||
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0 || error != 0)
    {
        closesocket(s);
        return INVALID_SOCKET;
    }
    nonBlocking = 0;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    return s;
}

struct Reply
{
    bool connected = false;
    int status = 0;
    std::string headers;
    std::string body;
    int64_t ms = 0;
};

// Sends the exact bytes, then reads until the server closes (every reply is Connection: close).
Reply Exchange(uint16_t port, std::string const& request, DWORD timeoutMs = 9000)
{
    Reply reply;
    auto const start = Steady::now();
    SOCKET s = ConnectTo(Loopback(), port, 3000);
    if (s == INVALID_SOCKET)
        return reply;
    reply.connected = true;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const*>(&timeoutMs), sizeof(timeoutMs));
    for (std::size_t sent = 0; sent < request.size();)
    {
        int const n = send(s, request.data() + sent, static_cast<int>(request.size() - sent), 0);
        if (n <= 0)
            break;
        sent += static_cast<std::size_t>(n);
    }
    std::string raw;
    char buffer[4096];
    for (int n = 0; (n = recv(s, buffer, sizeof(buffer), 0)) > 0;)
        raw.append(buffer, static_cast<std::size_t>(n));
    closesocket(s);
    reply.ms = Since(start);
    auto const end = raw.find("\r\n\r\n");
    if (raw.rfind("HTTP/1.1 ", 0) == 0 && raw.size() >= 12 && end != std::string::npos)
    {
        reply.status = std::stoi(raw.substr(9, 3));
        reply.headers = raw.substr(0, end);
        reply.body = raw.substr(end + 4);
    }
    return reply;
}

std::string Auth(std::string const& token = Token) { return "Authorization: Bearer " + token + "\r\n"; }

std::string JsonHeaders(std::size_t length)
{
    return "Content-Type: application/json\r\nContent-Length: " + std::to_string(length) + "\r\n";
}

std::string Http(std::string const& method, std::string const& target, std::string const& headers,
                 std::string const& body = {})
{
    return method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" + headers + "\r\n" + body;
}

Reply Post(uint16_t port, std::string const& route, std::string const& body, std::string const& token = Token)
{
    return Exchange(port, Http("POST", "/v1/channel/" + route, Auth(token) + JsonHeaders(body.size()), body));
}

// A request refused at admission is sent without its body: closing a socket with unread bytes resets it.
Reply Refused(uint16_t port, std::string const& token, std::string const& route = "gen-1")
{
    return Exchange(port, Http("POST", "/v1/channel/" + route, Auth(token) + JsonHeaders(2)));
}

std::string Envelope(int64_t deadlineOffsetMs)
{
    return Json{{"type", "query"}, {"requestId", "q-1"}, {"deadlineUnixMs", UnixNow() + deadlineOffsetMs}}.dump();
}

bool Error(Reply const& reply, int status, char const* name)
{
    return reply.status == status && reply.body == std::string("{\"error\":\"") + name + "\"}" &&
           reply.headers.find("Connection: close") != std::string::npos &&
           reply.headers.find("Content-Type: application/json") != std::string::npos;
}

// Rows of the TCP listener table owned by this process on the port, as IPv4 addresses (v6 rows counted apart).
std::vector<uint32_t> ListenRows(uint16_t port, unsigned& v6Rows)
{
    std::vector<uint32_t> rows;
    v6Rows = 0;
    DWORD const pid = GetCurrentProcessId();
    for (ULONG family : {AF_INET, AF_INET6})
    {
        DWORD size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
        std::vector<unsigned char> table(size + 4096);
        size = static_cast<DWORD>(table.size());
        Check(GetExtendedTcpTable(table.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR,
              "read the TCP listener table");
        if (family == AF_INET)
        {
            auto const* t = reinterpret_cast<MIB_TCPTABLE_OWNER_PID const*>(table.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i)
                if (t->table[i].dwOwningPid == pid && ntohs(static_cast<u_short>(t->table[i].dwLocalPort)) == port)
                    rows.push_back(t->table[i].dwLocalAddr);
        }
        else
        {
            auto const* t = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID const*>(table.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i)
                if (t->table[i].dwOwningPid == pid && ntohs(static_cast<u_short>(t->table[i].dwLocalPort)) == port)
                    ++v6Rows;
        }
    }
    return rows;
}

bool LoopbackOnly(uint16_t port)
{
    unsigned v6 = 0;
    auto const rows = ListenRows(port, v6);
    return rows.size() == 1 && rows[0] == htonl(INADDR_LOOPBACK) && v6 == 0;
}

bool NotListening(uint16_t port)
{
    unsigned v6 = 0;
    return ListenRows(port, v6).empty() && v6 == 0;
}

template <typename Condition>
bool WaitFor(Condition condition, int64_t deadlineMs = 2000)
{
    auto const start = Steady::now();
    while (!condition())
    {
        if (Since(start) > deadlineMs)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

struct FakeMailbox
{
    std::mutex mutex;
    std::string expected = Token;
    unsigned receives = 0;
    std::vector<std::thread::id> threads;
    ChannelError receiveResult = ChannelError::None;
    ChannelError pendingTake = ChannelError::Unknown;
    uint64_t sequence = 0;
    std::map<uint64_t, std::unique_ptr<ChannelPacket>> done;

    ChannelError Receive(std::string_view bearer, std::string_view, std::string_view, uint64_t& receipt)
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++receives;
        threads.push_back(std::this_thread::get_id());
        if (bearer != expected)
            return ChannelError::Unauthorized;
        if (receiveResult != ChannelError::None)
            return receiveResult;
        receipt = ++sequence;
        return ChannelError::None;
    }
    ChannelError Take(std::string_view bearer, uint64_t receipt, std::unique_ptr<ChannelPacket>& packet)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (bearer != expected)
            return ChannelError::Unauthorized;
        auto it = done.find(receipt);
        if (it == done.end())
            return pendingTake;
        packet = std::move(it->second);
        done.erase(it);
        return ChannelError::None;
    }
    void Publish(uint64_t receipt, ChannelError error, std::string body)
    {
        auto packet = std::make_unique<ChannelPacket>();
        packet->receipt = receipt;
        packet->error = error;
        packet->body = std::move(body);
        std::lock_guard<std::mutex> lock(mutex);
        done[receipt] = std::move(packet);
    }
    unsigned Receives()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return receives;
    }
    void Set(ChannelError receive, ChannelError take = ChannelError::Unknown)
    {
        std::lock_guard<std::mutex> lock(mutex);
        receiveResult = receive;
        pendingTake = take;
    }
};

struct Harness
{
    FakeMailbox mailbox;
    std::mutex logMutex;
    std::vector<std::string> logs;
    ChannelListener listener{[this](std::string_view bearer, std::string_view route, std::string_view body,
                                    uint64_t& receipt) { return mailbox.Receive(bearer, route, body, receipt); },
                             [this](std::string_view bearer, uint64_t receipt, std::unique_ptr<ChannelPacket>& packet)
                             { return mailbox.Take(bearer, receipt, packet); },
                             [this](bool, std::string const& line)
                             {
                                 std::lock_guard<std::mutex> lock(logMutex);
                                 logs.push_back(line);
                             }};
    uint16_t port = 0;
    explicit Harness(bool start = true)
    {
        if (start)
        {
            port = FreePort();
            Check(listener.Configure(true, port, Token) && listener.Running(), "listener starts on a free port");
        }
    }
    bool LogsClean()
    {
        std::lock_guard<std::mutex> lock(logMutex);
        for (auto const& line : logs)
            if (line.find(Token) != std::string::npos || line.find(Token2) != std::string::npos ||
                line.find("secret-route") != std::string::npos || line.find("deadlineUnixMs") != std::string::npos)
                return false;
        return true;
    }
};

void PureRules()
{
    Check(std::string_view(ListenerHost) == "127.0.0.1" && DefaultListenerPort == 18732, "literal host, port");
    Check(!ListenerPortValid(0) && !ListenerPortValid(80) && !ListenerPortValid(1023) && ListenerPortValid(1024) &&
              ListenerPortValid(18732) && ListenerPortValid(65535) && !ListenerPortValid(65536) &&
              !ListenerPortValid(70000),
          "port range 1024-65535");
    Check(!ListenerTokenValid("") && !ListenerTokenValid(std::string(21, 'a')) &&
              ListenerTokenValid(std::string(22, 'a')) && ListenerTokenValid(std::string(256, 'a')) &&
              !ListenerTokenValid(std::string(257, 'a')),
          "token length 22-256");
    Check(ListenerPeerAllowed("127.0.0.1") && !ListenerPeerAllowed("127.0.0.2") && !ListenerPeerAllowed("::1") &&
              !ListenerPeerAllowed("::ffff:127.0.0.1") && !ListenerPeerAllowed("192.168.1.5") &&
              !ListenerPeerAllowed("") && !ListenerPeerAllowed("127.0.0.10"),
          "peer must be literal 127.0.0.1");
    Check(ListenerBearerMatches("Bearer " + Token, Token), "exact bearer matches");
    for (std::string const& header :
         {std::string(), Token, "bearer " + Token, "Bearer  " + Token, "Bearer" + Token, "Basic " + Token,
          "Bearer " + Token + "x", "Bearer " + Token.substr(1), "Bearer " + Token2, std::string("Bearer ")})
        Check(!ListenerBearerMatches(header, Token), "miswritten, short, long or other bearer refused");
    Check(!ListenerBearerMatches("Bearer ", "") &&
              !ListenerBearerMatches("Bearer " + std::string(257, 'a'), std::string(257, 'a')),
          "empty or over-256 token never authenticates");
    Check(ListenerRouteValid("gen-1") && ListenerRouteValid("a.b:c_d-e") && ListenerRouteValid(std::string(128, 'r')),
          "identifier routes pass");
    for (std::string const& route : {std::string(), std::string("-a"), std::string("a/b"), std::string("a%2Fb"),
                                     std::string("a?b"), std::string("a b"), std::string(129, 'r')})
        Check(!ListenerRouteValid(route), "non-identifier route refused");
    Check(ListenerBodyShape("{}") && ListenerBodyShape(" {\"a\":1} \n"), "object bodies pass the shape gate");
    for (char const* body : {"", "   ", "[1]", "null", "{", "x{}", "\"{}\""})
        Check(!ListenerBodyShape(body), "non-object body refused");
    int64_t const now = 1700000000000;
    Check(ListenerWaitMs("{}", now) == ListenerWaitCapMs, "no deadline waits the cap");
    Check(ListenerWaitMs("{\"deadlineUnixMs\":" + std::to_string(now + 300) + "}", now) == 300, "deadline bounds");
    Check(ListenerWaitMs("{\"deadlineUnixMs\" : " + std::to_string(now + 300) + "}", now) == 300, "spaces");
    Check(ListenerWaitMs("{\"deadlineUnixMs\":" + std::to_string(now - 5) + "}", now) == 0, "past deadline is 0");
    Check(ListenerWaitMs("{\"deadlineUnixMs\":" + std::to_string(now + 60000) + "}", now) == ListenerWaitCapMs,
          "far deadline capped at the packet liveness");
    Check(ListenerWaitMs("{\"deadlineUnixMs\":\"1\"}", now) == ListenerWaitCapMs &&
              ListenerWaitMs("{\"deadlineUnixMs\":12345678901234567}", now) == ListenerWaitCapMs &&
              ListenerWaitMs("{\"deadlineUnixMs\"}", now) == ListenerWaitCapMs,
          "unreadable deadline waits the cap");
    Check(ListenerStatus(ChannelError::None) == 200 && ListenerStatus(ChannelError::Unauthorized) == 401 &&
              ListenerStatus(ChannelError::Inactive) == 503 && ListenerStatus(ChannelError::Invalid) == 400 &&
              ListenerStatus(ChannelError::TooLarge) == 413 && ListenerStatus(ChannelError::Stale) == 410 &&
              ListenerStatus(ChannelError::Duplicate) == 409 && ListenerStatus(ChannelError::Capacity) == 429 &&
              ListenerStatus(ChannelError::Closed) == 410 && ListenerStatus(ChannelError::Unknown) == 400 &&
              ListenerStatus(ChannelError::Standing) == 400,
          "one ChannelError to status table");
}

void Configuration()
{
    Harness h(false);
    uint16_t const port = FreePort();
    for (uint32_t bad : {0u, 80u, 1023u, 70000u})
        Check(!h.listener.Configure(true, bad, Token) && !h.listener.Running(), "invalid port leaves it off");
    Check(h.logs.size() == 4 && h.logs[0].find("ChannelPort must be 1024-65535") != std::string::npos,
          "one fixed warning per invalid port");
    Check(!h.listener.Configure(true, port, "short") && !h.listener.Configure(true, port, std::string(257, 'a')) &&
              !h.listener.Configure(false, port, Token) && !h.listener.Running() && h.listener.Starts() == 0,
          "invalid token or disabled channel leaves it off");
    Check(h.listener.Configure(true, port, Token) && h.listener.Starts() == 1 && h.listener.Port() == port &&
              LoopbackOnly(port),
          "valid settings start one 127.0.0.1 listener");
    Check(h.listener.Configure(true, port, Token) && h.listener.Starts() == 1, "same settings do not restart");
    uint16_t const moved = FreePort();
    Check(h.listener.Configure(true, moved, Token) && h.listener.Starts() == 2 && h.listener.Port() == moved,
          "a new port restarts the listener");
    Check(NotListening(port) && LoopbackOnly(moved) && !Exchange(port, Http("GET", "/", "")).connected,
          "the old port is closed after the move");
    Check(Error(Exchange(moved, Http("GET", "/v1/channel/x", "")), 401, "unauthorized"), "moved listener answers");
    Check(h.listener.Configure(true, moved, Token2) && h.listener.Starts() == 3, "a token change restarts");
    h.mailbox.expected = Token2;
    h.mailbox.Set(ChannelError::Duplicate);
    Check(Error(Refused(moved, Token), 401, "unauthorized") && h.mailbox.Receives() == 0,
          "the old token is refused before the mailbox");
    Check(Error(Post(moved, "gen-1", "{}", Token2), 409, "duplicate") && h.mailbox.Receives() == 1,
          "the new token reaches the mailbox");
    Check(!h.listener.Configure(false, moved, Token2) && !h.listener.Running() && NotListening(moved),
          "disabling the channel closes the port");
    Check(h.LogsClean(), "lifecycle logs never carry the token");
}

void TokenAndInput(Harness& h)
{
    auto const headersOnly = [&](std::string const& headers, std::string const& target = "/v1/channel/gen-1")
    { return Exchange(h.port, Http("POST", target, headers)); };
    // Bodies are announced but withheld: a reply proves the server answered without reading them.
    for (std::string const& auth :
         {std::string(), "Authorization: Bearer " + std::string(Token.size(), 'x') + "\r\n", Auth("too-short-token"),
          "Authorization: bearer " + Token + "\r\n", "Authorization: Bearer" + Token + "\r\n",
          "Authorization: Basic " + Token + "\r\n", Auth(Token + "x")})
        Check(Error(headersOnly(auth + JsonHeaders(2)), 401, "unauthorized"), "bad bearer is 401");
    Reply const big = headersOnly(Auth("wrong-token-wrong-token-wrong") + JsonHeaders(70000));
    Check(Error(big, 401, "unauthorized") && big.ms < 1000, "oversize with a bad token is 401, body never read");
    Check(Error(Exchange(h.port,
                         Http("POST", "/v1/channel/gen-1", "Expect: 100-continue\r\n" + Auth(Token2) + JsonHeaders(2))),
                401, "unauthorized"),
          "Expect: 100-continue is authenticated before the interim reply");
    Check(h.mailbox.Receives() == 0 && h.listener.Stats().unauthorized == 9, "no bad bearer reaches the mailbox");

    Reply const oversize = headersOnly(Auth() + JsonHeaders(MaxRequestBytes + 1));
    Check(Error(oversize, 413, "too_large") && oversize.ms < 1000, "65537 bytes refused before reading");
    Check(Error(headersOnly(Auth() + JsonHeaders(1000000000)), 413, "too_large"), "huge Content-Length refused");
    Check(Error(headersOnly(Auth() + "Content-Type: application/json\r\nTransfer-Encoding: chunked\r\n"), 413,
                "too_large"),
          "chunked body refused");
    Check(Error(headersOnly(Auth() + JsonHeaders(2) + "Content-Length: 2\r\n"), 413, "too_large"),
          "duplicate Content-Length refused");
    Check(Error(headersOnly(Auth() + JsonHeaders(2), "/v1/channel/" + std::string(129, 'r')), 413, "too_large"),
          "route over 128 bytes refused");
    for (char const* route : {"", "a%2Fb", "-a", "a/b", "a?x=1", "secret-route%20x"})
        Check(Error(headersOnly(Auth() + JsonHeaders(2), std::string("/v1/channel/") + route), 400, "invalid_input"),
              "bad route syntax is 400");
    Check(Error(headersOnly(Auth() + "Content-Length: 2\r\n"), 400, "invalid_input"), "missing content type");
    Check(Error(headersOnly(Auth() + "Content-Type: text/plain\r\nContent-Length: 2\r\n"), 400, "invalid_input"),
          "wrong content type");
    Check(Error(headersOnly(Auth() + JsonHeaders(0)), 400, "invalid_input"), "empty body");
    for (std::string const& body :
         {std::string("hello"), std::string("[1,2]"), std::string("   "), std::string("\"{}\"")})
        Check(Error(Post(h.port, "gen-1", body), 400, "invalid_input"), "non-object body is 400");
    Check(Error(Exchange(h.port, Http("GET", "/v1/channel/gen-1", Auth())), 405, "method_not_allowed") &&
              Error(Exchange(h.port, Http("PUT", "/v1/channel/gen-1", Auth() + JsonHeaders(2))), 405,
                    "method_not_allowed"),
          "other methods are 405");
    Check(Error(headersOnly(Auth() + JsonHeaders(2), "/v1/other"), 404, "not_found") &&
              Error(Exchange(h.port, Http("GET", "/", Auth())), 404, "not_found"),
          "other paths are 404");
    Check(Error(Exchange(h.port, "BLAH\r\n\r\n"), 400, "invalid_input"), "unparsable request line is 400");
    Check(h.mailbox.Receives() == 0 && h.listener.Stats().accepted == 0, "refused input never reaches the mailbox");

    h.mailbox.Set(ChannelError::Duplicate);
    std::string const body = Envelope(1000);
    Check(Error(Exchange(h.port, Http("POST", "/v1/channel/secret-route",
                                      Auth() + "Content-Type: Application/JSON; charset=utf-8\r\nContent-Length: " +
                                          std::to_string(body.size()) + "\r\n",
                                      body)),
                409, "duplicate") &&
              h.mailbox.Receives() == 1,
          "charset parameter accepted; mailbox Duplicate is 409");
    h.mailbox.Set(ChannelError::None);
}

void Deadlines(Harness& h)
{
    auto const quick = [&](ChannelError receive, ChannelError take, int status, char const* name)
    {
        h.mailbox.Set(receive, take);
        Reply const reply = Post(h.port, "gen-1", Envelope(3000));
        Check(Error(reply, status, name) && reply.ms < 300, "mailbox error maps without waiting");
    };
    quick(ChannelError::Capacity, ChannelError::Unknown, 429, "capacity");
    quick(ChannelError::Inactive, ChannelError::Unknown, 503, "inactive");
    quick(ChannelError::Stale, ChannelError::Unknown, 410, "stale");
    quick(ChannelError::Closed, ChannelError::Unknown, 410, "closed");
    quick(ChannelError::TooLarge, ChannelError::Unknown, 413, "too_large");
    // J5e: a standing follow/stay keeps 400 invalid_input and adds its reason code and sentence.
    h.mailbox.Set(ChannelError::Standing);
    Reply const standing = Post(h.port, "gen-1", Envelope(3000));
    Check(standing.status == 400 &&
              standing.body ==
                  "{\"error\":\"invalid_input\",\"reason\":\"standing_not_supported\",\"reasonText\":"
                  "\"I can follow or stay until you say otherwise, but I cannot take it as a standing "
                  "order.\"}" &&
              Json::parse(standing.body).size() == 3,
          "a standing shape answers invalid_input with its reason");
    quick(ChannelError::None, ChannelError::Stale, 410, "stale");  // Terminal Take error ends the wait.
    h.mailbox.Set(ChannelError::None);
    Reply const short300 = Post(h.port, "gen-1", Envelope(300));
    Check(Error(short300, 504, "timeout") && short300.ms >= 280 && short300.ms < 500,
          "envelope deadline 300 ms ends the wait near 300 ms");
    Reply const past = Post(h.port, "gen-1", Envelope(-1000));
    Check(Error(past, 504, "timeout") && past.ms < 200, "a passed deadline times out at once");
    Check(h.listener.Stats().timeouts == 2, "timeouts counted");
}

void Handoff(Harness& h)
{
    h.mailbox.Set(ChannelError::None);
    unsigned const before = h.mailbox.Receives();
    uint64_t const receipt = h.mailbox.sequence + 1;
    std::thread::id client;
    auto reply = std::async(std::launch::async,
                            [&]
                            {
                                client = std::this_thread::get_id();
                                return Post(h.port, "gen-1", Envelope(3000));
                            });
    Check(WaitFor([&] { return h.mailbox.Receives() == before + 1; }), "request reaches the mailbox");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // The world tick comes later.
    std::string const result = R"({"type":"query.result","requestId":"q-1"})";
    h.mailbox.Publish(receipt, ChannelError::None, result);
    Reply const answered = reply.get();
    Check(answered.status == 200 && answered.body == result && answered.ms >= 100 &&
              answered.headers.find("Connection: close") != std::string::npos &&
              answered.headers.find("Content-Type: application/json") != std::string::npos,
          "the world's packet body is the 200 reply");
    std::thread::id const io = h.mailbox.threads.back();
    Check(io != std::this_thread::get_id() && io != client, "Receive ran on a listener thread, not the world");
    for (auto const& [error, status, name] :
         {std::tuple{ChannelError::Unknown, 400, "invalid_input"},
          std::tuple{ChannelError::Duplicate, 409, "duplicate"}, std::tuple{ChannelError::None, 400, "invalid_input"}})
    {
        uint64_t const next = h.mailbox.sequence + 1;
        h.mailbox.Publish(next, error, "");
        Check(Error(Post(h.port, "gen-1", Envelope(3000)), status, name), "packet error maps through the table");
    }
}

void BindAddress(Harness& h)
{
    Check(LoopbackOnly(h.port), "the only listening socket is 127.0.0.1 (no wildcard, no IPv6)");
    char host[256] = {};
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* found = nullptr;
    in_addr other{};
    bool have = false;
    if (gethostname(host, sizeof(host)) == 0 && getaddrinfo(host, nullptr, &hints, &found) == 0)
    {
        for (addrinfo* it = found; it && !have; it = it->ai_next)
        {
            auto const address = reinterpret_cast<sockaddr_in const*>(it->ai_addr)->sin_addr;
            if ((ntohl(address.s_addr) >> 24) != 127)
            {
                other = address;
                have = true;
            }
        }
        freeaddrinfo(found);
    }
    if (!have)
    {
        std::cout << "SKIP non-loopback connect: this machine has no non-loopback IPv4 address\n";
        return;
    }
    SOCKET s = ConnectTo(other, h.port, 3000);
    Check(s == INVALID_SOCKET, "the port is unreachable on the machine's other IPv4 address");
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &other, text, sizeof(text));
    std::cout << "PASS non-loopback connect refused on " << text << "\n";
}

void SlowLoris()
{
    Harness h;
    Reply const stalled = Exchange(h.port, "POST /v1/channel/gen-1 HTTP/1.1\r\nHost: 127.0.0.1\r\n");
    Check(stalled.connected && stalled.ms < 3500 && h.mailbox.Receives() == 0,
          "stalled headers are closed within the read timeout");
}

void NeverTicks()
{
    Harness h;
    Reply const reply = Post(h.port, "gen-1", "{\"type\":\"query\"}");
    Check(Error(reply, 504, "timeout") && reply.ms >= ListenerWaitCapMs - 20 && reply.ms < ListenerWaitCapMs + 200,
          "no world tick and no deadline: 504 at the 5000 ms cap");
}

void Shutdown()
{
    Harness h;
    auto first = std::async(std::launch::async, [&] { return Post(h.port, "gen-1", "{}"); });
    auto second = std::async(std::launch::async, [&] { return Post(h.port, "gen-2", "{}"); });
    Check(WaitFor([&] { return h.mailbox.Receives() == 2; }), "two requests wait in the listener");
    auto const start = Steady::now();
    h.listener.Stop();
    int64_t const stopped = Since(start);
    Reply const a = first.get();
    Reply const b = second.get();
    Check(stopped < 500 && Error(a, 503, "inactive") && Error(b, 503, "inactive") && a.ms < 1500 && b.ms < 1500,
          "Stop() ends both waiting requests and returns within 500 ms");
    Check(!h.listener.Running() && NotListening(h.port), "no listener remains and the port is free");
    Check(h.listener.Configure(true, h.port, Token) && h.listener.Running() && LoopbackOnly(h.port),
          "the freed port binds again");
    auto third = std::async(std::launch::async, [&] { return Post(h.port, "gen-3", "{}"); });
    Check(WaitFor([&] { return h.mailbox.Receives() == 3; }), "a request waits again");
    auto const again = Steady::now();
    Check(!h.listener.Configure(false, h.port, Token) && Since(again) < 500, "Configure(false) stops it, bounded");
    Check(Error(third.get(), 503, "inactive") && NotListening(h.port), "Configure(false) ends the wait too");
    h.listener.Stop();  // Idempotent.
    Check(h.LogsClean(), "shutdown logs never carry the token");
}

// Real production mailbox: the bridge-query-selftest fixture with a fixed world clock taken from the real one,
// so the envelope deadline the listener reads is real time.
struct Unit
{
    uint64_t guid = 0;
    uint64_t group = 33;
    std::string name;
};

struct Source : QueryWorldSource
{
    DispatchGuard guard;
    std::map<uint64_t, Unit> objects;
    std::vector<uint64_t> members{11, 22};
    bool Observe(WorldTicket const&, DispatchGuard& current) override
    {
        current = guard;
        return true;
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
        auto const it = objects.find(guid);
        if (it == objects.end())
            return false;
        result = {it->second.guid, it->second.group, it->second.name, 70};
        return true;
    }
    bool ReadMark(WorldTicket const&, uint8_t, uint64_t&) override { return false; }
};

struct RealFixture
{
    Clock now = Now();
    Settings settings{"fixture", "codex", "http://127.0.0.1:47800", Token, false, "", ""};
    WorldPolicy world;
    Source source;
    WorldChannel channel{world, [this](WorldTicket const& ticket, DispatchGuard& guard)
                         { return source.Observe(ticket, guard); }, [this] { return now; }, &source};
    WorldTicket ticket;
    RealFixture()
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
        Check(world.ObserveScope(guard.scope, now), "capture world scope");
        Context input;
        input.speakerGuid = 22;
        input.channel = "direct";
        input.text = "What can we see?";
        ticket = world.Reserve(guard.scope, input, now, 30000);
        Check(ticket.generation != 0 && channel.Register(ticket, guard), "register game generation route");
        for (auto const guid : source.members)
            source.objects[guid] = {guid, 33, "unit-" + std::to_string(guid)};
    }
    std::string Query(std::string const& id, int64_t deadline = 0) const
    {
        return Json{{"version", 1},
                    {"type", "query"},
                    {"instance", "fixture"},
                    {"provider", "codex"},
                    {"epoch", ticket.scope.epoch},
                    {"requestId", id},
                    {"deadlineUnixMs", deadline ? deadline : ticket.request.deadline.unixMs},
                    {"controllerGuid", "22"},
                    {"botGuid", "11"},
                    {"query", "observable_group"}}
            .dump();
    }
};

void RealChannel()
{
    RealFixture f;
    ChannelListener listener([&](std::string_view bearer, std::string_view route, std::string_view body,
                                 uint64_t& receipt) { return f.channel.Receive(bearer, route, body, receipt); },
                             [&](std::string_view bearer, uint64_t receipt, std::unique_ptr<ChannelPacket>& packet)
                             { return f.channel.Take(bearer, receipt, packet); });
    uint16_t const port = FreePort();
    Check(listener.Configure(true, port, Token), "listener over the production mailbox");
    std::string const route = f.ticket.request.requestId;
    Check(ListenerRouteValid(route), "game-issued request ids pass the route rule");
    // One world tick per request, played here after the listener thread queued the packet.
    auto const tick = [&](std::future<Reply>& pending)
    {
        Check(WaitFor([&] { return f.channel.Used() == 1; }), "listener thread queued one physical packet");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Check(f.channel.Update() == 1, "world tick dequeues the packet");
        return pending.get();
    };
    auto query = std::async(std::launch::async, [&] { return Post(port, route, f.Query("query-1")); });
    Reply const answered = tick(query);
    Json const result = Json::parse(answered.body, nullptr, false);
    Check(answered.status == 200 && result.is_object() && result.value("type", "") == "query.result" &&
              result.value("requestId", "") == "query-1",
          "query round trip returns the filtered query.result");
    Check(WaitFor([&] { return f.channel.Used() == 0; }, 500), "physical slot released after the write");
    auto replay = std::async(std::launch::async, [&] { return Post(port, route, f.Query("query-1")); });
    Check(Error(tick(replay), 409, "duplicate"), "replayed request id is 409 from the world policy");
    auto junk = std::async(std::launch::async, [&] { return Post(port, route, "{not json}"); });
    Check(Error(tick(junk), 400, "invalid_input"), "strict decode failure from the world is 400");
    auto dropped = std::async(std::launch::async,
                              [&] { return Post(port, "unregistered-route", f.Query("query-2", UnixNow() + 300)); });
    Reply const gone = tick(dropped);
    Check(Error(gone, 504, "timeout") && gone.ms >= 280 && gone.ms < 600 && f.channel.Used() == 0,
          "a packet the world drops ends as a timeout at the envelope deadline");
    Check(Error(Refused(port, Token2, route), 401, "unauthorized") && f.channel.Used() == 0,
          "wrong bearer never reaches the production mailbox");
    listener.Stop();
}
}  // namespace

int main()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::cout << "FAIL WSAStartup\n";
        return 1;
    }
    int status = 0;
    try
    {
        PureRules();
        // The slow cases run on their own listeners beside the rest.
        auto neverTicks = std::async(std::launch::async, NeverTicks);
        auto slowLoris = std::async(std::launch::async, SlowLoris);
        Configuration();
        {
            Harness h;
            TokenAndInput(h);
            Deadlines(h);
            Handoff(h);
            BindAddress(h);
            Check(h.LogsClean(), "request handling logs nothing secret");
        }
        Shutdown();
        RealChannel();
        slowLoris.get();
        neverTicks.get();
        std::cout << "PASS bridge listener selftest\n" << checks.load() << " checks passed\n";
    }
    catch (std::exception const& e)
    {
        std::cout << "FAIL " << e.what() << "\n";
        status = 1;
    }
    WSACleanup();
    return status;
}
