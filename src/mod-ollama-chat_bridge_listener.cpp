#include "mod-ollama-chat_bridge_listener.h"

// Same rename as mod-ollama-chat_httpclient.cpp (identical header text in both TUs, so ODR-safe): other modules
// bundle other cpp-httplib versions into the same monolithic `modules` static library.
#define httplib ollamachat_httplib
#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

#include "./httplib.h"

namespace OllamaBridge
{
namespace
{
// The bearer check relies on this version's order: the pre-routing handler runs after the headers and before the
// body is read (Server::routing), and the Expect: 100-continue handler runs before any interim reply.
static_assert(std::string_view{CPPHTTPLIB_VERSION} == "0.23.1",
              "the bridge channel listener is verified against the bundled cpp-httplib 0.23.1");

constexpr std::string_view RoutePrefix = "/v1/channel/";
constexpr std::size_t MaxRouteBytes = 128;
constexpr std::size_t MaxBearerBytes = 256;  // WorldChannel::Authenticated never accepts a longer bearer.
constexpr time_t IoTimeoutSec = 2;
constexpr std::size_t Workers = 2;        // A handler blocks while the world answers; the bridge sends few calls.
constexpr std::size_t MaxQueued = 8;      // Further connections are closed at once (the client reads unavailable).
constexpr time_t AcceptIdleUsec = 50000;  // Accept-loop wake-up; also bounds Stop().

struct StatusEntry
{
    ChannelError error;
    int status;
};

constexpr std::array<StatusEntry, 11> StatusTable{{
    {ChannelError::None, 200},
    {ChannelError::Unauthorized, 401},
    {ChannelError::Inactive, 503},
    {ChannelError::Invalid, 400},
    {ChannelError::TooLarge, 413},
    {ChannelError::Stale, 410},
    {ChannelError::Duplicate, 409},
    {ChannelError::Capacity, 429},
    {ChannelError::Closed, 410},
    {ChannelError::Unknown, 400},  // A packet type with no reverse adapter (e.g. a generation result) is bad input.
    {ChannelError::Standing, 400},
}};

bool JsonSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool Digit(char c) { return c >= '0' && c <= '9'; }

int64_t UnixMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void Reply(httplib::Response& res, int status, std::string_view name)
{
    std::string body;
    body.reserve(name.size() + 12);
    body.append("{\"error\":\"").append(name).append("\"}");
    res.status = status;
    res.set_content(body, "application/json");
}

void Reply(httplib::Response& res, ChannelError error)
{
    if (error == ChannelError::Standing)
    {
        // J5e: the fixed words need no escaping.
        auto const words = DescribeRefusal(CommandRefusal::StandingNotSupported);
        std::string body = "{\"error\":\"invalid_input\",\"reason\":\"";
        body.append(words.code).append("\",\"reasonText\":\"").append(words.text).append("\"}");
        res.status = ListenerStatus(error);
        res.set_content(body, "application/json");
        return;
    }
    Reply(res, ListenerStatus(error), ChannelErrorName(error == ChannelError::Unknown ? ChannelError::Invalid : error));
}

bool JsonContentType(std::string const& value)
{
    constexpr std::string_view type = "application/json";
    if (value.size() < type.size())
        return false;
    for (std::size_t i = 0; i < type.size(); ++i)
    {
        char c = value[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != type[i])
            return false;
    }
    return value.size() == type.size() || value[type.size()] == ';' || value[type.size()] == ' ';
}
}  // namespace

bool ListenerPortValid(uint32_t port) { return port >= 1024 && port <= 65535; }

bool ListenerTokenValid(std::string_view token)
{
    // ValidSettings' minimum; Authenticated's maximum (a longer token could never be presented successfully).
    return token.size() >= 22 && token.size() <= MaxBearerBytes;
}

bool ListenerPeerAllowed(std::string_view remoteAddr) { return remoteAddr == ListenerHost; }

bool ListenerBearerMatches(std::string_view authorization, std::string_view token)
{
    constexpr std::string_view scheme = "Bearer ";
    // The scheme and the length bound are public; only the secret bytes are compared in constant time.
    if (token.empty() || token.size() > MaxBearerBytes || authorization.size() < scheme.size() ||
        authorization.size() > scheme.size() + MaxBearerBytes || authorization.substr(0, scheme.size()) != scheme)
        return false;
    std::string_view const bearer = authorization.substr(scheme.size());
    std::size_t difference = token.size() ^ bearer.size();
    for (std::size_t i = 0; i < MaxBearerBytes; ++i)
        difference |= (i < token.size() ? static_cast<unsigned char>(token[i]) : 0) ^
                      (i < bearer.size() ? static_cast<unsigned char>(bearer[i]) : 0);
    return difference == 0;
}

bool ListenerRouteValid(std::string_view route)
{
    // The module's identifier rule; percent-encoding, '/' and '?' never pass it.
    return route.size() <= MaxRouteBytes && ValidIdentifier(std::string(route));
}

bool ListenerBodyShape(std::string_view body)
{
    std::size_t first = 0;
    std::size_t last = body.size();
    while (first < last && JsonSpace(body[first]))
        ++first;
    while (last > first && JsonSpace(body[last - 1]))
        --last;
    return last - first >= 2 && body[first] == '{' && body[last - 1] == '}';
}

int64_t ListenerWaitMs(std::string_view body, int64_t nowUnixMs)
{
    constexpr std::string_view key = "\"deadlineUnixMs\"";
    std::size_t i = body.find(key);
    if (i == std::string_view::npos)
        return ListenerWaitCapMs;
    i += key.size();
    while (i < body.size() && JsonSpace(body[i]))
        ++i;
    if (i >= body.size() || body[i] != ':')
        return ListenerWaitCapMs;
    ++i;
    while (i < body.size() && JsonSpace(body[i]))
        ++i;
    int64_t deadline = 0;
    std::size_t digits = 0;
    for (; i < body.size() && Digit(body[i]); ++i)
    {
        if (++digits > 16)  // Beyond MaxSafeInteger: not a deadline the mailbox would accept.
            return ListenerWaitCapMs;
        deadline = deadline * 10 + (body[i] - '0');
    }
    if (!digits)
        return ListenerWaitCapMs;
    return std::clamp<int64_t>(deadline - nowUnixMs, 0, ListenerWaitCapMs);
}

int ListenerStatus(ChannelError error)
{
    for (auto const& entry : StatusTable)
        if (entry.error == error)
            return entry.status;
    return 400;
}

struct ChannelListener::Run
{
    uint32_t port = 0;
    std::string token;  // Immutable for the run: a token change restarts the listener.
    std::atomic<bool> stopping{false};
    httplib::Server server;
    std::thread thread;
};

ChannelListener::ChannelListener(Receive receive, Take take, Log log)
    : _receive(std::move(receive)), _take(std::move(take)), _log(std::move(log))
{
}

ChannelListener::~ChannelListener()
{
    std::lock_guard<std::mutex> lock(_mutex);
    StopLocked(false);  // Static destruction: never call into a logger that may already be gone.
}

void ChannelListener::Write(bool warning, std::string const& line) const
{
    if (_log)
        _log(warning, line);
}

bool ChannelListener::Configure(bool enabled, uint32_t port, std::string token)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!enabled || !ListenerTokenValid(token))
    {
        StopLocked(true);
        return false;
    }
    if (!ListenerPortValid(port))
    {
        StopLocked(true);
        Write(true, "[Ollama Chat] OllamaChat.Bridge.ChannelPort must be 1024-65535; bridge channel listener off.");
        return false;
    }
    if (_run && _run->port == port && _run->token == token && _run->server.is_running())
        return true;
    StopLocked(true);
    return StartLocked(port, std::move(token));
}

void ChannelListener::Stop()
{
    std::lock_guard<std::mutex> lock(_mutex);
    StopLocked(true);
}

bool ChannelListener::Running() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _run && _run->server.is_running();
}

uint32_t ChannelListener::Port() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _run ? _run->port : 0;
}

uint64_t ChannelListener::Starts() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _starts;
}

ListenerStats ChannelListener::Stats() const { return {_accepted.load(), _unauthorized.load(), _timeouts.load()}; }

bool ChannelListener::StartLocked(uint32_t port, std::string token)
{
    auto run = std::make_unique<Run>();
    run->port = port;
    run->token = std::move(token);
    Run* const current = run.get();
    auto& server = run->server;
    server.set_address_family(AF_INET);
    server.set_tcp_nodelay(true);
    server.set_keep_alive_max_count(1);  // One exchange per connection; every reply says Connection: close.
    server.set_keep_alive_timeout(IoTimeoutSec);
    server.set_read_timeout(IoTimeoutSec, 0);
    server.set_write_timeout(IoTimeoutSec, 0);
    server.set_idle_interval(0, AcceptIdleUsec);
    server.set_payload_max_length(MaxRequestBytes);  // Second bound behind the Content-Length check below.
    server.new_task_queue = [] { return new httplib::ThreadPool(Workers, MaxQueued); };

    // Runs after the headers, before any body byte is read. Order: peer, bearer, path, method, size, type, route.
    auto admit = [this, current](httplib::Request const& req, httplib::Response& res)
    {
        if (!ListenerPeerAllowed(req.remote_addr))
        {
            Reply(res, 403, "forbidden");
            return false;
        }
        if (!ListenerBearerMatches(req.get_header_value("Authorization"), current->token))
        {
            ++_unauthorized;
            Reply(res, ChannelError::Unauthorized);
            return false;
        }
        std::string_view const target = req.target;
        if (target.substr(0, RoutePrefix.size()) != RoutePrefix)
        {
            Reply(res, 404, "not_found");
            return false;
        }
        if (req.method != "POST")
        {
            Reply(res, 405, "method_not_allowed");
            return false;
        }
        // Bytes are bounded before reading: one exact Content-Length within the mailbox limit. Chunked or
        // undeclared bodies cannot be bounded up front and are refused like oversize ones.
        std::string const length = req.get_header_value("Content-Length");
        bool const declared = !req.has_header("Transfer-Encoding") &&
                              req.get_header_value_count("Content-Length") == 1 && !length.empty() &&
                              length.size() <= 7 && std::all_of(length.begin(), length.end(), Digit);
        unsigned long const bytes = declared ? std::stoul(length) : 0;
        std::string_view const route = target.substr(RoutePrefix.size());
        if (!declared || bytes > MaxRequestBytes || route.size() > MaxRouteBytes)
        {
            Reply(res, ChannelError::TooLarge);
            return false;
        }
        if (!bytes || !JsonContentType(req.get_header_value("Content-Type")) || !ListenerRouteValid(route))
        {
            Reply(res, ChannelError::Invalid);
            return false;
        }
        return true;
    };
    server.set_pre_routing_handler(
        [admit](httplib::Request const& req, httplib::Response& res)
        {
            return admit(req, res) ? httplib::Server::HandlerResponse::Unhandled
                                   : httplib::Server::HandlerResponse::Handled;
        });
    server.set_expect_100_continue_handler([admit](httplib::Request const& req, httplib::Response& res)
                                           { return admit(req, res) ? 100 : res.status; });
    server.Post(R"(/v1/channel/[^/]+)",
                [this, current](httplib::Request const& req, httplib::Response& res)
                {
                    std::string_view const route = std::string_view(req.target).substr(RoutePrefix.size());
                    if (!ListenerBodyShape(req.body))
                    {
                        Reply(res, ChannelError::Invalid);
                        return;
                    }
                    if (current->stopping)
                    {
                        Reply(res, ChannelError::Inactive);
                        return;
                    }
                    auto const until = std::chrono::steady_clock::now() +
                                       std::chrono::milliseconds(ListenerWaitMs(req.body, UnixMs()));
                    uint64_t receipt = 0;
                    ChannelError const received = _receive(current->token, route, req.body, receipt);
                    if (received != ChannelError::None)
                    {
                        Reply(res, received);
                        return;
                    }
                    ++_accepted;
                    // Take is non-blocking: poll until the world tick publishes. A packet the world drops never
                    // appears and ends as a timeout. The last Take runs at or after the deadline.
                    std::unique_ptr<ChannelPacket> packet;
                    while (true)
                    {
                        ChannelError const taken = _take(current->token, receipt, packet);
                        if (taken == ChannelError::None && packet)
                            break;
                        if (taken != ChannelError::None && taken != ChannelError::Unknown)
                        {
                            Reply(res, taken);
                            return;
                        }
                        if (current->stopping)
                        {
                            Reply(res, ChannelError::Inactive);
                            return;
                        }
                        auto const now = std::chrono::steady_clock::now();
                        if (now >= until)
                        {
                            ++_timeouts;
                            Reply(res, 504, "timeout");
                            return;
                        }
                        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
                            std::chrono::milliseconds(ListenerPollMs), until - now));
                    }
                    if (packet->error != ChannelError::None || packet->body.empty())
                    {
                        Reply(res, packet->error == ChannelError::None ? ChannelError::Invalid : packet->error);
                        return;
                    }
                    // The response owns the packet, and with it the physical mailbox slot, until it is written.
                    std::shared_ptr<ChannelPacket> held(std::move(packet));
                    res.status = 200;
                    res.set_content_provider(held->body.size(), "application/json",
                                             [held](std::size_t offset, std::size_t length, httplib::DataSink& sink)
                                             { return sink.write(held->body.data() + offset, length); });
                });
    // httplib's own refusals (bad request line, URI too long, unreadable body) get the same fixed bodies.
    server.set_error_handler(
        [](httplib::Request const&, httplib::Response& res)
        {
            if (!res.body.empty() || res.content_provider_)
                return httplib::Server::HandlerResponse::Unhandled;
            switch (res.status)
            {
                case 404:
                    Reply(res, 404, "not_found");
                    break;
                case 405:
                    Reply(res, 405, "method_not_allowed");
                    break;
                case 413:
                case 414:
                    Reply(res, res.status, "too_large");
                    break;
                default:
                    Reply(res, res.status, res.status >= 500 ? "internal" : "invalid_input");
                    break;
            }
            return httplib::Server::HandlerResponse::Handled;
        });
    server.set_exception_handler([](httplib::Request const&, httplib::Response& res, std::exception_ptr)
                                 { Reply(res, 500, "internal"); });

    if (!server.bind_to_port(ListenerHost, static_cast<int>(port)))
    {
        Write(true, "[Ollama Chat] Bridge channel listener could not bind 127.0.0.1:" + std::to_string(port) +
                        "; listener off.");
        return false;
    }
    run->thread = std::thread([&server] { server.listen_after_bind(); });
    server.wait_until_ready();  // Stop() only closes a running accept loop; never return before it runs.
    _run = std::move(run);
    ++_starts;
    Write(false, "[Ollama Chat] Bridge channel listener on 127.0.0.1:" + std::to_string(port) + ".");
    return true;
}

void ChannelListener::StopLocked(bool log)
{
    if (!_run)
        return;
    _run->stopping = true;  // Waiting handlers answer inactive within one poll.
    _run->server.stop();
    if (_run->thread.joinable())
        _run->thread.join();  // listen_after_bind returns after its worker pool has joined.
    uint32_t const port = _run->port;
    _run.reset();
    if (log)
        Write(false, "[Ollama Chat] Bridge channel listener on 127.0.0.1:" + std::to_string(port) + " stopped.");
}
}  // namespace OllamaBridge
