#ifndef MOD_OLLAMA_CHAT_BRIDGE_JSON_H
#define MOD_OLLAMA_CHAT_BRIDGE_JSON_H

// LCB6a: each wire check of a bridge packet, written once: the strict parse, the identifier, the numbers, the texts,
// the envelope and the one bearer compare (the listener's). Header only; the bridge, channel, listener and world
// units use it and keep no copy of their own.

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace OllamaBridge
{
constexpr int MaxJsonDepth = 16;
constexpr std::size_t MaxBearerBytes = 256;
constexpr std::size_t MaxIdentifierBytes = 128;
constexpr int64_t MaxSafeInteger = 9007199254740991LL;  // JavaScript's exact integer range.

inline bool AsciiLetterOrDigit(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// J2's identifier: 1..128 ASCII bytes, a letter or digit first, then letters, digits, '_', '.', ':' or '-'.
inline bool ValidIdentifier(std::string_view value)
{
    return !value.empty() && value.size() <= MaxIdentifierBytes && AsciiLetterOrDigit(value.front()) &&
           std::all_of(value.begin(), value.end(), [](unsigned char c)
                       { return AsciiLetterOrDigit(c) || c == '_' || c == '.' || c == ':' || c == '-'; });
}

// One JSON document with no repeated key in any object and at most MaxJsonDepth levels; throws otherwise (invalid
// UTF-8 included). The duplicate check runs before the DOM would overwrite the first value.
inline nlohmann::json StrictParse(std::string_view body)
{
    std::map<int, std::set<std::string>> keys;
    auto callback = [&keys](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value)
    {
        if (depth > MaxJsonDepth)
            throw std::runtime_error("invalid");
        if (event == nlohmann::json::parse_event_t::object_start)
            keys[depth + 1].clear();
        if (event == nlohmann::json::parse_event_t::key && !keys[depth].insert(value.get<std::string>()).second)
            throw std::runtime_error("invalid");
        return true;
    };
    return nlohmann::json::parse(body.begin(), body.end(), callback);
}

// An object with exactly these fields.
inline bool Exact(nlohmann::json const& value, std::initializer_list<char const*> fields)
{
    return value.is_object() && value.size() == fields.size() &&
           std::all_of(fields.begin(), fields.end(), [&](char const* field) { return value.contains(field); });
}

inline bool MatchesString(nlohmann::json const& body, char const* key, std::string_view expected)
{
    auto const it = body.find(key);
    return it != body.end() && it->is_string() && it->get_ref<std::string const&>() == expected;
}

// The value as a string; throws when it is none.
inline std::string String(nlohmann::json const& value)
{
    if (!value.is_string())
        throw std::runtime_error("invalid");
    return value.get<std::string>();
}

// A canonical decimal uint64 string, never zero; throws otherwise.
inline uint64_t Guid(nlohmann::json const& value)
{
    auto const text = String(value);
    if (text.empty() || text.size() > 20 || (text.size() > 1 && text.front() == '0'))
        throw std::runtime_error("invalid");
    uint64_t result = 0;
    for (unsigned char c : text)
    {
        if (c < '0' || c > '9' || result > (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10)
            throw std::runtime_error("invalid");
        result = result * 10 + c - '0';
    }
    if (!result)
        throw std::runtime_error("invalid");
    return result;
}

// A JSON integer in 0..max (a floating-point representation is refused); throws otherwise.
inline uint64_t Integer(nlohmann::json const& value, uint64_t max)
{
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value < 0))
        throw std::runtime_error("invalid");
    uint64_t const result = value.get<uint64_t>();
    if (result > max)
        throw std::runtime_error("invalid");
    return result;
}

// A received text (a reply, a unit's name): 1..max bytes, no NUL, at least one byte above a space. Line breaks
// stay; the parser already refused invalid UTF-8.
inline bool UsableText(std::string_view text, std::size_t max)
{
    return !text.empty() && text.size() <= max && text.find('\0') == std::string_view::npos &&
           std::any_of(text.begin(), text.end(), [](unsigned char c) { return c > ' '; });
}

// A received name a planner looks up (a place): a usable text without control characters.
inline bool UsableName(std::string_view text, std::size_t max)
{
    return UsableText(text, max) &&
           std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; });
}

// A text the module sends: control characters dropped, cut to maxBytes on a character boundary; none when blank or
// not valid UTF-8 (the envelope dump would throw).
inline std::optional<std::string> WireText(std::string_view value, std::size_t maxBytes)
{
    std::string text;
    for (unsigned char byte : value)
        if (byte >= 0x20 && byte != 0x7F)
            text.push_back(static_cast<char>(byte));
    if (text.size() > maxBytes)
    {
        std::size_t cut = maxBytes;
        while (cut && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
            --cut;
        text.resize(cut);
    }
    if (std::none_of(text.begin(), text.end(), [](unsigned char c) { return c > ' '; }))
        return std::nullopt;
    try
    {
        static_cast<void>(nlohmann::json(text).dump());
    }
    catch (nlohmann::json::exception const&)
    {
        return std::nullopt;
    }
    return text;
}

// The fields every envelope carries; the type's own fields are added by the caller.
inline nlohmann::json Envelope(std::string_view instance, std::string_view provider, std::string_view epoch,
                               std::string_view requestId, int64_t deadlineUnixMs, std::string_view type)
{
    return {{"version", 1},   {"instance", instance},   {"provider", provider},
            {"epoch", epoch}, {"requestId", requestId}, {"deadlineUnixMs", deadlineUnixMs},
            {"type", type}};
}

// The one bearer check, at the listener before any body byte is read: exactly "Bearer " + token. The scheme and
// the length bound are public; the secret bytes are compared in constant time over MaxBearerBytes.
inline bool BearerMatches(std::string_view authorization, std::string_view token)
{
    constexpr std::string_view scheme = "Bearer ";
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
}  // namespace OllamaBridge

#endif
