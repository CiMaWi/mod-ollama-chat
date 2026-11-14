// Server-free game-boundary proof: production helper plus a short synthetic local HTTP peer.
// No worldserver, game objects, native provider, authority or delivery proof is involved.
#include "mod-ollama-chat_bridge.h"
#define httplib ollamachat_httplib
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>
#include <vector>

#include "../src/httplib.h"

#ifndef _WIN64
#include <signal.h>
#include <sys/wait.h>
#endif

// Include the production translation unit once so the standalone harness can
// exercise its private deadline transport with blocked sends and injected connect waits.
// The regular build compiles this source normally; no test API is exported.
#include "../src/mod-ollama-chat_httpclient.cpp"

// Standalone HTTP compilation supplies declarations via its forced-include fixture.
uint32_t g_HttpTimeoutSeconds = 0;
bool g_DebugEnabled = true;

namespace
{
unsigned checks = 0;

void Check(bool condition, char const* label)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(label);
}

OllamaBridge::Settings Settings()
{
    return {"fixture-main", "fixture-a", "http://127.0.0.1:47800", "synthetic-not-a-real-link-token"};
}

OllamaBridge::Request Request()
{
    OllamaBridge::Request request;
    request.epoch = "fixture-epoch-a";
    request.requestId = "generation-1";
    request.deadline = OllamaBridge::CaptureDeadline({1000000, 10000}, 10000);
    request.controllerGuid = 9007199254740993ULL;
    request.botGuid = std::numeric_limits<uint64_t>::max();
    request.input.speakerGuid = request.controllerGuid;
    request.input.channel = "party";
    request.input.text = "Stay by me.\n\"quoted\"\\";
    request.input.systemPrompt = "World rules\n";
    request.input.persona = "A \"steady\" companion";
    request.input.memory = "We met by the inn.\\";
    request.input.groupScope = "party:9007199254740993";
    request.input.speakerControlsBot = true;
    request.input.directAddress = true;
    request.input.observations = {"A visible mark: star", "Someone said \"wait\"."};
    request.input.party = {{request.botGuid, "Fixture \"Bot\"", "healer", 80, request.controllerGuid}};
    return request;
}

nlohmann::json Reply(OllamaBridge::Settings const& settings, OllamaBridge::Request const& request)
{
    return {{"version", 1},
            {"instance", settings.instance},
            {"provider", settings.provider},
            {"epoch", request.epoch},
            {"requestId", request.requestId},
            {"deadlineUnixMs", request.deadline.unixMs},
            {"type", "generation.result"},
            {"text", "I shall wait here."}};
}

void ObserverSettingsChecks(std::string const& directory)
{
    using namespace OllamaBridge;
    auto settings = Settings();
    settings.provider = "codex";
    settings.observerTokenRef = directory + "/valid/observer.json";
    auto write = [&](std::string const& document)
    {
        std::ofstream file(settings.observerTokenRef, std::ios::binary | std::ios::trunc);
        file << document;
        Check(static_cast<bool>(file), "write disposable observer credential");
    };
    std::string const secret(64, 'a');
    auto const document = nlohmann::json{{"version", 1}, {"token", secret}}.dump();
    write(document);
    Check(!LoadObserverToken(settings) && settings.observerToken.empty(), "observer defaults disabled");
    settings.observerEnabled = true;
    Check(LoadObserverToken(settings) && ValidObserverSettings(settings), "protected exact credential loaded");
    auto const captured = settings;
    write("{\"version\":1,\"version\":1,\"token\":\"" + secret + "\"}");
    Check(!LoadObserverToken(settings), "duplicate otherwise-valid credential rejected");
    write(nlohmann::json{{"version", true}, {"token", secret}}.dump());
    Check(!LoadObserverToken(settings), "boolean credential version rejected");
    write(document + std::string(65536 - document.size(), ' '));
    Check(LoadObserverToken(settings), "credential file exact byte cap accepted");
    write(document + std::string(1, static_cast<char>(0xff)));
    Check(!LoadObserverToken(settings), "invalid credential UTF-8 rejected");
    for (auto const& bad :
         {"{}", "{\"version\":true,\"token\":\"x\"}", "{\"version\":1,\"version\":1,\"token\":\"x\"}"})
    {
        write(bad);
        Check(!LoadObserverToken(settings) && settings.observerToken.empty(), "malformed credential clears cache");
    }
    for (auto const& bad :
         {std::string(63, 'a'), std::string(65, 'a'), std::string(64, 'A'), std::string(63, 'a') + "\n"})
    {
        write(nlohmann::json{{"version", 1}, {"token", bad}}.dump());
        Check(!LoadObserverToken(settings) && settings.observerToken.empty(), "exact lowercase token bound");
    }
    write(nlohmann::json{{"version", 1}, {"token", secret}, {"extra", true}}.dump());
    Check(!LoadObserverToken(settings), "unknown credential field rejected");
    write(std::string(65537, 'x'));
    Check(!LoadObserverToken(settings), "credential file byte cap");
    write(document);
    settings.token = secret;
    Check(!LoadObserverToken(settings) && settings.observerToken.empty(), "generation token cannot observe");
    settings = captured;
    settings.provider = "claude";
    Check(!LoadObserverToken(settings), "observer limited to Codex");
    for (auto const& reference :
         std::vector<std::string>{"observer.json", "C:observer.json", "\\\\127.0.0.1\\refuse\\observer.json",
                                  directory + "/valid/missing.json", directory + "/missing/observer.json",
                                  directory + "/unsafe/observer.json", directory + "/unsafe-file/observer.json",
                                  directory + "/alias/observer.json"})
    {
        settings = captured;
        settings.observerTokenRef = reference;
        Check(!LoadObserverToken(settings) && settings.observerToken.empty(), "unsafe credential reference refused");
    }
#ifdef _WIN32
    std::string loaded;
    std::vector<std::filesystem::path> attempted;
    auto recordOpen = [&](std::filesystem::path const& path, bool isDirectory)
    {
        attempted.push_back(path);
        return OpenObserverComponent(path, isDirectory);
    };
    auto const alias = std::filesystem::path(directory) / "alias";
    Check(!ReadObserverToken(alias / "observer.json", loaded, recordOpen), "reparse ancestor refused by real resolver");
    Check(!attempted.empty() && attempted.back() == alias,
          "unsafe ancestor stops actual opens before any descendant lookup");
    bool rootFirst = !attempted.empty() && attempted.front() == alias.root_path();
    for (std::size_t index = 1; index < attempted.size(); ++index)
        rootFirst = rootFirst && attempted[index].parent_path() == attempted[index - 1];
    Check(rootFirst, "actual component opens are ordered from root toward leaf");
    bool checkedPins = false;
    auto checkPinnedOpen = [&](std::filesystem::path const& path, bool isDirectory)
    {
        auto const handle = OpenObserverComponent(path, isDirectory);
        if (!isDirectory && handle != INVALID_HANDLE_VALUE)
        {
            checkedPins = true;
            for (auto ancestor = path.parent_path(); ancestor.filename() == "valid" || ancestor.filename() == "fixture";
                 ancestor = ancestor.parent_path())
            {
                auto renamed = ancestor;
                renamed += ".pin-test";
                bool const moved = MoveFileExW(ancestor.c_str(), renamed.c_str(), 0);
                DWORD const moveError = GetLastError();
                if (moved)
                    Check(MoveFileExW(renamed.c_str(), ancestor.c_str(), 0), "restore unexpected fixture rename");
                Check(!moved && moveError == ERROR_SHARING_VIOLATION, "all pinned ancestors deny rename at file open");
                ObserverHandle writer{CreateFileW(ancestor.c_str(), GENERIC_WRITE,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
                Check(writer.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
                      "all pinned ancestors deny reparse writer at file open");
            }
        }
        return handle;
    };
    Check(ReadObserverToken(std::filesystem::path(captured.observerTokenRef), loaded, checkPinnedOpen) &&
              loaded == document && checkedPins,
          "validated ancestry stays pinned through same-handle file read");
    settings = captured;
    settings.observerTokenRef = directory + "/alias/../valid/observer.json";
    Check(!LoadObserverToken(settings), "parent traversal cannot bypass unsafe ancestor");
    settings.observerTokenRef = directory + "/valid/../valid/observer.json";
    Check(!LoadObserverToken(settings), "parent traversal is refused without escaping pinned ancestry");
#endif
    auto const link = std::filesystem::path(directory) / "valid" / "linked.json";
    std::filesystem::create_hard_link(captured.observerTokenRef, link);
    settings = captured;
    Check(!LoadObserverToken(settings), "multiply linked credential refused");
    std::filesystem::remove(link);
    settings = captured;
    Check(LoadObserverToken(settings) && captured.observerToken == secret,
          "owned captured credential survives mutable settings refusal");
}

void BoundaryChecks()
{
    using namespace OllamaBridge;
    auto settings = Settings();
    auto request = Request();
    Clock const now{1000000, 10000};
    Check(ValidSettings(settings), "synthetic settings accepted");
    for (auto const* bad : {"http://localhost:47800", "https://127.0.0.1", "http://127.0.0.1/", "http://127.0.0.1:0",
                            "http://127.0.0.1:65536", "http://127.0.0.1:80?q=x", "http://127.0.0.1@elsewhere",
                            "http://127.0.0.10", "http://127.0.0.1:1/path"})
        Check(!ValidEndpoint(bad), "endpoint rejected");
    Check(ValidEndpoint("http://127.0.0.1") && ValidEndpoint("http://127.0.0.1:65535"), "endpoint limits");
    Check(!ValidIdentifier("_bad") && !ValidIdentifier(std::string(129, 'x')), "identifier limits");
    auto const headers = Headers(settings);
    Check(headers.size() == 1 && headers[0].first == "Authorization" && headers[0].second == "Bearer " + settings.token,
          "local bearer header");
    settings.token += "\r\nInjected: value";
    Check(Headers(settings).empty(), "header injection rejected");
    settings = Settings();
    settings.token.clear();
    Check(!ValidSettings(settings) && Headers(settings).empty(), "missing token rejected");
    settings = Settings();

    std::string body;
    Check(Build(settings, request, now, body), "generation build");
    auto const wire = nlohmann::json::parse(body);
    Check(wire["controllerGuid"] == "9007199254740993" && wire["botGuid"] == "18446744073709551615",
          "uint64 GUID strings");
    Check(wire["input"]["text"] == request.input.text && wire["input"]["persona"] == request.input.persona &&
              wire["input"]["memory"] == request.input.memory &&
              wire["input"]["party"][0]["name"] == request.input.party[0].name &&
              wire["input"]["observations"] == request.input.observations,
          "escaped owned context");
    Check(body.find(settings.token) == std::string::npos, "token absent from JSON");
    auto const savedSettings = settings;
    auto const savedRequest = request;
    settings.endpoint = "http://127.0.0.1:9";
    request.input.text = "reload changed original values";
    std::string snapshot;
    Check(Build(savedSettings, savedRequest, now, snapshot) && snapshot == body, "owned snapshot immutable");
    settings = Settings();
    request = Request();

    auto reply = Reply(settings, request);
    Check(Parse(settings, request, reply.dump(), now).ok, "valid result");
    for (auto const* key : {"instance", "provider", "epoch", "requestId", "type"})
    {
        auto wrong = reply;
        wrong[key] = "wrong";
        Check(!Parse(settings, request, wrong.dump(), now).ok, "wrong response identity or type");
        wrong.erase(key);
        Check(!Parse(settings, request, wrong.dump(), now).ok, "missing response identity or type");
    }
    auto wrong = reply;
    wrong["version"] = 1.0;
    Check(!Parse(settings, request, wrong.dump(), now).ok, "float version rejected");
    wrong = reply;
    wrong["deadlineUnixMs"] = request.deadline.unixMs + 1;
    Check(!Parse(settings, request, wrong.dump(), now).ok, "extended deadline rejected");
    wrong["deadlineUnixMs"] = std::to_string(request.deadline.unixMs);
    Check(!Parse(settings, request, wrong.dump(), now).ok, "string deadline rejected");
    wrong = reply;
    wrong["commands"] = nlohmann::json::array();
    Check(!Parse(settings, request, wrong.dump(), now).ok, "unexpected commands rejected");
    wrong = reply;
    wrong["type"] = "command";
    Check(!Parse(settings, request, wrong.dump(), now).ok, "command response rejected");
    wrong = reply;
    wrong.erase("text");
    wrong["type"] = "failure";
    wrong["code"] = "unavailable";
    Check(Parse(settings, request, wrong.dump(), now).error == "bridge_terminal_failure", "terminal failure");
    for (auto const& text : {std::string(), std::string(" \n\t"), std::string(4097, 'x'), std::string("a\0b", 3)})
    {
        wrong = reply;
        wrong["text"] = text;
        Check(!Parse(settings, request, wrong.dump(), now).ok, "invalid response text");
    }
    wrong = reply;
    wrong["text"] = std::string(4096, 'x');
    Check(Parse(settings, request, wrong.dump(), now).ok, "exact text cap");
    // DS.14 (b): a generation result carries exactly one of text and "silent": true, in the same eight keys.
    auto const spoken = Parse(settings, request, reply.dump(), now);
    Check(spoken.ok && !spoken.silent && spoken.text == "I shall wait here.", "DS.14: a text answer is not silent");
    auto silentReply = reply;
    silentReply.erase("text");
    silentReply["silent"] = true;
    auto const silent = Parse(settings, request, silentReply.dump(), now);
    Check(silent.ok && silent.silent && silent.text.empty() && silent.error.empty() && !silent.personaProposal,
          "DS.14: an action-only answer parsed as silent");
    for (auto const& value : {nlohmann::json(false), nlohmann::json(1), nlohmann::json("true"), nlohmann::json()})
    {
        wrong = silentReply;
        wrong["silent"] = value;
        auto const refused = Parse(settings, request, wrong.dump(), now);
        Check(!refused.ok && !refused.silent && refused.error == "bridge_invalid_response",
              "DS.14: silent only as true");
    }
    wrong = reply;
    wrong["silent"] = true;
    Check(!Parse(settings, request, wrong.dump(), now).ok, "DS.14: text and silent together malformed");
    wrong = reply;
    wrong.erase("text");
    Check(!Parse(settings, request, wrong.dump(), now).ok, "DS.14: neither text nor silent malformed");
    wrong = silentReply;
    wrong["mood"] = "calm";
    Check(!Parse(settings, request, wrong.dump(), now).ok, "DS.14: a silent answer with an extra key malformed");
    wrong = silentReply;
    wrong.erase("silent");
    wrong["silent"] = true;
    wrong["type"] = "failure";
    Check(!Parse(settings, request, wrong.dump(), now).ok, "DS.14: silent only on a generation result");
    Check(!Parse(settings, request, "{broken" + settings.token, now).ok, "malformed reply");
    Check(Parse(settings, request, "{broken" + settings.token, now).error.find(settings.token) == std::string::npos,
          "fixed parse error");
    Check(!Parse(settings, request, std::string(MaxResponseBytes + 1, 'x'), now).ok, "response cap before parse");
    std::string duplicate = reply.dump();
    duplicate.insert(1, "\"version\":1,");
    Check(!Parse(settings, request, duplicate, now).ok, "duplicate response key");
    std::string const nested = std::string(32, '[') + "0" + std::string(32, ']');
    Check(!Parse(settings, request, nested, now).ok, "depth bound");
    Check(!Parse(settings, request, reply.dump(), {1009999, 20000}).ok, "monotonic exact expiry");
    Check(!Parse(settings, request, reply.dump(), {1010000, 19999}).ok, "UTC exact expiry");
    Check(!Parse(settings, request, reply.dump(), {900000, 20000}).ok, "UTC rollback cannot extend");
    Check(Parse(settings, request, reply.dump(), {900000, 19999}).ok, "rollback keeps monotonic remainder");
    Check(CaptureDeadline(now, MaxLifetimeMs + 1).unixMs == 0 &&
              CaptureDeadline({std::numeric_limits<int64_t>::max(), 1}, 1).unixMs == 0,
          "deadline limits");

    request.input.text = std::string(4097, 'x');
    Check(!Build(settings, request, now, body) && body.empty(), "request text cap");
    request = Request();
    request.controllerGuid = 0;
    Check(!Build(settings, request, now, body), "no fabricated controller");
    request = Request();
    request.input.text = std::string(1, '\xff');
    Check(!Build(settings, request, now, body), "invalid UTF8 rejected");
    request = Request();
    request.input.observations.assign(33, "visible");
    Check(!Build(settings, request, now, body), "observation count cap");
    request = Request();
    request.input.party.push_back(request.input.party.front());
    Check(!Build(settings, request, now, body), "duplicate participant rejected");
    request = Request();
    request.input.persona.assign(8192, '\\');
    request.input.systemPrompt.assign(8192, '\\');
    request.input.memory.assign(8192, '\\');
    request.input.observations.assign(32, std::string(1024, '\\'));
    Check(!Build(settings, request, now, body) && body.empty(), "encoded request byte cap");
}

void CircuitChecks()
{
    using namespace OllamaBridge;
    auto const settings = Settings();
    auto request = Request();
    auto identity = MakeIdentity(settings, request);
    Circuit circuit;
    Check(!circuit.Acquire(identity, 10000).admitted, "inactive circuit");
    circuit.Reset(identity);
    auto const first = circuit.Acquire(identity, 10000);
    auto const late = circuit.Acquire(identity, 10000);
    circuit.Complete(first, false, 10000);
    circuit.Complete(late, true, 10001);
    Check(!circuit.Acquire(identity, 14999).admitted, "failure opens; late success cannot close");
    std::atomic<unsigned> admitted = 0;
    std::vector<std::thread> contenders;
    for (unsigned i = 0; i < 8; ++i)
        contenders.emplace_back(
            [&]
            {
                if (circuit.Acquire(identity, 15000).admitted)
                    ++admitted;
            });
    for (auto& contender : contenders)
        contender.join();
    Check(admitted == 1, "one concurrent recovery");

    circuit.Reset(identity);
    auto const old = circuit.Acquire(identity, 16000);
    identity.epoch = "fixture-epoch-b";
    circuit.Reset(identity);
    circuit.Complete(old, false, 16000);
    Check(circuit.Acquire(identity, 16000).admitted, "old epoch failure ignored");
    Check(!circuit.Acquire(MakeIdentity(settings, request), 16000).admitted, "old epoch cannot enter");
    auto changedEndpoint = identity;
    changedEndpoint.endpoint = "http://127.0.0.1:9";
    Check(!circuit.Acquire(changedEndpoint, 16000).admitted, "endpoint identity isolated");

    identity = MakeIdentity(settings, request);
    circuit.Reset(identity);
    Clock now{1000000, 10000};
    unsigned calls = 0;
    auto readClock = [&] { return now; };
    auto fail = [&](OllamaBridge::Settings const&, std::string const&, int64_t remaining)
    {
        ++calls;
        Check(remaining == 10000, "bounded generation transport budget");
        return OllamaHttpResult{0, {}, "synthetic transport failure"};
    };
    Check(!Generate(settings, request, circuit, readClock, fail).ok && calls == 1, "one HTTP failure");
    Check(!Generate(settings, request, circuit, readClock, fail).ok && calls == 1, "open circuit skips transport");
    now = {1005000, 15000};
    auto succeed = [&](OllamaBridge::Settings const&, std::string const&, int64_t remaining)
    {
        ++calls;
        Check(remaining == 5000, "recovery retains original deadline");
        return OllamaHttpResult{200, Reply(settings, request).dump(), {}};
    };
    Check(Generate(settings, request, circuit, readClock, succeed).ok && calls == 2, "recovery closes circuit");
    Check(circuit.Acquire(identity, 15000).admitted, "successful recovery resumes");
    now = {900000, 20000};
    Check(!Generate(settings, request, circuit, readClock, fail).ok && calls == 2, "expired work skips transport");
    now = {1000000, 10000};
    circuit.Reset(identity);
    auto throws = [](OllamaBridge::Settings const&, std::string const&, int64_t) -> OllamaHttpResult
    { throw std::runtime_error("remote credential-like text"); };
    Check(Generate(settings, request, circuit, readClock, throws).error == "bridge_unavailable", "fixed throw reason");
    circuit.Reset(identity);
    auto slow = [&](OllamaBridge::Settings const&, std::string const&, int64_t)
    {
        now = {900000, 20000};
        return OllamaHttpResult{200, Reply(settings, request).dump(), {}};
    };
    Check(!Generate(settings, request, circuit, readClock, slow).ok, "late response rejected");
}

struct TestSocket
{
    socket_t value = INVALID_SOCKET;
    ~TestSocket()
    {
        if (value != INVALID_SOCKET)
        {
            httplib::detail::shutdown_socket(value);
            httplib::detail::close_socket(value);
        }
    }
};

// Nonblocking I/O and a fixture deadline bound both teardown and failed-test
// cleanup. Only the thread owning an accepted socket closes it.
class RawPeer
{
public:
    enum class Mode
    {
        Continue,
        Headers,
        Body,
        NoRead
    };

    explicit RawPeer(Mode mode) : _mode(mode)
    {
        _listener.value = ::socket(AF_INET, SOCK_STREAM, 0);
        Check(_listener.value != INVALID_SOCKET, "raw peer socket");
        if (mode == Mode::NoRead)
            Check(httplib::detail::set_socket_opt(_listener.value, SOL_SOCKET, SO_RCVBUF, 1024),
                  "raw peer receive buffer");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Check(::bind(_listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                  ::listen(_listener.value, 1) == 0,
              "raw peer loopback bind/listen");
        socklen_t size = sizeof(address);
        Check(::getsockname(_listener.value, reinterpret_cast<sockaddr*>(&address), &size) == 0,
              "raw peer ephemeral port");
        port = ntohs(address.sin_port);
        _thread = std::thread([this] { Run(); });
    }

    ~RawPeer() { Finish(); }

    void Finish()
    {
        _stop = true;
        if (_thread.joinable())
            _thread.join();
    }

    int port = 0;
    std::atomic<bool> accepted = false;
    std::atomic<std::size_t> sent = 0;

private:
    void Run()
    {
        auto const until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
        while (!_stop && std::chrono::steady_clock::now() < until)
        {
            if (httplib::detail::select_read(_listener.value, 0, 10000) <= 0)
                continue;
            TestSocket peer{::accept(_listener.value, nullptr, nullptr)};
            if (peer.value == INVALID_SOCKET)
                return;
            accepted = true;
            httplib::detail::set_nonblocking(peer.value, true);
            std::string chunk;
            if (_mode == Mode::Continue)
                for (unsigned i = 0; i < 256; ++i)
                    chunk += "HTTP/1.1 100 Continue\r\n\r\n";
            else if (_mode == Mode::Headers)
                chunk = "HTTP/1.1 200 OK\r\nX-Synthetic: ";
            else if (_mode == Mode::Body)
                chunk = "HTTP/1.1 200 OK\r\nContent-Length: 20000\r\n\r\n";
            while (!_stop && std::chrono::steady_clock::now() < until)
            {
                if (_mode == Mode::NoRead)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                if (httplib::detail::select_write(peer.value, 0, 10000) <= 0)
                    continue;
                auto const bytes =
                    httplib::detail::send_socket(peer.value, chunk.data(), chunk.size(), CPPHTTPLIB_SEND_FLAGS);
                if (bytes <= 0)
                    return;
                sent += static_cast<std::size_t>(bytes);
                // Keep interim replies continuously readable. Drip other phases
                // so their independent header/body byte caps do not win first.
                if (_mode == Mode::Headers || _mode == Mode::Body)
                {
                    chunk = "x";
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            return;
        }
    }

    Mode const _mode;
    TestSocket _listener;
    std::atomic<bool> _stop = false;
    std::thread _thread;
};

void RawDeadlineChecks()
{
    auto settings = Settings();
    for (auto const mode : {RawPeer::Mode::Continue, RawPeer::Mode::Headers, RawPeer::Mode::Body})
    {
        RawPeer peer(mode);
        settings.endpoint = "http://127.0.0.1:" + std::to_string(peer.port);
        auto const started = std::chrono::steady_clock::now();
        auto const result = OllamaHttp_PostBridge(settings.endpoint, "echo", OllamaBridge::Headers(settings), 80);
        auto const elapsed = std::chrono::steady_clock::now() - started;
        peer.Finish();
        Check(peer.accepted && peer.sent > 0, "raw deadline fixture exercised");
        Check(!result.ok() && result.body.empty() && result.error == "bridge_transport_timeout",
              "raw response timeout fails closed");
        Check(elapsed >= std::chrono::milliseconds(60) && elapsed < std::chrono::milliseconds(500),
              "absolute deadline during interim/header/body reads");
    }
}

void StalledWriteChecks()
{
    RawPeer peer(RawPeer::Mode::NoRead);
    auto const started = std::chrono::steady_clock::now();
    {
        BridgeDeadlineClient client("127.0.0.1", peer.port, started + std::chrono::milliseconds(80));
        client.set_keep_alive(false);
        client.set_read_timeout(std::chrono::seconds(2));
        client.set_write_timeout(std::chrono::seconds(2));
        client.set_socket_options([](socket_t socket)
                                  { httplib::detail::set_socket_opt(socket, SOL_SOCKET, SO_SNDBUF, 1024); });
        // Fixture-only bulk data forces backpressure. Production still rejects
        // bodies above 64 KiB; no production socket option is changed for testing.
        std::string const body(4 * 1024 * 1024, 'x');
        std::size_t uploaded = 0;
        auto const response = client.Post("/v1/generate", body.data(), body.size(), "application/json",
                                          [&](uint64_t current, uint64_t)
                                          {
                                              uploaded = static_cast<std::size_t>(current);
                                              return true;
                                          });
        Check(!response && response.error() == httplib::Error::Write && uploaded < body.size(),
              "absolute deadline interrupts blocked write");
    }
    peer.Finish();
    Check(peer.accepted && std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500),
          "blocked write and owned socket/peer cleanup bounded");

    BridgeDeadlineClient expired("127.0.0.1", peer.port, std::chrono::steady_clock::now());
    Check(!expired.Post("/v1/generate", "{}", "application/json"), "expired connect refused");
}

void InterruptedConnectChecks()
{
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    // Deterministic pending-connect equivalent: the real wait helper receives
    // an interrupted poll after 20 ms. No POSIX signal execution is claimed here.
    for (auto const budget : {80ms, 250ms})
    {
        auto const started = Clock::time_point{};
        auto now = started;
        unsigned waits = 0;
        auto interrupted = [&](pollfd* descriptor, nfds_t count, int timeout)
        {
            ++waits;
            Check(count == 1 && descriptor->events == (POLLIN | POLLOUT) && timeout == budget.count(),
                  "pending connect uses remaining deadline");
            now += 20ms;
#ifdef _WIN64
            WSASetLastError(WSAEINTR);
#else
            errno = EINTR;
#endif
            return waits == 1 ? -1 : 0;
        };
        auto const error = WaitForBridgeConnect(INVALID_SOCKET, started + budget, interrupted, [&] { return now; });
        Check(error == httplib::Error::Connection && waits == 1 && now == started + 20ms,
              "interrupted pending connect fails without renewing budget");
    }

    auto const expires = Clock::time_point{} + 80ms;
    auto now = expires - 20ms;
    unsigned waits = 0;
    auto lateReady = [&](pollfd* descriptor, nfds_t, int timeout)
    {
        ++waits;
        Check(timeout == 20, "pending connect deducts elapsed setup time");
        now += 20ms;
        descriptor->revents = POLLOUT;
        return 1;
    };
    Check(WaitForBridgeConnect(INVALID_SOCKET, expires, lateReady, [&] { return now; }) ==
                  httplib::Error::ConnectionTimeout &&
              waits == 1,
          "pending connect rejects readiness at absolute expiry");
    Check(WaitForBridgeConnect(INVALID_SOCKET, expires, lateReady, [&] { return now; }) ==
                  httplib::Error::ConnectionTimeout &&
              waits == 1,
          "expired pending connect does not poll");
}

void SignalSafetyChecks()
{
    unsigned calls = 0;
    bool succeeds = true;
    auto setOption = [&](socket_t socket, int level, int option, int value)
    {
        ++calls;
        Check(socket == INVALID_SOCKET && level == SOL_SOCKET && option == 123 && value == 1,
              "signal safety sets the supplied per-socket option");
        return succeeds;
    };
    Check(ConfigureBridgeSignalSafety(INVALID_SOCKET, true, 123, setOption) && calls == 0,
          "per-send protection needs no socket option");
    Check(ConfigureBridgeSignalSafety(INVALID_SOCKET, false, 123, setOption) && calls == 1,
          "per-socket protection requires successful setup");
    succeeds = false;
    Check(!ConfigureBridgeSignalSafety(INVALID_SOCKET, false, 123, setOption) && calls == 2,
          "failed per-socket protection fails closed");
    Check(!ConfigureBridgeSignalSafety(INVALID_SOCKET, false, 0, setOption) && calls == 2,
          "unsupported signal protection fails closed without a syscall");
}

#ifndef _WIN64
void ClosedPeerSignalChecks()
{
    // Fork before HttpChecks creates a Server (which globally ignores SIGPIPE).
    // Only the child resets/unblocks signals; the parent disposition is untouched.
    for (bool const protectedSend : {false, true})
    {
        pid_t const child = fork();
        Check(child >= 0, "isolated SIGPIPE child created");
        if (child == 0)
        {
            try
            {
                struct sigaction defaults{};
                defaults.sa_handler = SIG_DFL;
                Check(sigemptyset(&defaults.sa_mask) == 0 && sigaction(SIGPIPE, &defaults, nullptr) == 0 &&
                          sigaction(SIGALRM, &defaults, nullptr) == 0,
                      "child default signal dispositions");
                sigset_t unblocked;
                Check(sigemptyset(&unblocked) == 0 && sigaddset(&unblocked, SIGPIPE) == 0 &&
                          sigaddset(&unblocked, SIGALRM) == 0 && sigprocmask(SIG_UNBLOCK, &unblocked, nullptr) == 0,
                      "child signal masks");
                alarm(3);
                if (!protectedSend)
                {
                    int pair[2];
                    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "negative-control socket pair");
                    TestSocket writer{pair[0]};
                    Check(::close(pair[1]) == 0, "negative-control closed peer");
                    ::send(writer.value, "x", 1, 0);
                    _exit(2);  // An unprotected closed-peer send must terminate this child with SIGPIPE.
                }

                TestSocket listener{::socket(AF_INET, SOCK_STREAM, 0)};
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                Check(listener.value != INVALID_SOCKET &&
                          ::bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                          ::listen(listener.value, 1) == 0,
                      "closed-peer listener ready");
                socklen_t length = sizeof(address);
                Check(::getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &length) == 0,
                      "closed-peer listener port");
                auto const expires = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                socket_t owned = INVALID_SOCKET;
                {
                    BridgeDeadlineClient client("127.0.0.1", ntohs(address.sin_port), expires);
                    client.set_keep_alive(false);
                    client.set_header_writer(
                        [&](httplib::Stream&, httplib::Headers&) -> ssize_t
                        {
                            TestSocket peer{::accept(listener.value, nullptr, nullptr)};
                            Check(peer.value != INVALID_SOCKET, "closed-peer connection accepted");
                            Check(::close(peer.value) == 0, "peer closes before request write");
                            peer.value = INVALID_SOCKET;
                            owned = client.socket();
#ifdef SO_NOSIGPIPE
                            int enabled = 0;
                            socklen_t optionLength = sizeof(enabled);
                            Check(::getsockopt(owned, SOL_SOCKET, SO_NOSIGPIPE, &enabled, &optionLength) == 0 &&
                                      enabled == 1,
                                  "owned socket has per-socket SIGPIPE suppression");
#endif
                            // Force EPIPE on the next real stream send regardless of TCP close/RST timing.
                            Check(::shutdown(owned, SHUT_WR) == 0, "closed-peer write shutdown");
                            return 1;
                        });
                    auto const result = client.Post("/v1/generate", "{}", "application/json");
                    Check(!result && result.error() == httplib::Error::Write, "closed-peer send fails terminally");
                    Check(owned != INVALID_SOCKET && client.socket() == INVALID_SOCKET && fcntl(owned, F_GETFD) == -1 &&
                              errno == EBADF,
                          "failed send closes owned socket before client destruction");
                }
                Check(std::chrono::steady_clock::now() < expires, "closed-peer failure and cleanup bounded");
                struct sigaction after{};
                Check(sigaction(SIGPIPE, nullptr, &after) == 0 && after.sa_handler == SIG_DFL,
                      "transport preserves default SIGPIPE disposition");
                _exit(0);
            }
            catch (...)
            {
                _exit(1);
            }
        }
        int status = 0;
        pid_t waited;
        do
        {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        Check(waited == child, "isolated SIGPIPE child reaped");
        Check(
            protectedSend ? WIFEXITED(status) && WEXITSTATUS(status) == 0
                          : WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE,
            protectedSend ? "protected closed-peer send survives and cleans up" : "raw send SIGPIPE negative control");
    }
}
#endif

void HttpChecks()
{
    using namespace OllamaBridge;
    httplib::Server server;
    server.new_task_queue = [] { return new httplib::ThreadPool(2); };
    std::atomic<unsigned> received = 0;
    std::atomic<bool> sawHeader = false;
    std::atomic<unsigned> observed = 0;
    std::atomic<bool> sawObserverHeader = false;
    auto observe = [&](httplib::Request const& request, httplib::Response& response)
    {
        ++observed;
        sawObserverHeader = request.get_header_value_count("Authorization") == 1 &&
                            request.get_header_value("Authorization") == "Bearer " + std::string(64, 'a') &&
                            request.get_header_value("Content-Type") == "application/json";
        if (request.body == "redirect")
            response.set_redirect("http://127.0.0.1:1/forbidden");
        else if (request.body == "failure")
        {
            response.status = 503;
            response.set_content("synthetic-remote-error", "application/json");
        }
        else if (request.body == "oversize")
            response.set_content(std::string(MaxResponseBytes + 1, 'x'), "application/json");
        else if (request.body == "slow")
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            response.set_content("{}", "application/json");
        }
        else
            response.set_content(request.body, "application/json");
    };
    server.Post("/v1/observer/open", observe);
    server.Post("/v1/observer/snapshot", observe);
    server.Post("/v1/generate",
                [&](httplib::Request const& request, httplib::Response& response)
                {
                    ++received;
                    sawHeader = request.get_header_value("Authorization") == "Bearer " + Settings().token;
                    if (request.body == "oversize")
                        response.set_content(std::string(MaxResponseBytes + 1, 'x'), "application/json");
                    else if (request.body == "slow")
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                        response.set_content("{}", "application/json");
                    }
                    else if (request.body == "redirect")
                        response.set_redirect("http://127.0.0.1:1/forbidden");
                    else
                        response.set_content("{\"synthetic\":true}", "application/json");
                });
    int const port = server.bind_to_any_port("127.0.0.1");
    Check(port > 0, "synthetic peer bind");
    std::thread listener([&] { server.listen_after_bind(); });
    struct StopPeer
    {
        httplib::Server& server;
        std::thread& listener;
        ~StopPeer()
        {
            server.stop();
            if (listener.joinable())
                listener.join();
        }
    } cleanup{server, listener};
    auto const readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!server.is_running() && std::chrono::steady_clock::now() < readyDeadline)
        std::this_thread::yield();
    Check(server.is_running(), "synthetic peer ready deadline");
    auto settings = Settings();
    settings.endpoint = "http://127.0.0.1:" + std::to_string(port);
    auto const headers = Headers(settings);
    auto result = OllamaHttp_PostBridge(settings.endpoint, "echo", headers, 1000);
    Check(result.ok() && result.error.empty() && result.body == "{\"synthetic\":true}" && sawHeader,
          "actual bounded HTTP/auth/path");
    result = OllamaHttp_PostBridge(settings.endpoint, "oversize", headers, 1000);
    Check(!result.ok() && result.body.empty() && result.error == "bridge_response_too_large", "stream response cap");
    result = OllamaHttp_PostBridge(settings.endpoint, "redirect", headers, 1000);
    Check(result.status == 302 && received == 3, "no redirect follow");
    auto const started = std::chrono::steady_clock::now();
    result = OllamaHttp_PostBridge(settings.endpoint, "slow", headers, 80);
    auto const elapsed = std::chrono::steady_clock::now() - started;
    Check(!result.ok() && result.body.empty() && result.error == "bridge_transport_timeout" &&
              elapsed < std::chrono::milliseconds(500),
          "actual stalled read deadline");
    Check(!OllamaHttp_PostBridge(settings.endpoint, "echo", {}, 1000).ok(), "missing HTTP token");
    Check(!OllamaHttp_PostBridge("http://localhost:" + std::to_string(port), "echo", headers, 1000).ok(),
          "nonliteral endpoint rejected before HTTP");
    Check(!OllamaHttp_PostBridge(settings.endpoint, std::string(MaxRequestBytes + 1, 'x'), headers, 1000).ok(),
          "request size rejected before HTTP");
    Check(!OllamaHttp_PostBridge(settings.endpoint, "echo", headers, 0).ok(), "expired transport no HTTP");
    auto observer = settings;
    observer.provider = "codex";
    observer.observerEnabled = true;
    observer.observerTokenRef = "synthetic-owned-snapshot/observer.json";
    observer.observerToken = std::string(64, 'a');
    std::string const open =
        "{\"version\":1,\"type\":\"observer.open\",\"instance\":\"fixture-main\",\"provider\":\"codex\"}";
    result = OllamaHttp_PostObserver(observer, OllamaObserverOperation::Open, open, 1000);
    Check(result.ok() && result.body == open && sawObserverHeader, "fixed open path exact body observer bearer");
    std::string const snapshot = std::string(MaxRequestBytes, 'x');
    result = OllamaHttp_PostObserver(observer, OllamaObserverOperation::Snapshot, snapshot, 1000);
    Check(result.error == "bridge_response_too_large" && result.body.empty() && observed == 2,
          "snapshot request cap admitted with bounded echo response");
    result = OllamaHttp_PostObserver(observer, OllamaObserverOperation::Snapshot, "snapshot", 1000);
    Check(result.ok() && result.body == "snapshot" && observed == 3, "fixed snapshot path and exact bytes");
    result = OllamaHttp_PostObserver(observer, OllamaObserverOperation::Open, "redirect", 1000);
    Check(result.status == 302 && observed == 4, "observer redirect never followed");
    result = OllamaHttp_PostObserver(observer, OllamaObserverOperation::Open, "failure", 1000);
    Check(result.status == 503 && observed == 5, "observer errors never retried");
    result = OllamaHttp_PostObserver(observer, OllamaObserverOperation::Open, "slow", 50);
    Check(result.error == "bridge_transport_timeout" && result.body.empty(), "observer total deadline");
    unsigned const beforeRefusals = observed;
    for (int64_t const budget : {int64_t(0), int64_t(-1), int64_t(5001)})
        Check(!OllamaHttp_PostObserver(observer, OllamaObserverOperation::Open, open, budget).ok(),
              "observer freshness budget bounded");
    Check(!OllamaHttp_PostObserver(observer, static_cast<OllamaObserverOperation>(99), open, 1000).ok(),
          "unknown operation refused");
    Check(!OllamaHttp_PostObserver(observer, OllamaObserverOperation::Open, snapshot + "x", 1000).ok(),
          "observer oversized body refused before HTTP");
    auto invalidObserver = observer;
    invalidObserver.observerToken = observer.token;
    Check(!OllamaHttp_PostObserver(invalidObserver, OllamaObserverOperation::Open, open, 1000).ok(),
          "observer never falls back to link bearer");
    invalidObserver = observer;
    invalidObserver.observerEnabled = false;
    Check(!OllamaHttp_PostObserver(invalidObserver, OllamaObserverOperation::Open, open, 1000).ok(),
          "disabled observer sends no HTTP");
    Check(observed == beforeRefusals && received == 4, "observer refusals and operations preserve generation route");
    server.stop();
    listener.join();
    auto const refusedAt = std::chrono::steady_clock::now();
    result = OllamaHttp_PostBridge(settings.endpoint, "echo", headers, 1000);
    Check(!result.ok() && result.body.empty() && std::chrono::steady_clock::now() - refusedAt < std::chrono::seconds(2),
          "actual quick refusal");
}

// J5f (DS.14 (d)): `input.state` exactly as Build encodes it. Absent unless present; every number bounded and every
// string a card fact (64 bytes, no controls, valid UTF-8); the partner only in the bot's group or sight (yards and
// position only on the same map); the combat target only the bot's own visible victim while alive and fighting.
OllamaBridge::BotState FullState()
{
    OllamaBridge::BotState state;
    state.present = true;
    state.healthPct = 73.4f;
    state.powerType = 0;
    state.powerCurrent = 4100;
    state.powerMax = 8200;
    state.level = 42;
    state.alive = true;
    state.inCombat = true;
    state.victim = true;
    state.victimVisible = true;
    state.victimName = "Defias Pillager";
    state.victimLevel = 41;
    state.victimHealthPct = 35.0f;
    state.travelling = true;
    state.questTitle = "The Defias Brotherhood";
    state.zone = "Westfall";
    state.area = "Moonbrook";
    state.mapId = 0;
    state.x = -11018.4f;
    state.y = 1513.6f;
    state.z = 43.2f;
    state.partnerOnline = true;
    state.partnerInGroup = true;
    state.partnerVisible = true;
    state.partnerSameMap = true;
    state.partnerYards = 18.6f;
    state.partnerZone = "Westfall";
    state.partnerX = -11001.2f;
    state.partnerY = 1520.4f;
    state.partnerZ = 44.0f;
    state.mounted = true;
    state.swimming = true;
    state.groupSize = 2;
    state.masterName = "Aldric";
    state.follow = true;
    state.plans = OllamaBridge::StatePlanMovement | OllamaBridge::StatePlanFocusHealing;
    state.freeBagSlots = 14;
    state.money = 1234567;
    return state;
}

// The encoded `input.state`, or null when the envelope has none.
nlohmann::json StateWire(OllamaBridge::BotState const& state)
{
    auto request = Request();
    request.input.state = state;
    std::string body;
    Check(OllamaBridge::Build(Settings(), request, {1000000, 10000}, body), "state: build");
    auto const wire = nlohmann::json::parse(body);
    return wire["input"].contains("state") ? wire["input"]["state"] : nlohmann::json();
}

std::string StateActivityKind(OllamaBridge::BotState const& state)
{
    return StateWire(state)["activity"]["kind"].get<std::string>();
}

void StateChecks()
{
    using namespace OllamaBridge;
    float const nan = std::numeric_limits<float>::quiet_NaN();
    float const inf = std::numeric_limits<float>::infinity();
    BotState absent = FullState();
    absent.present = false;
    Check(StateWire(absent).is_null() && StateWire(BotState()).is_null(), "state: absent unless present");

    Check(StateWire(FullState()) == nlohmann::json::parse(R"({"health":73,"power":{"kind":"mana","pct":50},"level":42,
        "alive":"alive","combat":{"target":{"name":"Defias Pillager","level":41,"health":35}},
        "activity":{"kind":"fighting"},"where":{"zone":"Westfall","area":"Moonbrook","mapId":0,"x":-11018,"y":1514,
        "z":43},"partner":{"zone":"Westfall","yards":19,"x":-11001,"y":1520,"z":44},"mounted":true,"swimming":true,
        "group":{"size":2,"leads":false,"follows":"Aldric"},"movement":"follow","plans":["movement","focus_healing"],
        "bags":14,"money":1234567})"),
          "state: every field encoded with its key");

    // Bounds.
    for (auto const& [pct, want] :
         std::initializer_list<std::pair<float, int>>{{-5.0f, 0}, {150.0f, 100}, {nan, 0}, {inf, 100}, {99.6f, 100}})
    {
        auto state = FullState();
        state.healthPct = pct;
        state.victimHealthPct = pct;
        auto const wire = StateWire(state);
        Check(wire["health"] == want && wire["combat"]["target"]["health"] == want, "state: percent clamped");
    }
    auto state = FullState();
    state.level = 300;
    state.victimLevel = 300;
    state.groupSize = 50;
    state.freeBagSlots = 300;
    state.powerCurrent = 9000;
    state.x = nan;
    state.y = 1e9f;
    state.z = -inf;
    state.partnerYards = -3.0f;
    state.partnerX = inf;
    auto wire = StateWire(state);
    Check(wire["level"] == 255 && wire["combat"]["target"]["level"] == 255, "state: levels capped at 255");
    Check(wire["group"]["size"] == 40 && wire["bags"] == 255, "state: group size and bags capped");
    Check(wire["power"]["pct"] == 100, "state: power above its maximum reads full");
    Check(wire["where"]["x"] == 0 && wire["where"]["y"] == 0 && wire["where"]["z"] == 0 &&
              wire["partner"]["yards"] == 0 && wire["partner"]["x"] == 0,
          "state: yards not finite, beyond any map or negative read 0");
    state = FullState();
    state.partnerYards = nan;
    Check(StateWire(state)["partner"]["yards"] == 0, "state: partner distance NaN reads 0");
    state.level = 0;
    state.victimLevel = 0;
    wire = StateWire(state);
    Check(!wire.contains("level") && !wire["combat"]["target"].contains("level"), "state: an unknown level is omitted");
    state = FullState();
    state.victimName = std::string(63, 'a') + "\xC3\xA9" + std::string(10, 'b');
    state.zone = std::string(70, 'z');
    state.area = std::string("Moon\x01") + "brook\x7F";
    state.questTitle = "\xFF\xFE";
    state.casting = true;
    state.inCombat = false;
    state.masterName = "\t";
    wire = StateWire(state);
    Check(wire["where"]["zone"] == std::string(64, 'z') && wire["where"]["area"] == "Moonbrook",
          "state: strings cut to 64 bytes, controls dropped");
    Check(!wire.contains("combat") && wire["activity"] == nlohmann::json{{"kind", "casting"}},
          "state: no target outside combat");
    state.inCombat = true;
    state.casting = false;
    wire = StateWire(state);
    Check(wire["combat"]["target"]["name"] == std::string(63, 'a'), "state: a cut never splits a UTF-8 sequence");
    Check(!wire["group"].contains("follows"), "state: a blank name is omitted");
    state.inCombat = false;
    wire = StateWire(state);
    Check(wire["activity"] == nlohmann::json{{"kind", "questing"}}, "state: an invalid UTF-8 quest title is omitted");

    // The partner filter.
    state = FullState();
    state.partnerInGroup = false;
    state.partnerVisible = false;
    Check(!StateWire(state).contains("partner"), "state: partner absent when neither grouped nor visible");
    state.partnerVisible = true;
    Check(StateWire(state).contains("partner"), "state: a visible partner outside the group is known");
    state = FullState();
    state.partnerOnline = false;
    Check(!StateWire(state).contains("partner"), "state: an offline partner is absent");
    state = FullState();
    state.partnerVisible = false;
    state.partnerSameMap = false;
    state.partnerZone = "Stormwind City";
    Check(StateWire(state)["partner"] == nlohmann::json{{"zone", "Stormwind City"}},
          "state: a grouped partner on another map gives the zone only");

    // The combat filter: own victim, visible, alive and fighting.
    for (int drop = 0; drop < 4; ++drop)
    {
        state = FullState();
        (drop == 0 ? state.victim : drop == 1 ? state.victimVisible : drop == 2 ? state.inCombat : state.alive) = false;
        Check(!StateWire(state).contains("combat"), "state: combat target only the bot's own visible victim");
    }

    // Activity priority: dead idle; flying > fighting > casting > looting > resting > questing > travelling > idle.
    state = FullState();
    state.flying = state.casting = state.looting = state.resting = state.working = true;
    Check(StateActivityKind(state) == "flying", "state: flying first");
    state.flying = false;
    Check(StateActivityKind(state) == "fighting", "state: fighting before casting");
    state.inCombat = false;
    Check(StateActivityKind(state) == "casting", "state: casting before looting");
    state.casting = false;
    Check(StateActivityKind(state) == "looting", "state: looting before resting");
    state.looting = false;
    Check(StateActivityKind(state) == "resting", "state: resting before questing");
    state.resting = false;
    Check(StateWire(state)["activity"] == nlohmann::json({{"kind", "questing"}, {"detail", "The Defias Brotherhood"}}),
          "state: questing names the quest");
    state.travelling = false;
    Check(StateActivityKind(state) == "questing", "state: working at a quest destination is questing");
    state.working = false;
    Check(StateActivityKind(state) == "idle", "state: nothing to do is idle");
    state.travelling = true;
    state.questTitle.clear();
    Check(StateWire(state)["activity"] == nlohmann::json{{"kind", "travelling"}}, "state: travelling without a quest");
    state.alive = false;
    state.flying = true;
    Check(StateActivityKind(state) == "idle" && StateWire(state)["alive"] == "dead", "state: the dead are idle");
    state.ghost = true;
    Check(StateWire(state)["alive"] == "ghost", "state: a ghost");

    // Movement priority, plans, power kinds, optional keys.
    state = FullState();
    state.stay = state.guard = state.grind = state.passive = true;
    Check(StateWire(state)["movement"] == "stay", "state: stay first");
    state.stay = false;
    Check(StateWire(state)["movement"] == "follow", "state: follow before guard");
    state.follow = false;
    Check(StateWire(state)["movement"] == "guard", "state: guard before grind");
    state.guard = false;
    Check(StateWire(state)["movement"] == "grind", "state: grind before passive");
    state.grind = false;
    Check(StateWire(state)["movement"] == "passive", "state: passive");
    state.passive = false;
    Check(StateWire(state)["movement"] == "none", "state: no movement strategy");
    state.plans = StatePlanMovement | StatePlanFormation | StatePlanSaveMana | StatePlanFocusHealing;
    Check(StateWire(state)["plans"] == nlohmann::json({"movement", "formation", "save_mana", "focus_healing"}),
          "state: plan bits in PlanKind order");
    state.plans = StatePlanSaveMana;
    Check(StateWire(state)["plans"] == nlohmann::json({"save_mana"}), "state: one plan");
    state.plans = 0;
    Check(!StateWire(state).contains("plans"), "state: no plans, no key");
    for (auto const& [type, kind] : std::initializer_list<std::pair<uint8_t, char const*>>{
             {0, "mana"}, {1, "rage"}, {2, "focus"}, {3, "energy"}, {6, "runic"}})
    {
        state.powerType = type;
        Check(StateWire(state)["power"]["kind"] == kind, "state: power kinds");
    }
    state.powerType = 4;
    Check(!StateWire(state).contains("power"), "state: other power types omitted");
    state.powerType = 1;
    state.powerMax = 0;
    Check(!StateWire(state).contains("power"), "state: no maximum, no power");
    state = FullState();
    state.area = state.zone;
    state.mounted = state.swimming = false;
    state.groupSize = 0;
    wire = StateWire(state);
    Check(!wire["where"].contains("area") && !wire.contains("mounted") && !wire.contains("swimming") &&
              !wire.contains("group"),
          "state: area equal to zone, false flags and no group omitted");

    // Size over fixtures (recorded in the step's Log; the brief estimated 380-450 bytes).
    std::vector<std::size_t> sizes;
    state = FullState();
    sizes.push_back(StateWire(state).dump().size());  // Grouped, fighting, everything set.
    state.inCombat = false;
    state.mounted = state.swimming = false;
    sizes.push_back(StateWire(state).dump().size());  // Grouped, questing.
    state.partnerInGroup = state.partnerVisible = false;
    state.groupSize = 0;
    state.follow = false;
    state.plans = 0;
    sizes.push_back(StateWire(state).dump().size());  // Solo, partner unknown.
    state.travelling = false;
    state.questTitle.clear();
    state.area = state.zone;
    sizes.push_back(StateWire(state).dump().size());  // Solo, idle.
    state = FullState();
    state.inCombat = false;
    state.resting = true;
    state.zone = "Stormwind City";
    state.area = "Trade District";
    sizes.push_back(StateWire(state).dump().size());  // Grouped, resting in a city.
    state = FullState();
    state.victimName = state.questTitle = state.zone = state.area = state.partnerZone = state.masterName =
        std::string(80, 'w');
    state.plans = 15;
    state.money = 4294967295u;
    std::size_t const worst = StateWire(state).dump().size();
    std::sort(sizes.begin(), sizes.end());
    std::cout << "bridge-selftest: input.state bytes p50 " << sizes[sizes.size() / 2] << ", max " << sizes.back()
              << " over " << sizes.size() << " fixtures; worst case (all strings at the cap) " << worst << '\n';
    Check(worst <= 1024, "state: worst case stays within 1 KiB");
}

// J6e-c (DS.14 (c)): `input.wake` is exactly {condition, watchId} and only on a wake; the chain's depth and origin
// stay in the module. ActivityCode follows the state's activity kind (the watch compares codes).
void WakeChecks()
{
    using namespace OllamaBridge;
    auto request = Request();
    std::string body;
    Check(Build(Settings(), request, {1000000, 10000}, body) && !nlohmann::json::parse(body)["input"].contains("wake"),
          "wake: absent on a chat line");
    request.input.wakeCondition = "combat_end";
    request.input.wakeWatchId = "watch-7";
    request.input.wakeDepth = 2;
    request.input.originUnixMs = 987654321;
    Check(Build(Settings(), request, {1000000, 10000}, body), "wake: build");
    auto const input = nlohmann::json::parse(body)["input"];
    Check(input.contains("wake") &&
              input["wake"] == nlohmann::json({{"condition", "combat_end"}, {"watchId", "watch-7"}}),
          "wake: exactly condition and watch id");
    Check(body.find("987654321") == std::string::npos && !input.contains("wakeDepth") && !input.contains("depth"),
          "wake: depth and origin never on the wire");

    auto const code = [](BotState const& state) { return static_cast<int>(ActivityCode(state)); };
    BotState state = FullState();
    Check(code(state) == 2, "activity code: fighting 2");
    state.flying = true;
    Check(code(state) == 1, "activity code: flying 1");
    state = FullState();
    state.alive = false;
    Check(code(state) == 0, "activity code: dead is idle 0");
    state = FullState();
    state.inCombat = false;
    Check(code(state) == 6, "activity code: questing 6");
    state.casting = true;
    Check(code(state) == 3, "activity code: casting 3");
    state.casting = false;
    state.looting = true;
    Check(code(state) == 4, "activity code: looting 4");
    state.looting = false;
    state.resting = true;
    Check(code(state) == 5, "activity code: resting 5");
    state.resting = false;
    state.questTitle.clear();
    Check(code(state) == 7, "activity code: travelling 7");
    state.travelling = false;
    Check(code(state) == 0, "activity code: idle 0");
}

// LC10: `input.facts`, `input.traits` and the state's around, servant, mount and deeds exactly as Build encodes them.
// Absent fields leave the request as before; a deed is a whole sentence of at most 200 bytes (LC7's cap), never the
// 64-byte fact rule; every list stops at its wire cap; an empty list or traits object is left out.
nlohmann::json InputWire(OllamaBridge::Request const& request)
{
    std::string body;
    Check(OllamaBridge::Build(Settings(), request, {1000000, 10000}, body), "lc10: build");
    return nlohmann::json::parse(body)["input"];
}

OllamaBridge::BotFacts FullFacts()
{
    OllamaBridge::BotFacts facts;
    facts.name = "Thorgrim";
    facts.race = "Dwarf";
    facts.className = "Warrior";
    facts.gender = "male";
    facts.level = 42;
    facts.side = "Alliance";
    facts.phrase = "fights with a great axe in both hands";
    facts.crafts = {{"Mining", "expert"}, {"Blacksmithing", "journeyman"}};
    facts.useful = {"sharpening stones"};
    facts.standing = {{"Ironforge", "honoured"}};
    facts.guild = "Stonehearth";
    return facts;
}

void FactsChecks()
{
    using namespace OllamaBridge;
    std::string const oldBody = []
    {
        std::string body;
        Check(Build(Settings(), Request(), {1000000, 10000}, body), "lc10: old build");
        return body;
    }();
    auto const oldInput = nlohmann::json::parse(oldBody)["input"];
    std::vector<std::string> keys;
    for (auto const& item : oldInput.items())
        keys.push_back(item.key());
    Check(keys == std::vector<std::string>{"channel", "directAddress", "groupScope", "memory", "observations", "party",
                                           "persona", "speakerControlsBot", "speakerGuid", "systemPrompt", "text"},
          "lc10: without the new fields the input has exactly the old keys");
    auto request = Request();
    request.input.facts = BotFacts();  // Present but without who or phrase: left out whole.
    request.input.traits.words = {"", " \t", "\x01"};
    request.input.traits.interests = {"\xFF"};
    std::string body;
    Check(Build(Settings(), request, {1000000, 10000}, body) && body == oldBody,
          "lc10: facts without who, blank and invalid traits leave the request byte-identical");
    request = Request();
    auto state = FullState();
    request.input.state = state;
    Check(Build(Settings(), request, {1000000, 10000}, body), "lc10: old state build");
    std::string const oldStateBody = body;
    state.around = {{"", "hostile", "", 1}, {"Wolf", "beast", "", 1}};
    state.servant = state.mount = " ";
    state.deeds = {"", "\xC3"};
    request.input.state = state;
    Check(Build(Settings(), request, {1000000, 10000}, body) && body == oldStateBody,
          "lc10: state lists without a valid entry are left out (old bytes)");

    // Facts: who, phrase, the lists in the reader's order; empty lists and no guild left out.
    request = Request();
    request.input.facts = FullFacts();
    Check(InputWire(request)["facts"] == nlohmann::json::parse(R"({"who":{"name":"Thorgrim","race":"Dwarf",
        "class":"Warrior","gender":"male","level":42,"side":"Alliance"},
        "phrase":"fights with a great axe in both hands",
        "crafts":[{"name":"Mining","rank":"expert"},{"name":"Blacksmithing","rank":"journeyman"}],
        "useful":["sharpening stones"],"standing":[{"faction":"Ironforge","word":"honoured"}],"guild":"Stonehearth"})"),
          "lc10: facts encoded with their keys, empty lists left out");
    auto const dropped = [&request](auto&& spoil)
    {
        auto facts = FullFacts();
        spoil(facts);
        request.input.facts = facts;
        return !InputWire(request).contains("facts");
    };
    Check(dropped([](BotFacts& f) { f.phrase = "  "; }) && dropped([](BotFacts& f) { f.name.clear(); }) &&
              dropped([](BotFacts& f) { f.race = "\x01"; }) && dropped([](BotFacts& f) { f.className = "\xFF"; }) &&
              dropped([](BotFacts& f) { f.gender = "other"; }) && dropped([](BotFacts& f) { f.level = 0; }) &&
              dropped([](BotFacts& f) { f.side = "Scourge"; }),
          "lc10: facts left out whole when who or the phrase fails its rule");
    auto facts = FullFacts();
    facts.level = 300;
    facts.guild.clear();
    facts.phrase = std::string(96, 'p');
    facts.crafts = {{"Mining", ""}, {"", "expert"}, {"Herbalism", std::string(40, 'r')}};
    facts.standing.clear();
    facts.useful.assign(13, "torch");
    facts.servants.assign(9, "Grimtooth");
    facts.relatives = {"Brann", " "};
    request.input.facts = facts;
    auto wire = InputWire(request)["facts"];
    Check(wire["who"]["level"] == 255 && !wire.contains("guild") && !wire.contains("standing") &&
              wire["phrase"] == std::string(96, 'p') &&
              wire["crafts"] ==
                  nlohmann::json::parse(R"([{"name":"Herbalism","rank":"rrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr"}])") &&
              wire["useful"].size() == 12 && wire["servants"].size() == 8 &&
              wire["relatives"] == nlohmann::json::array({"Brann"}),
          "lc10: level capped, pairs need both texts (word cut at 32), lists at their caps, blanks dropped");
    facts.phrase = std::string(97, 'p');
    request.input.facts = facts;
    Check(InputWire(request)["facts"]["phrase"] == std::string(96, 'p'), "lc10: phrase cut at 96 bytes");

    // Traits: each word whole within 1024 bytes (never cut at 64), 32 words, 3 interests; one list alone is fine.
    request = Request();
    std::string const longWord = std::string(1023, 'w') + "\xC3\xA9";  // 1025 bytes, byte 1024 inside a character.
    request.input.traits.words = {"very careful", std::string(100, 'c'), std::string(1024, 'k'), longWord};
    for (int i = 0; i < 30; ++i)
        request.input.traits.words.push_back("rather chatty");
    request.input.traits.interests = {"fishing", "inns and ale", "old maps", "dancing"};
    wire = InputWire(request)["traits"];
    Check(wire.size() == 2 && wire["words"].size() == 32 && wire["words"][0] == "very careful" &&
              wire["words"][1] == std::string(100, 'c') && wire["words"][2] == std::string(1024, 'k') &&
              wire["words"][3] == std::string(1023, 'w') &&
              wire["interests"] == nlohmann::json({"fishing", "inns and ale", "old maps"}),
          "lc10: trait words whole to 1024 bytes, cut on a character boundary past it, lists at their caps");
    request.input.traits.words.clear();
    wire = InputWire(request);
    Check(wire.contains("traits") && !wire["traits"].contains("words") && wire["traits"]["interests"].size() == 3,
          "lc10: traits with interests only");
    request.input.traits = {};
    Check(!InputWire(request).contains("traits"), "lc10: traits left out when both lists are empty");

    // State: around (nearest first, one per name, known kinds, count 1-255), servant and mount (64-byte rule),
    // deeds (whole sentences to 200 bytes, newest first, at most 3; a blank or invalid one dropped, not the state).
    request = Request();
    state = FullState();
    state.around = {{"Defias Pillager", "hostile", "", 2},
                    {"Wolf", "hostile", "", 0},
                    {"Wolf\x01", "hostile", "", 1},
                    {"Innkeeper Farley", "service", "innkeeper", 300},
                    {"Strange Rock", "riddle", "", 1},
                    {"\xFF", "object", "", 1},
                    {std::string(70, 'n'), "person", std::string(70, 'd'), 1}};
    for (int i = 0; i < 12; ++i)
        state.around.push_back({"Kobold " + std::to_string(i), "hostile", "", 1});
    state.servant = std::string(64, 's');
    state.mount = std::string(65, 'm');
    std::string const deed200 = std::string(199, 'd') + ".";
    std::string const deed201 = std::string(199, 'e') + "\xC3\xA9";  // Byte 200 inside a character.
    state.deeds = {deed200, " ", deed201, "\xC3\x28", std::string(201, 'f'), "A fourth deed."};
    request.input.state = state;
    auto const stateWire = InputWire(request)["state"];
    auto const& around = stateWire["around"];
    Check(around.size() == 12 &&
              around[0] == nlohmann::json({{"name", "Defias Pillager"}, {"kind", "hostile"}, {"count", 2}}) &&
              around[1] == nlohmann::json({{"name", "Wolf"}, {"kind", "hostile"}, {"count", 1}}) &&
              around[2] ==
                  nlohmann::json(
                      {{"name", "Innkeeper Farley"}, {"kind", "service"}, {"detail", "innkeeper"}, {"count", 255}}) &&
              around[3] == nlohmann::json({{"name", std::string(64, 'n')},
                                           {"kind", "person"},
                                           {"detail", std::string(64, 'd')},
                                           {"count", 1}}) &&
              around[4]["name"] == "Kobold 0" && around[11]["name"] == "Kobold 7",
          "lc10: around in order, one per name after the bounds, known kinds only, count 1-255, at most 12");
    Check(stateWire["servant"] == std::string(64, 's') && stateWire["mount"] == std::string(64, 'm'),
          "lc10: servant and mount keep the 64-byte rule (64 unchanged, 65 cut)");
    Check(stateWire["deeds"] == nlohmann::json({deed200, std::string(199, 'e'), std::string(200, 'f')}),
          "lc10: deeds whole to 200 bytes, cut on a character boundary past it, blank and invalid dropped, at most 3");
    Check(stateWire["money"] == 1234567 && stateWire["where"]["zone"] == "Westfall",
          "lc10: the old state keys are unchanged beside the new ones");

    // Size: every new field at its cap (traits at 1024 bytes each) on the fixture request stays under the bound.
    request = Request();
    BotFacts cap;
    cap.name = cap.race = cap.className = cap.guild = std::string(64, 'x');
    cap.gender = "female";
    cap.level = 80;
    cap.side = "Horde";
    cap.phrase = std::string(96, 'p');
    cap.crafts.assign(8, {std::string(64, 'c'), std::string(32, 'r')});
    cap.useful.assign(12, std::string(64, 'u'));
    cap.servants.assign(8, std::string(64, 's'));
    cap.standing.assign(6, {std::string(64, 'f'), std::string(32, 'w')});
    cap.relatives.assign(8, std::string(64, 'k'));
    request.input.facts = cap;
    request.input.traits.words.assign(32, "extremely careful");
    request.input.traits.interests.assign(3, "inns and ale");
    state = FullState();
    state.around.clear();
    for (int i = 0; i < 12; ++i)
        state.around.push_back({std::string(60, 'a') + std::to_string(1000 + i), "hostile", std::string(64, 'd'), 255});
    state.servant = state.mount = std::string(64, 'v');
    state.deeds.assign(3, std::string(200, 'z'));
    request.input.state = state;
    Check(Build(Settings(), request, {1000000, 10000}, body), "lc10: realistic traits build");
    std::size_t const realistic = body.size();
    auto const capped = nlohmann::json::parse(body)["input"];
    Check(capped["state"]["around"].size() == 12 && capped["facts"]["crafts"].size() == 8 &&
              capped["state"]["deeds"].size() == 3,
          "lc10: every list at its cap is sent whole");
    request.input.traits.words.assign(32, std::string(1024, 't'));
    request.input.traits.interests.assign(3, std::string(1024, 'i'));
    Check(Build(Settings(), request, {1000000, 10000}, body) && body.size() < MaxRequestBytes,
          "lc10: every new field at its cap stays under MaxRequestBytes");
    std::cout << "bridge-selftest: lc10 request bytes: old " << oldBody.size() << ", every new field at cap with "
              << "realistic traits " << realistic << ", with every trait at 1024 bytes " << body.size() << '\n';
}
}  // namespace

int main(int argc, char** argv)
{
    try
    {
        BoundaryChecks();
        StateChecks();
        WakeChecks();
        FactsChecks();
        if (argc == 2)
            ObserverSettingsChecks(argv[1]);
        CircuitChecks();
        InterruptedConnectChecks();
        SignalSafetyChecks();
#ifndef _WIN64
        ClosedPeerSignalChecks();
#endif
        HttpChecks();
        RawDeadlineChecks();
        StalledWriteChecks();
        std::cout << "bridge-selftest: " << checks << " checks passed (synthetic, no live game authority/delivery)\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "bridge-selftest failed: " << error.what() << '\n';
        return 1;
    }
}
