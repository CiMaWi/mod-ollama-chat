#include "mod-ollama-chat_bridge.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

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
        input.text.size() > MaxTextBytes || input.memory.size() > 8192 || input.groupScope.size() > 128 ||
        input.observations.size() > 32 || input.party.size() > 40)
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

bool Blank(std::string const& value)
{
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
}

// One text on the wire: control characters dropped, cut to maxBytes on a character boundary; none when blank or not
// valid UTF-8 (the envelope dump would throw).
std::optional<std::string> WireText(std::string const& value, std::size_t maxBytes)
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
    if (Blank(text))
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

// A text under its key, left out when WireText gives none.
void PutText(nlohmann::json& json, char const* key, std::string const& value, std::size_t maxBytes)
{
    if (auto text = WireText(value, maxBytes))
        json[key] = std::move(*text);
}

// One fact on the wire: the text rule at MaxFactBytes.
void PutFact(nlohmann::json& context, char const* key, std::string const& value)
{
    PutText(context, key, value, MaxFactBytes);
}

// LC10: a list of texts in the given order, an entry without a text dropped, at most cap entries; left out when empty.
void PutTextList(nlohmann::json& json, char const* key, std::vector<std::string> const& values, std::size_t cap,
                 std::size_t maxBytes)
{
    nlohmann::json list = nlohmann::json::array();
    for (std::string const& value : values)
    {
        if (list.size() == cap)
            break;
        if (auto text = WireText(value, maxBytes))
            list.push_back(std::move(*text));
    }
    if (!list.empty())
        json[key] = std::move(list);
}

// LC15c: `traits.words` and, beside them, `traits.areas`: one area per word, a word dropped (no text) takes its area
// with it so the two stay as long as each other; no areas key unless the game gave one area per word.
void PutTraitWords(nlohmann::json& json, BotTraits const& traits)
{
    bool const withAreas = traits.areas.size() == traits.words.size();
    nlohmann::json words = nlohmann::json::array();
    nlohmann::json areas = nlohmann::json::array();
    for (std::size_t i = 0; i < traits.words.size() && words.size() < MaxTraitWords; ++i)
    {
        auto word = WireText(traits.words[i], MaxTraitBytes);
        if (!word)
            continue;
        words.push_back(std::move(*word));
        if (withAreas)
            if (auto area = WireText(traits.areas[i], MaxTraitBytes))
                areas.push_back(std::move(*area));
    }
    if (words.empty())
        return;
    bool const allAreas = withAreas && areas.size() == words.size();
    json["words"] = std::move(words);
    if (allAreas)
        json["areas"] = std::move(areas);
}

// LC10: a list of {first: fact, second: word} pairs, the same rule; a pair missing either text is dropped.
void PutPairList(nlohmann::json& json, char const* key, std::vector<std::pair<std::string, std::string>> const& values,
                 std::size_t cap, char const* firstKey, char const* secondKey)
{
    nlohmann::json list = nlohmann::json::array();
    for (auto const& [first, second] : values)
    {
        if (list.size() == cap)
            break;
        auto name = WireText(first, MaxFactBytes);
        auto word = WireText(second, MaxFactWordBytes);
        if (name && word)
            list.push_back({{firstKey, std::move(*name)}, {secondKey, std::move(*word)}});
    }
    if (!list.empty())
        json[key] = std::move(list);
}

constexpr std::size_t MaxFactCrafts = 8;
constexpr std::size_t MaxFactUseful = 12;
constexpr std::size_t MaxFactServants = 8;
constexpr std::size_t MaxFactStanding = 6;
constexpr std::size_t MaxFactRelatives = 8;

// LC10 `input.facts`: who the bot is (the fact rule), how it fights, its crafts, standing and kin. False when who
// or the phrase does not survive the bounds; the module then leaves the whole object out.
bool FactsJson(BotWho const& bot, PlayerbotBridge::BotFacts const& facts, nlohmann::json& json)
{
    nlohmann::json who = nlohmann::json::object();
    PutFact(who, "name", bot.name);
    PutFact(who, "race", bot.race);
    PutFact(who, "class", bot.className);
    if (bot.gender == "male" || bot.gender == "female")
        who["gender"] = bot.gender;
    if (bot.level)
        who["level"] = std::min<uint32_t>(bot.level, 255);
    if (bot.team <= 1)
        who["side"] = bot.team ? "Horde" : "Alliance";
    auto phrase = WireText(facts.phrase, MaxPhraseBytes);
    if (who.size() != 6 || !phrase)
        return false;
    json = {{"who", std::move(who)}, {"phrase", std::move(*phrase)}};
    PutPairList(json, "crafts", facts.crafts, MaxFactCrafts, "name", "rank");
    PutTextList(json, "useful", facts.useful, MaxFactUseful, MaxFactBytes);
    PutTextList(json, "servants", facts.servants, MaxFactServants, MaxFactBytes);
    PutPairList(json, "standing", facts.standing, MaxFactStanding, "faction", "word");
    PutFact(json, "guild", facts.guild);
    PutTextList(json, "relatives", facts.relatives, MaxFactRelatives, MaxFactBytes);
    return true;
}

// LCB4: whether an open row's key may go to the wire: [a-z][a-z_]{0,23} and not a key the envelope writes itself.
bool OpenKey(std::string const& key, std::span<std::string_view const> shaped)
{
    return !key.empty() && key.size() <= 24 && key.front() >= 'a' && key.front() <= 'z' &&
           std::all_of(key.begin(), key.end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; }) &&
           std::find(shaped.begin(), shaped.end(), key) == shaped.end();
}

// LCB4: playerbots' open rows of one object, in their order, under the open-key rule (bridge.h); a bad row, a key
// already written and every row past MaxOpenKeys are dropped alone.
void PutOpenKeys(nlohmann::json& json, std::vector<PlayerbotBridge::StateKey> const& rows,
                 std::span<std::string_view const> shaped)
{
    std::size_t kept = 0;
    for (PlayerbotBridge::StateKey const& row : rows)
    {
        if (kept == MaxOpenKeys)
            break;
        if (!OpenKey(row.key, shaped) || json.contains(row.key))
            continue;
        if (bool const* flag = std::get_if<bool>(&row.value))
            json[row.key] = *flag;
        else if (int64_t const* number = std::get_if<int64_t>(&row.value))
            json[row.key] = std::clamp(*number, -MaxOpenNumber, MaxOpenNumber);
        else if (auto text = WireText(std::get<std::string>(row.value), MaxFactBytes))
            json[row.key] = std::move(*text);
        else
            continue;
        ++kept;
    }
}

constexpr std::string_view AroundKeys[] = {"name", "kind", "detail", "count"};
// "plans" names the lasting settings in force, read from the bot (LCB7b; the module keeps no plans, LC.36).
constexpr std::string_view StateKeys[] = {"health",  "power",   "level",    "alive", "combat",   "activity", "where",
                                          "partner", "mounted", "swimming", "group", "movement", "plans",    "bags",
                                          "money",   "around",  "servant",  "mount", "deeds"};

// LC10 `input.state.around`: nearest first, one entry per name (after the bounds), a known kind, count 1-255; LCB4:
// each entry's open rows beside its keys.
nlohmann::json AroundJson(std::vector<PlayerbotBridge::AroundEntry> const& entries)
{
    nlohmann::json around = nlohmann::json::array();
    for (PlayerbotBridge::AroundEntry const& entry : entries)
    {
        if (around.size() == MaxStateAround)
            break;
        auto name = WireText(entry.Name, MaxFactBytes);
        if (!name || entry.Kind > PlayerbotBridge::AroundKind::Object ||
            std::any_of(around.begin(), around.end(),
                        [&name](nlohmann::json const& listed) { return listed.at("name") == *name; }))
            continue;
        nlohmann::json item = {{"name", std::move(*name)}, {"kind", PlayerbotBridge::AroundKindWord(entry.Kind)}};
        PutFact(item, "detail", entry.Detail);
        item["count"] = std::clamp<uint32_t>(entry.Count, 1, 255);
        PutOpenKeys(item, entry.open, AroundKeys);
        around.push_back(std::move(item));
    }
    return around;
}

uint32_t StatePercent(float value)
{
    if (!(value > 0.0f))
        return 0;
    return value >= 100.0f ? 100 : static_cast<uint32_t>(value + 0.5f);
}

// Whole yards; a value that is not finite or beyond any map reads as 0.
int64_t StateYards(float value)
{
    return std::isfinite(value) && std::fabs(value) <= 100000.0f ? std::llround(value) : 0;
}

char const* StatePower(uint8_t type)
{
    switch (type)
    {
        case 0:
            return "mana";
        case 1:
            return "rage";
        case 2:
            return "focus";
        case 3:
            return "energy";
        case 6:
            return "runic";
        default:
            return nullptr;
    }
}

// The most absorbing thing the bot is doing.
char const* StateActivity(PlayerbotBridge::BotStateFacts const& state)
{
    if (!state.alive)
        return "idle";
    if (state.flying)
        return "flying";
    if (state.inCombat)
        return "fighting";
    if (state.casting)
        return "casting";
    if (state.looting)
        return "looting";
    if (state.resting)
        return "resting";
    if ((state.travelling || state.working) && !state.questTitle.empty())
        return "questing";
    if (state.travelling)
        return "travelling";
    return "idle";
}

char const* StateMovement(PlayerbotBridge::BotStateFacts const& state)
{
    return state.stay      ? "stay"
           : state.follow  ? "follow"
           : state.guard   ? "guard"
           : state.grind   ? "grind"
           : state.passive ? "passive"
                           : "none";
}

// DS.14/J5f `input.state`: what the bot itself knows about its condition, place and doings; LCB4: playerbots' open
// rows last.
nlohmann::json StateJson(PlayerbotBridge::BotStateFacts const& state)
{
    nlohmann::json json = nlohmann::json::object();
    json["health"] = StatePercent(state.healthPct);
    if (char const* kind = StatePower(state.powerType); kind && state.powerMax)
        json["power"] = {{"kind", kind},
                         {"pct", StatePercent(100.0f * std::min(state.powerCurrent, state.powerMax) / state.powerMax)}};
    if (state.level)
        json["level"] = std::min<uint32_t>(state.level, 255);
    json["alive"] = state.alive ? "alive" : state.ghost ? "ghost" : "dead";
    // Only the bot's own victim, and only while the bot can see it.
    if (state.alive && state.inCombat && state.victim && state.victimVisible)
    {
        nlohmann::json target = nlohmann::json::object();
        PutFact(target, "name", state.victimName);
        if (state.victimLevel)
            target["level"] = std::min<uint32_t>(state.victimLevel, 255);
        target["health"] = StatePercent(state.victimHealthPct);
        json["combat"] = {{"target", std::move(target)}};
    }
    char const* activity = StateActivity(state);
    nlohmann::json doing = {{"kind", activity}};
    if (std::string_view(activity) == "questing")
        PutFact(doing, "detail", state.questTitle);
    json["activity"] = std::move(doing);
    nlohmann::json where = nlohmann::json::object();
    PutFact(where, "zone", state.zone);
    if (state.area != state.zone)
        PutFact(where, "area", state.area);
    where["mapId"] = state.mapId;
    where["x"] = StateYards(state.x);
    where["y"] = StateYards(state.y);
    where["z"] = StateYards(state.z);
    json["where"] = std::move(where);
    // The partner only while in the bot's group or in its sight; distance and position only on the same map.
    if (state.partnerOnline && (state.partnerInGroup || state.partnerVisible))
    {
        nlohmann::json partner = nlohmann::json::object();
        PutFact(partner, "zone", state.partnerZone);
        if (state.partnerSameMap)
        {
            partner["yards"] = std::max<int64_t>(StateYards(state.partnerYards), 0);
            partner["x"] = StateYards(state.partnerX);
            partner["y"] = StateYards(state.partnerY);
            partner["z"] = StateYards(state.partnerZ);
        }
        json["partner"] = std::move(partner);
    }
    if (state.mounted)
        json["mounted"] = true;
    if (state.swimming)
        json["swimming"] = true;
    if (state.groupSize)
    {
        nlohmann::json group = {{"size", std::min<uint32_t>(state.groupSize, 40)}, {"leads", state.leads}};
        if (state.follow)
            PutFact(group, "follows", state.masterName);
        json["group"] = std::move(group);
    }
    json["movement"] = StateMovement(state);
    // LC.36: a bot can revert only what it sees, so each lasting setting in force is named while it holds, in the
    // hub's plan order (envelope.mjs PLAN_KINDS); follow and stay are the movement above.
    nlohmann::json plans = nlohmann::json::array();
    if (state.formation)
        plans.push_back("formation");
    if (state.saveMana)
        plans.push_back("save_mana");
    if (state.focusHeal)
        plans.push_back("focus_healing");
    if (!plans.empty())
        json["plans"] = std::move(plans);
    json["bags"] = std::min<uint32_t>(state.freeBagSlots, 255);
    json["money"] = state.money;
    if (nlohmann::json around = AroundJson(state.around); !around.empty())
        json["around"] = std::move(around);
    PutFact(json, "servant", state.servantName);
    PutFact(json, "mount", state.mountName);
    // Whole record sentences (LC7's cap), never the 64-byte fact rule.
    PutTextList(json, "deeds", state.deeds, MaxStateDeeds, MaxStateDeedBytes);
    PutOpenKeys(json, state.open, StateKeys);
    return json;
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

bool BridgeUsable(bool enabled, Settings const& settings) { return enabled && ValidSettings(settings); }

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
        nlohmann::json envelope = {{"version", 1},
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
                                     {"memory", input.memory},
                                     {"groupScope", input.groupScope},
                                     {"directAddress", input.directAddress},
                                     {"speakerControlsBot", input.speakerControlsBot},
                                     {"party", std::move(party)}}}};
        // LC10: the lasting facts and the trait words, each only when there is something to say.
        if (nlohmann::json facts; input.facts && FactsJson(input.who, *input.facts, facts))
            envelope["input"]["facts"] = std::move(facts);
        nlohmann::json traits = nlohmann::json::object();
        PutTraitWords(traits, input.traits);
        PutTextList(traits, "interests", input.traits.interests, MaxTraitInterests, MaxTraitBytes);
        if (!traits.empty())
            envelope["input"]["traits"] = std::move(traits);
        if (input.state)
            envelope["input"]["state"] = StateJson(*input.state);
        if (!input.wakeCondition.empty())  // J6e-c: a wake turn (never addressed); the watch that opened it.
            envelope["input"]["wake"] = {{"condition", input.wakeCondition}, {"watchId", input.wakeWatchId}};
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
        if (!parsed.is_object() || parsed.size() != 8u || !parsed.contains("version") ||
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
        // DS.14: exactly one of text or "silent": true (an action-only answer).
        bool const silent = parsed.contains("silent");
        if (!MatchesString(parsed, "type", "generation.result") || silent == parsed.contains("text"))
            return result;
        if (silent ? !parsed["silent"].is_boolean() || !parsed["silent"].get<bool>() : !parsed["text"].is_string())
            return result;
        static std::string const wordless;
        auto const& text = silent ? wordless : parsed["text"].get_ref<std::string const&>();
        if (!silent &&
            (text.empty() || text.size() > MaxTextBytes || Blank(text) || text.find('\0') != std::string::npos))
            return result;
        result.ok = true;
        result.silent = silent;
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

uint8_t ActivityCode(PlayerbotBridge::BotStateFacts const& state)
{
    constexpr std::string_view kinds[] = {"idle",    "flying",  "fighting", "casting",
                                          "looting", "resting", "questing", "travelling"};
    std::string_view const kind = StateActivity(state);
    for (std::size_t code = 0; code < std::size(kinds); ++code)
        if (kinds[code] == kind)
            return static_cast<uint8_t>(code);
    return 0;
}
}  // namespace OllamaBridge
