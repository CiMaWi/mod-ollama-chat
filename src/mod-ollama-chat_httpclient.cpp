#include "mod-ollama-chat_httpclient.h"

#include "mod-ollama-chat_bridge.h"
#include "mod-ollama-chat_config.h"

// Rename the httplib namespace to avoid ODR/ABI collisions with other modules
// that bundle a different cpp-httplib version (e.g. mod-pbc 0.43.1, mod-ale
// 0.13.1) into the same monolithic `modules` static library.
#define httplib ollamachat_httplib
#include "./httplib.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include "Log.h"

#ifdef _WIN32
#include <AclAPI.h>
#pragma comment(lib, "advapi32.lib")
#endif

namespace
{
static_assert(std::string_view{CPPHTTPLIB_VERSION} == "0.23.1",
    "mod-ollama-chat requires its bundled cpp-httplib 0.23.1");

// Keep SIGPIPE protection local to this transport, independent of process signal handlers.
#if defined(MSG_NOSIGNAL)
constexpr int BridgeSendFlags = CPPHTTPLIB_SEND_FLAGS | MSG_NOSIGNAL;
#else
constexpr int BridgeSendFlags = CPPHTTPLIB_SEND_FLAGS;
#endif

// The injected option setter lets Windows tests exercise checked POSIX setup failure.
template <typename SetOption>
bool ConfigureBridgeSignalSafety(socket_t socket, bool sendIsSafe, int socketOption, SetOption setOption)
{
    if (sendIsSafe)
        return true;
    return socketOption != 0 && setOption(socket, SOL_SOCKET, socketOption, 1);
}

bool ConfigureBridgeSignalSafety(socket_t socket)
{
#if defined(_WIN64) || (defined(MSG_NOSIGNAL) && !defined(SO_NOSIGPIPE))
    constexpr bool sendIsSafe = true;
#else
    constexpr bool sendIsSafe = false;
#endif
#ifdef SO_NOSIGPIPE
    constexpr int socketOption = SO_NOSIGPIPE;
#else
    constexpr int socketOption = 0;
#endif
    return ConfigureBridgeSignalSafety(socket, sendIsSafe, socketOption,
                                       [](socket_t target, int level, int option, int value)
                                       { return httplib::detail::set_socket_opt(target, level, option, value); });
}

// Check the absolute deadline even for buffered input. Socket I/O is nonblocking:
// readiness is not a guarantee that a subsequent send can finish without waiting.
class BridgeDeadlineStream final : public httplib::Stream
{
public:
    BridgeDeadlineStream(socket_t socket, std::chrono::steady_clock::time_point started,
                         std::chrono::steady_clock::time_point expires)
        : _socket(socket), _started(started), _expires(expires)
    {
    }

    bool is_readable() const override { return Alive() && _offset < _size; }
    bool wait_readable() const override { return is_readable() || Wait(false); }
    bool wait_writable() const override { return Wait(true); }

    ssize_t read(char* data, std::size_t size) override
    {
        if (!Alive())
            return -1;
        if (_offset == _size)
        {
            ssize_t received;
            do
            {
                if (!Wait(false))
                    return -1;
                received = ::recv(_socket, _buffer.data(), static_cast<int>(_buffer.size()), CPPHTTPLIB_RECV_FLAGS);
            } while (received < 0 && Retry());
            if (received <= 0 || !Alive())
                return received <= 0 ? received : -1;
            _offset = 0;
            _size = static_cast<std::size_t>(received);
        }
        auto const count = std::min(size, _size - _offset);
        std::copy_n(_buffer.data() + _offset, count, data);
        _offset += count;
        return static_cast<ssize_t>(count);
    }

    ssize_t write(char const* data, std::size_t size) override
    {
        ssize_t sent;
        do
        {
            if (!Wait(true))
                return -1;
            auto const count = static_cast<int>(std::min<std::size_t>(size, 65536));
            sent = ::send(_socket, data, count, BridgeSendFlags);
        } while (sent < 0 && Retry());
        return Alive() ? sent : -1;
    }

    void get_remote_ip_and_port(std::string& ip, int& port) const override
    {
        httplib::detail::get_remote_ip_and_port(_socket, ip, port);
    }
    void get_local_ip_and_port(std::string& ip, int& port) const override
    {
        httplib::detail::get_local_ip_and_port(_socket, ip, port);
    }
    socket_t socket() const override { return _socket; }
    time_t duration() const override
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _started)
            .count();
    }

private:
    bool Alive() const { return std::chrono::steady_clock::now() < _expires; }

    bool Wait(bool write) const
    {
        auto const remaining = _expires - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero())
            return false;
        auto timeout = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        if (write)
            timeout = std::min<int64_t>(timeout, 1000);
        pollfd descriptor{};
        descriptor.fd = _socket;
        descriptor.events = write ? POLLOUT : POLLIN;
        // Fail closed on an interrupted poll; never restart a stale wait budget.
        return httplib::detail::poll_wrapper(&descriptor, 1, static_cast<int>(timeout)) > 0 && Alive();
    }

    static bool Retry()
    {
#ifdef _WIN64
        int const error = WSAGetLastError();
        return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
    }

    socket_t const _socket;
    std::chrono::steady_clock::time_point const _started;
    std::chrono::steady_clock::time_point const _expires;
    std::array<char, 4096> _buffer{};
    std::size_t _offset = 0;
    std::size_t _size = 0;
};

// One wait only: EINTR fails closed instead of renewing a relative timeout.
// Injected wait/clock arguments let the standalone harness prove this path.
template <typename Poll, typename Clock>
httplib::Error WaitForBridgeConnect(socket_t socket, std::chrono::steady_clock::time_point expires, Poll poll,
                                    Clock now)
{
    auto const remaining = expires - now();
    if (remaining <= std::chrono::steady_clock::duration::zero())
        return httplib::Error::ConnectionTimeout;
    pollfd descriptor{};
    descriptor.fd = socket;
    descriptor.events = POLLIN | POLLOUT;
    auto const timeout = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
    int const result = poll(&descriptor, 1, static_cast<int>(timeout));
    if (now() >= expires || result == 0)
        return httplib::Error::ConnectionTimeout;
    if (result < 0 || !(descriptor.revents & (POLLIN | POLLOUT)))
        return httplib::Error::Connection;
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0 || error != 0)
        return httplib::Error::Connection;
    return now() < expires ? httplib::Error::Success : httplib::Error::ConnectionTimeout;
}

// The supported stream hook keeps connected socket ownership/cleanup in httplib and all
// deadline enforcement on this worker. No cancellation thread survives Post.
class BridgeDeadlineClient final : public httplib::ClientImpl
{
public:
    BridgeDeadlineClient(std::string const& host, int port, std::chrono::steady_clock::time_point expires)
        : httplib::ClientImpl(host, port), _expires(expires)
    {
    }

protected:
    bool create_and_connect_socket(Socket& socket, httplib::Error& error) override
    {
        auto const started = std::chrono::steady_clock::now();
        auto const expires = std::min(_expires, started + std::chrono::milliseconds(250));
        error = httplib::Error::ConnectionTimeout;
        if (started >= expires)
            return false;
        error = httplib::Error::Connection;
        if (host_ != "127.0.0.1" || port_ <= 0 || port_ > 65535)
            return false;

        // No resolver or alternate address attempts: this transport is literal IPv4 loopback only.
#ifdef _WIN64
        socket_t owned =
            WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT | WSA_FLAG_OVERLAPPED);
#elif defined(SOCK_CLOEXEC)
        socket_t owned = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
#else
        socket_t owned = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
        if (owned == INVALID_SOCKET)
            return false;
        auto cleanup = httplib::detail::scope_exit(
            [&]
            {
                if (owned != INVALID_SOCKET)
                    httplib::detail::close_socket(owned);
            });
#ifdef _WIN64
        u_long nonblocking = 1;
        if (ioctlsocket(owned, FIONBIO, &nonblocking) != 0)
            return false;
#else
#ifndef SOCK_CLOEXEC
        if (fcntl(owned, F_SETFD, FD_CLOEXEC) == -1)
            return false;
#endif
        int const flags = fcntl(owned, F_GETFL, 0);
        if (flags == -1 || fcntl(owned, F_SETFL, flags | O_NONBLOCK) == -1)
            return false;
#endif
        if (tcp_nodelay_)
            httplib::detail::set_socket_opt(owned, IPPROTO_TCP, TCP_NODELAY, 1);
        if (socket_options_)
            socket_options_(owned);
        // Check after optional customization, before connect or any send. Failure
        // leaves the socket with the local guard; unsupported POSIX targets fail closed.
        if (!ConfigureBridgeSignalSafety(owned))
            return false;

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<uint16_t>(port_));
        error = httplib::Error::ConnectionTimeout;
        if (std::chrono::steady_clock::now() >= expires)
            return false;
        if (::connect(owned, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        {
            error = httplib::Error::Connection;
            if (httplib::detail::is_connection_error())
                return false;
            error = WaitForBridgeConnect(owned, expires, httplib::detail::poll_wrapper, std::chrono::steady_clock::now);
            if (error != httplib::Error::Success)
                return false;
        }
        error = httplib::Error::ConnectionTimeout;
        if (std::chrono::steady_clock::now() >= expires)
            return false;
        socket.sock = owned;
        owned = INVALID_SOCKET;
        error = httplib::Error::Success;
        return true;
    }

    bool process_socket(Socket const& socket, std::chrono::steady_clock::time_point started,
                        std::function<bool(httplib::Stream&)> callback) override
    {
#ifdef _WIN64
        u_long nonblocking = 1;
        if (ioctlsocket(socket.sock, FIONBIO, &nonblocking) != 0)
            return false;
#else
        int const flags = fcntl(socket.sock, F_GETFL, 0);
        if (flags == -1 || fcntl(socket.sock, F_SETFL, flags | O_NONBLOCK) == -1)
            return false;
#endif
        BridgeDeadlineStream stream(socket.sock, started, _expires);
        return callback(stream);
    }

private:
    std::chrono::steady_clock::time_point const _expires;
};

    struct ParsedUrl
    {
        bool        valid = false;
        bool        https = false;
        std::string host;
        int         port = 0;
        std::string path;
    };

    ParsedUrl ParseUrl(const std::string& url)
    {
        ParsedUrl out;

        static const std::regex urlRegex(R"(^(https?)://([^:/?#]+)(?::(\d+))?([/?#].*)?$)");
        std::smatch match;
        if (!std::regex_match(url, match, urlRegex))
            return out;

        const std::string protocol = match[1].str();
        out.https = (protocol == "https");
        out.host  = match[2].str();

        if (match[3].matched)
        {
            out.port = std::stoi(match[3].str());
        }
        else
        {
            // No explicit port. Use the scheme default -- NOT Ollama's 11434,
            // which silently misrouted every reverse-proxied deployment on :80.
            out.port = out.https ? 443 : 80;
        }

        out.path  = match[4].matched ? match[4].str() : "/";
        out.valid = true;
        return out;
    }

    httplib::Headers BuildHeaders(const std::string& host, const OllamaHttpHeaders& extra)
    {
        httplib::Headers headers = {
            { "Content-Type", "application/json" },
            { "User-Agent",   "AzerothCore-OllamaChat/2.0" },
            { "Accept",       "application/json" },
        };

        if (host.find("ngrok") != std::string::npos)
            headers.emplace("ngrok-skip-browser-warning", "true");

        for (const auto& kv : extra)
            headers.emplace(kv.first, kv.second);

        return headers;
    }

    // One connection per worker thread per endpoint. A single shared client
    // would keep-alive correctly but cpp-httplib serialises requests on it,
    // which would collapse MaxConcurrentQueries back down to 1.
    struct ClientCache
    {
        std::unordered_map<std::string, std::unique_ptr<httplib::Client>> plain;
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        std::unordered_map<std::string, std::unique_ptr<httplib::SSLClient>> ssl;
#endif
        int timeout = -1;
    };

    thread_local ClientCache t_clients;

    void ApplyTimeout(httplib::Client& c, int timeout)
    {
        c.set_connection_timeout(timeout);
        c.set_read_timeout(timeout);
        c.set_write_timeout(timeout);
        c.set_keep_alive(true);
    }

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    void ApplyTimeout(httplib::SSLClient& c, int timeout)
    {
        c.set_connection_timeout(timeout);
        c.set_read_timeout(timeout);
        c.set_write_timeout(timeout);
        c.set_keep_alive(true);
    }
#endif

    // Drop pooled sockets when the configured timeout changes.
    void InvalidateIfTimeoutChanged(int timeout)
    {
        if (t_clients.timeout == timeout)
            return;
        t_clients.plain.clear();
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        t_clients.ssl.clear();
#endif
        t_clients.timeout = timeout;
    }

    OllamaHttpResult Perform(const ParsedUrl& u, int timeout,
                             const std::string& jsonData, bool isPost,
                             const OllamaHttpHeaders& extraHeaders, bool verifyCerts)
    {
        OllamaHttpResult result;
        // Verification is a property of the SSL context, so a client built
        // one way must not be reused the other way.
        const std::string key = u.host + ":" + std::to_string(u.port) + (verifyCerts ? "+verify" : "");

        InvalidateIfTimeoutChanged(timeout);

        httplib::Result response(nullptr, httplib::Error::Unknown);
        const httplib::Headers headers = BuildHeaders(u.host, extraHeaders);

        if (u.https)
        {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
            auto& slot = t_clients.ssl[key];
            if (!slot)
            {
                slot = std::make_unique<httplib::SSLClient>(u.host, u.port);
                // With verification on, cpp-httplib loads the Windows ROOT
                // store (crypt32 is linked by mod-ollama-chat.cmake) or the
                // OpenSSL default paths elsewhere.
                slot->enable_server_certificate_verification(verifyCerts);
                ApplyTimeout(*slot, timeout);
            }
            response = isPost ? slot->Post(u.path, headers, jsonData, "application/json")
                              : slot->Get(u.path, headers);
#else
            result.error = "HTTPS requested but the module was built without OpenSSL support.";
            return result;
#endif
        }
        else
        {
            auto& slot = t_clients.plain[key];
            if (!slot)
            {
                slot = std::make_unique<httplib::Client>(u.host, u.port);
                ApplyTimeout(*slot, timeout);
            }
            response = isPost ? slot->Post(u.path, headers, jsonData, "application/json")
                              : slot->Get(u.path, headers);
        }

        if (!response)
        {
            result.error = httplib::to_string(response.error());

            // A dead keep-alive socket surfaces as a transport error; drop the
            // cached client so the next attempt reconnects cleanly.
            if (u.https)
            {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
                t_clients.ssl.erase(key);
#endif
            }
            else
            {
                t_clients.plain.erase(key);
            }
            return result;
        }

        result.status = response->status;
        result.body   = response->body;

        // Hosted providers say how long to back off on 429/503. Only the
        // delta-seconds form is honoured; an HTTP-date is rare and the caller
        // falls back to its own schedule.
        if (response->has_header("Retry-After"))
        {
            try
            {
                result.retryAfterSeconds = std::stoi(response->get_header_value("Retry-After"));
                if (result.retryAfterSeconds < 0)
                    result.retryAfterSeconds = 0;
            }
            catch (const std::exception&)
            {
                result.retryAfterSeconds = 0;
            }
        }

        return result;
    }
}

namespace
{
// The only callers choose a literal route below, never a route supplied by configuration/input.
OllamaHttpResult PostLocalBridge(std::string const& baseUrl, char const* path, std::string const& jsonData,
                                 OllamaHttpHeaders const& headers, int64_t remainingMs)
{
    OllamaHttpResult result;
    result.error = "bridge_invalid_transport_input";
    if (!OllamaBridge::ValidEndpoint(baseUrl) || jsonData.empty() || jsonData.size() > OllamaBridge::MaxRequestBytes ||
        remainingMs <= 0 || remainingMs > OllamaBridge::MaxLifetimeMs || headers.size() != 1 ||
        headers.front().first != "Authorization" || headers.front().second.compare(0, 7, "Bearer ") != 0)
        return result;

    OllamaBridge::Settings const link = {"link", "link", baseUrl, headers.front().second.substr(7)};
    if (!OllamaBridge::ValidSettings(link))
        return result;

    try
    {
        auto const started = std::chrono::steady_clock::now();
        auto const expires = started + std::chrono::milliseconds(remainingMs);
        ParsedUrl const parsed = ParseUrl(baseUrl + path);
        if (!parsed.valid || parsed.https || parsed.host != "127.0.0.1")
            return result;

        BridgeDeadlineClient client(parsed.host, parsed.port, expires);
        client.set_follow_location(false);
        client.set_keep_alive(false);
        client.set_decompress(false);

        bool oversized = false;
        bool expired = false;
        auto receiver = [&](char const* data, std::size_t length)
        {
            expired = std::chrono::steady_clock::now() >= expires;
            oversized = length > OllamaBridge::MaxResponseBytes - result.body.size();
            if (expired || oversized)
                return false;
            result.body.append(data, length);
            return true;
        };
        auto const response =
            client.Post(parsed.path, BuildHeaders(parsed.host, headers), jsonData, "application/json", receiver);
        expired = expired || std::chrono::steady_clock::now() >= expires;
        if (!response || expired || oversized)
        {
            result.body.clear();
            result.error = oversized ? "bridge_response_too_large"
                           : expired ? "bridge_transport_timeout"
                                     : "bridge_transport_unavailable";
            return result;
        }
        result.status = response->status;
        result.error.clear();
    }
    catch (std::exception const&)
    {
        result.body.clear();
        result.error = "bridge_transport_unavailable";
    }
    return result;
}

#ifdef _WIN32
struct ObserverHandle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~ObserverHandle()
    {
        if (value != INVALID_HANDLE_VALUE && value != nullptr)
            CloseHandle(value);
    }
};

// Validate permissions on the same handle used for reading, not a second path lookup.
bool ObserverOwnerOnly(HANDLE handle)
{
    ObserverHandle processToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &processToken.value))
        return false;
    DWORD bytes = 0;
    GetTokenInformation(processToken.value, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> user(bytes);
    if (bytes == 0 || !GetTokenInformation(processToken.value, TokenUser, user.data(), bytes, &bytes))
        return false;
    auto const sid = reinterpret_cast<TOKEN_USER const*>(user.data())->User.Sid;
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
                        &acl, nullptr, &descriptor) != ERROR_SUCCESS)
        return false;
    std::unique_ptr<void, decltype(&LocalFree)> security(descriptor, LocalFree);
    if (!owner || !EqualSid(owner, sid) || !acl)
        return false;
    for (DWORD index = 0; index < acl->AceCount; ++index)
    {
        void* entry = nullptr;
        if (!GetAce(acl, index, &entry))
            return false;
        auto const* ace = static_cast<ACCESS_ALLOWED_ACE const*>(entry);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || !EqualSid(const_cast<DWORD*>(&ace->SidStart), sid))
            return false;
    }
    return true;
}

HANDLE OpenObserverComponent(std::filesystem::path const& path, bool directory)
{
    return CreateFileW(path.c_str(), READ_CONTROL | (directory ? FILE_LIST_DIRECTORY : GENERIC_READ), FILE_SHARE_READ,
                       nullptr, OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
}

// The injectable opener observes actual lookups in the standalone production-helper test.
template <typename OpenFile>
bool ReadObserverToken(std::filesystem::path const& path, std::string& data, OpenFile openFile)
{
    // Open/check each component before any descendant lookup. With no write/delete sharing, each retained
    // handle prevents that ancestor from being replaced, renamed or changed into a reparse point until read ends.
    // FILE_LIST_DIRECTORY is needed: metadata-only handles do not enforce these sharing restrictions.
    std::vector<std::unique_ptr<ObserverHandle>> directories;
    auto cursor = path.root_path();
    auto pinDirectory = [&]()
    {
        auto directory = std::make_unique<ObserverHandle>();
        directory->value = openFile(cursor, true);
        BY_HANDLE_FILE_INFORMATION info{};
        if (directory->value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(directory->value, &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
        directories.push_back(std::move(directory));
        return true;
    };
    if (!pinDirectory())
        return false;
    for (auto const& component : path.parent_path().relative_path())
    {
        if (component == "." || component == ".." || component.native().find(L':') != std::wstring::npos)
            return false;
        cursor /= component;
        if (!pinDirectory())
            return false;
    }
    if (!ObserverOwnerOnly(directories.back()->value))
        return false;
    cursor /= path.filename();
    ObserverHandle file{openFile(cursor, false)};
    BY_HANDLE_FILE_INFORMATION info{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nNumberOfLinks != 1 || info.nFileSizeHigh != 0 || info.nFileSizeLow == 0 || info.nFileSizeLow > 65536 ||
        !ObserverOwnerOnly(file.value))
        return false;
    data.resize(info.nFileSizeLow);
    DWORD read = 0;
    return ReadFile(file.value, data.data(), info.nFileSizeLow, &read, nullptr) && read == info.nFileSizeLow;
}

bool ReadObserverToken(std::filesystem::path const& path, std::string& data)
{
    return ReadObserverToken(path, data, OpenObserverComponent);
}

#endif
}  // namespace

bool OllamaBridge::ValidObserverSettings(Settings const& settings)
{
    return settings.observerEnabled && settings.provider == "codex" && ValidSettings(settings) &&
           !settings.observerTokenRef.empty() && settings.observerToken != settings.token &&
           settings.observerToken.size() == 64 &&
           std::all_of(settings.observerToken.begin(), settings.observerToken.end(),
                       [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool OllamaBridge::LoadObserverToken(Settings& settings)
{
    settings.observerToken.clear();
    if (!settings.observerEnabled || settings.provider != "codex" || !ValidSettings(settings) ||
        settings.observerTokenRef.find('\0') != std::string::npos)
        return false;
    try
    {
        std::filesystem::path const path{
            std::u8string(settings.observerTokenRef.begin(), settings.observerTokenRef.end())};
        if (!path.is_absolute() || path.filename() != "observer.json")
            return false;
        std::string data;
#ifdef _WIN32
        // A credential reference cannot initiate SMB/network or removable-drive I/O.
        auto const drive = path.root_name().native();
        if (drive.size() != 2 || drive[1] != L':' || GetDriveTypeW(path.root_path().c_str()) != DRIVE_FIXED ||
            !ReadObserverToken(path, data))
            return false;
#else
        // The reviewed native service requires Windows; no weaker file-policy fallback.
        return false;
#endif
        std::set<std::string> keys;
        bool duplicate = false;
        auto const document = nlohmann::json::parse(
            data,
            [&](int, nlohmann::json::parse_event_t event, nlohmann::json& value)
            {
                if (event == nlohmann::json::parse_event_t::key && !keys.insert(value.get<std::string>()).second)
                    duplicate = true;
                return true;
            });
        if (duplicate || !document.is_object() || document.size() != 2 || !document.contains("version") ||
            !document["version"].is_number_integer() || document["version"] != 1 || !document.contains("token") ||
            !document["token"].is_string())
            return false;
        settings.observerToken = document["token"].get<std::string>();
        if (ValidObserverSettings(settings))
            return true;
    }
    catch (std::exception const&)
    {
    }
    settings.observerToken.clear();
    return false;
}

OllamaHttpResult OllamaHttp_PostBridge(std::string const& baseUrl, std::string const& jsonData,
                                       OllamaHttpHeaders const& headers, int64_t remainingMs)
{
    return PostLocalBridge(baseUrl, "/v1/generate", jsonData, headers, remainingMs);
}

OllamaHttpResult OllamaHttp_PostObserver(OllamaBridge::Settings const& settings, OllamaObserverOperation operation,
                                         std::string const& jsonData, int64_t remainingMs)
{
    char const* path = nullptr;
    switch (operation)
    {
        case OllamaObserverOperation::Open:
            path = "/v1/observer/open";
            break;
        case OllamaObserverOperation::Snapshot:
            path = "/v1/observer/snapshot";
            break;
    }
    if (!path || !OllamaBridge::ValidObserverSettings(settings) || remainingMs <= 0 || remainingMs > 5000)
        return {0, {}, "bridge_invalid_observer_input"};
    return PostLocalBridge(settings.endpoint, path, jsonData, {{"Authorization", "Bearer " + settings.observerToken}},
                           remainingMs);
}

bool OllamaHttp_TlsAvailable()
{
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    return true;
#else
    return false;
#endif
}

// --------------------------------------------------------------------------

std::string OllamaDeriveBaseUrl(const std::string& url)
{
    ParsedUrl u = ParseUrl(url);
    if (!u.valid)
        return url;

    std::ostringstream oss;
    oss << (u.https ? "https://" : "http://") << u.host;

    const int defaultPort = u.https ? 443 : 80;
    if (u.port != defaultPort)
        oss << ":" << u.port;

    return oss.str();
}

OllamaHttpClient::OllamaHttpClient()
    : m_timeout(120), m_available(true)
{
}

OllamaHttpClient::~OllamaHttpClient() = default;

void OllamaHttpClient::SetTimeout(int seconds)
{
    m_timeout = seconds;
}

bool OllamaHttpClient::IsAvailable() const
{
    return m_available;
}

OllamaHttpResult OllamaHttpClient::PostEx(const std::string& url, const std::string& jsonData,
                                          int timeoutOverride,
                                          const OllamaHttpHeaders& extraHeaders,
                                          bool verifyCerts)
{
    OllamaHttpResult result;

    try
    {
        ParsedUrl u = ParseUrl(url);
        if (!u.valid)
        {
            result.error = "Invalid URL format: " + url;
            LOG_ERROR("module.ollamachat", "[Ollama Chat] {}", result.error);
            return result;
        }

        const int timeout = timeoutOverride > 0
                                ? timeoutOverride
                                : (g_HttpTimeoutSeconds > 0
                                       ? static_cast<int>(g_HttpTimeoutSeconds)
                                       : m_timeout);

        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] POST {}:{}{}", u.host, u.port, u.path);

        result = Perform(u, timeout, jsonData, true, extraHeaders, verifyCerts);

        if (!result.error.empty())
            LOG_ERROR("module.ollamachat", "[Ollama Chat] HTTP POST to {}:{}{} failed: {}",
                      u.host, u.port, u.path, result.error);
        else if (!result.ok() && g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] HTTP {} from {}{} body: {}",
                     result.status, u.host, u.path, result.body);
    }
    catch (const std::exception& e)
    {
        result.error = e.what();
        LOG_ERROR("module.ollamachat", "[Ollama Chat] HTTP client exception: {}", e.what());
    }

    return result;
}

OllamaHttpResult OllamaHttpClient::GetEx(const std::string& url, int timeoutOverride)
{
    OllamaHttpResult result;

    try
    {
        ParsedUrl u = ParseUrl(url);
        if (!u.valid)
        {
            result.error = "Invalid URL format: " + url;
            return result;
        }

        const int timeout = timeoutOverride > 0
                                ? timeoutOverride
                                : (g_HttpTimeoutSeconds > 0
                                       ? static_cast<int>(g_HttpTimeoutSeconds)
                                       : m_timeout);

        result = Perform(u, timeout, std::string(), false, {}, false);
    }
    catch (const std::exception& e)
    {
        result.error = e.what();
    }

    return result;
}

std::string OllamaHttpClient::Post(const std::string& url, const std::string& jsonData)
{
    OllamaHttpResult r = PostEx(url, jsonData);
    return r.ok() ? r.body : std::string();
}
