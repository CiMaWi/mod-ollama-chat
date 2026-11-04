#include "mod-ollama-chat_bridge.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace OllamaBridge
{
namespace
{
bool AsciiLetterOrDigit(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool ValidContext(Context const& input)
{
    if (!input.speakerGuid || input.channel.empty() || input.channel.size() > 128 || input.text.empty() ||
        input.text.size() > MaxTextBytes || input.systemPrompt.size() > 8192 || input.persona.size() > 8192 ||
        input.memory.size() > 8192 || input.groupScope.size() > 128 || input.observations.size() > 32 ||
        input.party.size() > 40)
        return false;

    for (auto const& observation : input.observations)
        if (observation.size() > 1024)
            return false;

    std::set<uint64_t> participants;
    for (auto const& participant : input.party)
        if (!participant.guid || !participants.insert(participant.guid).second || participant.name.size() > 128 ||
            participant.healthPercent < -1 || participant.healthPercent > 100 ||
            (!participant.role.empty() && participant.role != "tank" && participant.role != "healer" &&
             participant.role != "damage"))
            return false;

    return true;
}

bool ValidRequest(Settings const& settings, Request const& request, Clock now)
{
    return ValidSettings(settings) && ValidIdentifier(request.epoch) && ValidIdentifier(request.requestId) &&
           request.controllerGuid && request.botGuid && ValidContext(request.input) &&
           RemainingMs(request.deadline, now) > 0;
}

bool MatchesString(nlohmann::json const& body, char const* key, std::string const& expected)
{
    auto const it = body.find(key);
    return it != body.end() && it->is_string() && it->get_ref<std::string const&>() == expected;
}
}  // namespace

bool Identity::operator==(Identity const& other) const
{
    return instance == other.instance && provider == other.provider && endpoint == other.endpoint &&
           epoch == other.epoch;
}

bool ValidIdentifier(std::string const& value)
{
    return !value.empty() && value.size() <= 128 && AsciiLetterOrDigit(value.front()) &&
           std::all_of(value.begin(), value.end(), [](unsigned char c)
                       { return AsciiLetterOrDigit(c) || c == '_' || c == '.' || c == ':' || c == '-'; });
}

bool ValidEndpoint(std::string const& value)
{
    std::string const prefix = "http://127.0.0.1";
    if (value == prefix)
        return true;
    if (value.size() <= prefix.size() + 1 || value.size() > prefix.size() + 6 ||
        value.compare(0, prefix.size(), prefix) != 0 || value[prefix.size()] != ':')
        return false;

    uint32_t port = 0;
    for (std::size_t i = prefix.size() + 1; i < value.size(); ++i)
    {
        unsigned char const c = value[i];
        if (c < '0' || c > '9')
            return false;
        port = port * 10 + c - '0';
    }
    return port > 0 && port <= 65535;
}

bool ValidSettings(Settings const& settings)
{
    // Provision at least 128 random bits as printable base64url/hex. Size cannot prove entropy.
    return ValidIdentifier(settings.instance) && ValidIdentifier(settings.provider) &&
           ValidEndpoint(settings.endpoint) && settings.token.size() >= 22 && settings.token.size() <= 512 &&
           std::all_of(settings.token.begin(), settings.token.end(),
                       [](unsigned char c)
                       {
                           return AsciiLetterOrDigit(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '+' ||
                                  c == '/' || c == '=';
                       });
}

Clock Now()
{
    return {std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count(),
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count()};
}

Deadline CaptureDeadline(Clock now, int64_t lifetimeMs)
{
    if (now.unixMs < 0 || now.steadyMs < 0 || lifetimeMs <= 0 || lifetimeMs > MaxLifetimeMs ||
        now.unixMs > std::numeric_limits<int64_t>::max() - lifetimeMs ||
        now.steadyMs > std::numeric_limits<int64_t>::max() - lifetimeMs)
        return {};
    return {now.unixMs + lifetimeMs, now.steadyMs + lifetimeMs};
}

bool Expired(Deadline deadline, Clock now) { return RemainingMs(deadline, now) == 0; }

int64_t RemainingMs(Deadline deadline, Clock now)
{
    if (now.unixMs < 0 || now.steadyMs < 0 || deadline.unixMs <= now.unixMs || deadline.steadyMs <= now.steadyMs)
        return 0;
    int64_t const remaining = std::min(deadline.unixMs - now.unixMs, deadline.steadyMs - now.steadyMs);
    // UTC rollback cannot extend lifetime; the monotonic clock is always a second cap.
    if (remaining > MaxLifetimeMs)
        return 0;
    return remaining;
}

Identity MakeIdentity(Settings const& settings, Request const& request)
{
    return {settings.instance, settings.provider, settings.endpoint, request.epoch};
}

OllamaHttpHeaders Headers(Settings const& settings)
{
    if (!ValidSettings(settings))
        return {};
    return {{"Authorization", "Bearer " + settings.token}};
}

bool Build(Settings const& settings, Request const& request, Clock now, std::string& body)
{
    body.clear();
    if (!ValidRequest(settings, request, now))
        return false;
    try
    {
        auto const& input = request.input;
        nlohmann::json party = nlohmann::json::array();
        for (auto const& participant : input.party)
        {
            nlohmann::json entry = {{"guid", std::to_string(participant.guid)}, {"name", participant.name}};
            if (!participant.role.empty())
                entry["role"] = participant.role;
            if (participant.healthPercent >= 0)
                entry["healthPercent"] = participant.healthPercent;
            if (participant.visibleTargetGuid)
                entry["visibleTargetGuid"] = std::to_string(participant.visibleTargetGuid);
            party.push_back(std::move(entry));
        }
        nlohmann::json const envelope = {{"version", 1},
                                         {"instance", settings.instance},
                                         {"provider", settings.provider},
                                         {"epoch", request.epoch},
                                         {"requestId", request.requestId},
                                         {"deadlineUnixMs", request.deadline.unixMs},
                                         {"type", "generation"},
                                         {"controllerGuid", std::to_string(request.controllerGuid)},
                                         {"botGuid", std::to_string(request.botGuid)},
                                         {"input",
                                          {{"channel", input.channel},
                                           {"text", input.text},
                                           {"observations", input.observations},
                                           {"speakerGuid", std::to_string(input.speakerGuid)},
                                           {"systemPrompt", input.systemPrompt},
                                           {"persona", input.persona},
                                           {"memory", input.memory},
                                           {"groupScope", input.groupScope},
                                           {"directAddress", input.directAddress},
                                           {"speakerControlsBot", input.speakerControlsBot},
                                           {"party", std::move(party)}}}};
        std::string encoded = envelope.dump();
        if (encoded.size() > MaxRequestBytes)
            return false;
        body = std::move(encoded);
        return true;
    }
    catch (std::exception const&)
    {
        return false;
    }
}

Result Parse(Settings const& settings, Request const& request, std::string const& body, Clock now)
{
    Result result;
    result.error = "bridge_invalid_response";
    if (!ValidRequest(settings, request, now))
    {
        result.error = "bridge_expired_or_invalid_request";
        return result;
    }
    if (body.empty() || body.size() > MaxResponseBytes)
        return result;
    try
    {
        // Reject duplicate keys before the DOM overwrites them. Bound parser nesting as well as bytes.
        std::map<int, std::set<std::string>> keys;
        auto callback = [&keys](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value)
        {
            if (depth > 16)
                throw std::runtime_error("depth");
            if (event == nlohmann::json::parse_event_t::object_start)
                keys[depth + 1].clear();
            if (event == nlohmann::json::parse_event_t::key && !keys[depth].insert(value.get<std::string>()).second)
                throw std::runtime_error("duplicate");
            return true;
        };
        auto const parsed = nlohmann::json::parse(body, callback);
        if (!parsed.is_object() || parsed.size() != 8 || !parsed.contains("version") ||
            !parsed["version"].is_number_integer() || parsed["version"] != 1 ||
            !MatchesString(parsed, "instance", settings.instance) ||
            !MatchesString(parsed, "provider", settings.provider) || !MatchesString(parsed, "epoch", request.epoch) ||
            !MatchesString(parsed, "requestId", request.requestId) || !parsed.contains("deadlineUnixMs") ||
            !parsed["deadlineUnixMs"].is_number_integer() || parsed["deadlineUnixMs"] != request.deadline.unixMs)
            return result;

        if (MatchesString(parsed, "type", "failure"))
        {
            if (MatchesString(parsed, "code", "unavailable") || MatchesString(parsed, "code", "timeout") ||
                MatchesString(parsed, "code", "invalid_input") || MatchesString(parsed, "code", "cancelled"))
                result.error = "bridge_terminal_failure";
            return result;
        }
        if (!MatchesString(parsed, "type", "generation.result") || !parsed.contains("text") ||
            !parsed["text"].is_string())
            return result;
        auto const& text = parsed["text"].get_ref<std::string const&>();
        if (text.empty() || text.size() > MaxTextBytes ||
            std::all_of(text.begin(), text.end(),
                        [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }) ||
            text.find('\0') != std::string::npos)
            return result;
        result.ok = true;
        result.text = text;
        result.error.clear();
    }
    catch (std::exception const&)
    {
        // Fixed reason deliberately excludes the parse exception and remote body.
    }
    return result;
}

void Circuit::Reset(Identity identity)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _identity = std::move(identity);
    ++_revision;
    _open = false;
    _recovering = false;
    _retryAtMs = 0;
}

Circuit::Ticket Circuit::Acquire(Identity const& identity, int64_t steadyMs)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_revision == 0 || !(_identity == identity) || (_open && (steadyMs < _retryAtMs || _recovering)))
        return {};
    if (_open)
        _recovering = true;
    return {_revision, true, _open};
}

void Circuit::Complete(Ticket ticket, bool success, int64_t steadyMs)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!ticket.admitted || ticket.revision != _revision)
        return;
    if (!success)
    {
        ++_revision;
        _open = true;
        _recovering = false;
        _retryAtMs = steadyMs > std::numeric_limits<int64_t>::max() - CircuitCooldownMs
                         ? std::numeric_limits<int64_t>::max()
                         : steadyMs + CircuitCooldownMs;
    }
    else if (ticket.recovery)
    {
        ++_revision;
        _open = false;
        _recovering = false;
    }
}

Result Generate(Settings const& settings, Request const& request, Circuit& circuit, ReadClock const& readClock,
                Transport const& transport)
{
    Result result;
    std::string body;
    Clock now = readClock();
    if (!Build(settings, request, now, body))
    {
        result.error = "bridge_expired_or_invalid_request";
        return result;
    }
    auto const ticket = circuit.Acquire(MakeIdentity(settings, request), now.steadyMs);
    if (!ticket.admitted)
    {
        result.error = "bridge_circuit_open_or_inactive";
        return result;
    }
    try
    {
        now = readClock();
        int64_t const remaining = RemainingMs(request.deadline, now);
        if (remaining == 0)
            result.error = "bridge_expired_or_invalid_request";
        else
        {
            auto const http = transport(settings, body, remaining);
            if (http.ok() && http.error.empty())
                result = Parse(settings, request, http.body, readClock());
            else
                result.error = "bridge_unavailable";
            result.status = http.status;
        }
    }
    catch (std::exception const&)
    {
        result.error = "bridge_unavailable";
    }
    circuit.Complete(ticket, result.ok, readClock().steadyMs);
    return result;
}
}  // namespace OllamaBridge
