#include "mod-ollama-chat_bridge_channel.h"

#include <algorithm>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace OllamaBridge
{
namespace
{
using Json = nlohmann::json;
constexpr int64_t MaxSafeInteger = 9007199254740991LL;
constexpr int64_t ScopeLifetimeMs = 5000;

bool Identifier(std::string const& value)
{
    auto alnum = [](unsigned char c)
    { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    return !value.empty() && value.size() <= 128 && alnum(value.front()) &&
           std::all_of(value.begin(), value.end(),
                       [&](unsigned char c) { return alnum(c) || c == '_' || c == '.' || c == ':' || c == '-'; });
}

bool SameScope(WorldScope const& a, WorldScope const& b)
{
    return std::tie(a.version, a.instance, a.provider, a.epoch, a.botGuid, a.controllerGuid, a.contextId, a.channel,
                    a.perceptionScope, a.contextRevision, a.membershipRevision, a.personaRevision) ==
           std::tie(b.version, b.instance, b.provider, b.epoch, b.botGuid, b.controllerGuid, b.contextId, b.channel,
                    b.perceptionScope, b.contextRevision, b.membershipRevision, b.personaRevision);
}

bool ValidDeadline(Deadline deadline, Clock now)
{
    return now.unixMs >= 0 && now.unixMs <= MaxSafeInteger && now.steadyMs >= 0 && deadline.unixMs > now.unixMs &&
           deadline.unixMs <= MaxSafeInteger && deadline.unixMs - now.unixMs <= MaxLifetimeMs &&
           deadline.steadyMs > now.steadyMs && deadline.steadyMs - now.steadyMs <= deadline.unixMs - now.unixMs;
}

bool Alive(Deadline deadline, Clock now)
{
    return now.unixMs >= 0 && now.steadyMs >= 0 && now.unixMs < deadline.unixMs && now.steadyMs < deadline.steadyMs;
}

bool Exact(Json const& value, std::initializer_list<char const*> fields)
{
    return value.is_object() && value.size() == fields.size() &&
           std::all_of(fields.begin(), fields.end(), [&](char const* field) { return value.contains(field); });
}

std::string String(Json const& value)
{
    if (!value.is_string())
        throw std::runtime_error("invalid");
    return value.get<std::string>();
}

uint64_t Guid(Json const& value)
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

uint64_t Integer(Json const& value, uint64_t max)
{
    if (!value.is_number_integer() || (value.is_number_integer() && !value.is_number_unsigned() && value < 0))
        throw std::runtime_error("invalid");
    uint64_t const result = value.get<uint64_t>();
    if (result > max)
        throw std::runtime_error("invalid");
    return result;
}

bool Text(std::string const& text, std::size_t max)
{
    return !text.empty() && text.size() <= max && text.find('\0') == std::string::npos &&
           std::any_of(text.begin(), text.end(), [](unsigned char c) { return c > ' '; });
}

// WT18b: a place name (route query, go_to, meet_at): text of 1..64 bytes, not blank, no control characters (the
// parser already refused invalid UTF-8). It is a value the planner looks up, never command syntax.
std::string Place(Json const& value)
{
    auto const text = String(value);
    if (!Text(text, ChannelMaxPlaceName) ||
        std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; }))
        throw std::runtime_error("invalid");
    return text;
}

// J5e: a standing follow/stay shape (any params) is refused with its own reason, still invalid_input on the wire.
struct StandingShape : std::runtime_error
{
    StandingShape() : std::runtime_error("standing") {}
};

ChannelCommand Command(Json const& body)
{
    if (!Exact(body, {"version", "instance", "provider", "epoch", "requestId", "deadlineUnixMs", "type", "commandId",
                      "issuerGuid", "botGuid", "intent", "params"}))
        throw std::runtime_error("invalid");
    ChannelCommand command;
    command.commandId = String(body.at("commandId"));
    command.issuerGuid = Guid(body.at("issuerGuid"));
    command.botGuid = Guid(body.at("botGuid"));
    if (!Identifier(command.commandId))
        throw std::runtime_error("invalid");
    auto const intent = String(body.at("intent"));
    auto const& params = body.at("params");
    constexpr std::pair<char const*, ChannelIntent> intents[] = {{"follow", ChannelIntent::Follow},
                                                                 {"stay", ChannelIntent::Stay},
                                                                 {"flee", ChannelIntent::Flee},
                                                                 {"regroup", ChannelIntent::Regroup},
                                                                 {"attack_own_target", ChannelIntent::AttackOwnTarget},
                                                                 {"pull", ChannelIntent::Pull},
                                                                 {"mark", ChannelIntent::Mark},
                                                                 {"focus_healing", ChannelIntent::FocusHealing},
                                                                 {"assign_role", ChannelIntent::AssignRole},
                                                                 {"save_mana", ChannelIntent::SaveMana},
                                                                 {"formation", ChannelIntent::Formation},
                                                                 {"position", ChannelIntent::Position},
                                                                 {"dungeon_clear", ChannelIntent::DungeonClear},
                                                                 {"go_to", ChannelIntent::GoTo},
                                                                 {"quest", ChannelIntent::Quest},
                                                                 {"errands", ChannelIntent::Errands},
                                                                 {"set_hearth", ChannelIntent::SetHearth},
                                                                 {"grind_here", ChannelIntent::GrindHere},
                                                                 {"gather_here", ChannelIntent::GatherHere},
                                                                 {"meet_at", ChannelIntent::MeetAt},
                                                                 {"pause_plan", ChannelIntent::PausePlan},
                                                                 {"resume_plan", ChannelIntent::ResumePlan}};
    auto const found =
        std::find_if(std::begin(intents), std::end(intents), [&](auto const& entry) { return intent == entry.first; });
    if (found == std::end(intents))
        throw std::runtime_error("invalid");
    command.intent = found->second;
    switch (command.intent)
    {
        case ChannelIntent::Follow:
        case ChannelIntent::Stay:
            // J5e: params with keys (a standing or timed shape) name their reason; any other shape stays invalid.
            if (!Exact(params, {}))
            {
                if (params.is_object())
                    throw StandingShape();
                throw std::runtime_error("invalid");
            }
            break;
        case ChannelIntent::Flee:
        case ChannelIntent::Regroup:
        case ChannelIntent::AttackOwnTarget:
        case ChannelIntent::Errands:
        case ChannelIntent::SetHearth:
        case ChannelIntent::GrindHere:
        case ChannelIntent::GatherHere:
        case ChannelIntent::PausePlan:
        case ChannelIntent::ResumePlan:
            if (!Exact(params, {}))
                throw std::runtime_error("invalid");
            break;
        case ChannelIntent::GoTo:  // WT18b: the planner looks the place up and refuses an unknown one.
        case ChannelIntent::MeetAt:
            if (!Exact(params, {"place"}))
                throw std::runtime_error("invalid");
            command.params = ChannelPlace{Place(params.at("place"))};
            break;
        case ChannelIntent::Quest:  // WT18b: a quest id; zero is refused by ValidIntentParams.
            if (!Exact(params, {"questId"}))
                throw std::runtime_error("invalid");
            command.params = ChannelQuestId{static_cast<uint32_t>(Integer(params.at("questId"), UINT32_MAX))};
            break;
        case ChannelIntent::Pull:
        case ChannelIntent::FocusHealing:
            if (!Exact(params, {"targetGuid"}))
                throw std::runtime_error("invalid");
            command.params = ChannelTarget{Guid(params.at("targetGuid"))};
            break;
        case ChannelIntent::Mark:
            if (!Exact(params, {"targetGuid", "icon"}))
                throw std::runtime_error("invalid");
            command.params =
                ChannelMark{Guid(params.at("targetGuid")), static_cast<uint8_t>(Integer(params.at("icon"), 7))};
            break;
        case ChannelIntent::AssignRole:
        {
            if (!Exact(params, {"role"}))
                throw std::runtime_error("invalid");
            auto const role = String(params.at("role"));
            if (role != "tank" && role != "healer" && role != "damage")
                throw std::runtime_error("invalid");
            command.params = role == "tank"     ? ChannelRole::Tank
                             : role == "healer" ? ChannelRole::Healer
                                                : ChannelRole::Damage;
            break;
        }
        case ChannelIntent::SaveMana:
            if (!Exact(params, {"enabled"}) || !params.at("enabled").is_boolean())
                throw std::runtime_error("invalid");
            command.params = params.at("enabled").get<bool>();
            break;
        case ChannelIntent::Formation:
        {
            if (!Exact(params, {"shape"}))
                throw std::runtime_error("invalid");
            auto const shape = String(params.at("shape"));
            if (shape != "close" && shape != "spread" && shape != "line")
                throw std::runtime_error("invalid");
            command.params = shape == "close"    ? ChannelShape::Close
                             : shape == "spread" ? ChannelShape::Spread
                                                 : ChannelShape::Line;
            break;
        }
        case ChannelIntent::Position:
        {
            if (!Exact(params, {"x", "y", "z", "mapId"}) || !params.at("x").is_number() ||
                !params.at("y").is_number() || !params.at("z").is_number())
                throw std::runtime_error("invalid");
            ChannelPosition position{params.at("x").get<double>(), params.at("y").get<double>(),
                                     params.at("z").get<double>(),
                                     static_cast<uint32_t>(Integer(params.at("mapId"), UINT32_MAX))};
            command.params = position;
            break;
        }
        case ChannelIntent::DungeonClear:
        {
            if (!Exact(params, {"mode"}))
                throw std::runtime_error("invalid");
            auto const mode = String(params.at("mode"));
            if (mode != "on" && mode != "off" && mode != "pause")
                throw std::runtime_error("invalid");
            command.params = mode == "on" ? ChannelMode::On : mode == "off" ? ChannelMode::Off : ChannelMode::Pause;
            break;
        }
    }
    if (!ValidIntentParams(command.intent, command.params))
        throw std::runtime_error("invalid");
    return command;
}

// J6e-c: a watch on the route's own bot; condition one of four names, untilUnixMs an absolute UTC time in ms.
ChannelWatch WatchMessage(Json const& body)
{
    if (!Exact(body, {"version", "instance", "provider", "epoch", "requestId", "deadlineUnixMs", "type", "commandId",
                      "botGuid", "condition", "untilUnixMs"}))
        throw std::runtime_error("invalid");
    ChannelWatch watch;
    watch.commandId = String(body.at("commandId"));
    watch.botGuid = Guid(body.at("botGuid"));
    watch.untilUnixMs = static_cast<int64_t>(Integer(body.at("untilUnixMs"), MaxSafeInteger));
    if (!Identifier(watch.commandId) || !ParseWatchCondition(String(body.at("condition")), watch.condition))
        throw std::runtime_error("invalid");
    return watch;
}

Json EnvelopeJson(ChannelEnvelope const& envelope, char const* type)
{
    return {{"version", 1},
            {"instance", envelope.instance},
            {"provider", envelope.provider},
            {"epoch", envelope.epoch},
            {"requestId", envelope.requestId},
            {"deadlineUnixMs", envelope.deadlineUnixMs},
            {"type", type}};
}
// J5f-b: valid UTF-8 without control characters, cut to max bytes on a character boundary; empty when unusable.
std::string TopicText(std::string const& text, std::size_t max)
{
    if (std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; }))
        return {};
    std::size_t end = std::min(max, text.size());
    while (end > 0 && end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
        --end;
    std::string result = text.substr(0, end);
    try
    {
        (void)Json(result).dump();  // Throws on invalid UTF-8.
    }
    catch (std::exception const&)
    {
        return {};
    }
    return Text(result, max) ? result : std::string();
}

// WT18b: a planner topic's row word, as StateTopicRow names it.
bool PlannerRow(StateTopic topic, std::string const& kind)
{
    for (uint8_t code = 0; code < 8; ++code)
    {
        std::string const word = StateTopicRow(topic, code);
        if (!word.empty() && word == kind)
            return true;
    }
    return false;
}

// Gameplay strategies the bot can talk about; every other playerbots strategy name is dropped.
bool StrategyAllowed(std::string const& name)
{
    static constexpr char const* allowed[] = {"tank",
                                              "tank assist",
                                              "heal",
                                              "dps",
                                              "dps assist",
                                              "dps aoe",
                                              "aoe",
                                              "cure",
                                              "buff",
                                              "boost",
                                              "cc",
                                              "threat",
                                              "potions",
                                              "food",
                                              "loot",
                                              "gather",
                                              "passive",
                                              "aggressive",
                                              "save mana",
                                              "close",
                                              "ranged",
                                              "behind",
                                              "avoid aoe",
                                              "kite",
                                              "mount",
                                              "quest",
                                              "grind",
                                              "rpg",
                                              "new rpg",
                                              "travel",
                                              "explore",
                                              "attack tagged",
                                              "follow",
                                              "stay",
                                              "guard",
                                              "runaway",
                                              "focus heal targets",
                                              "sit"};
    return std::any_of(std::begin(allowed), std::end(allowed), [&](char const* item) { return name == item; });
}

bool ParseTopic(std::string const& name, StateTopic& topic)
{
    for (std::size_t i = 0; i < StateTopicCount; ++i)
        if (name == StateTopicName(static_cast<StateTopic>(i)))
        {
            topic = static_cast<StateTopic>(i);
            return true;
        }
    return false;
}

// One entry with its topic's fixed keys, or false when the entry is unusable (dropped, never rejected whole).
bool TopicEntry(StateTopic topic, StateEntry const& entry, Json& value)
{
    auto const name = TopicText(entry.name, ChannelMaxTopicName);
    if (name.empty())
        return false;
    switch (topic)
    {
        case StateTopic::Inventory:
        {
            static constexpr char const* qualities[] = {"poor", "common",    "uncommon", "rare",
                                                        "epic", "legendary", "artifact", "heirloom"};
            if ((entry.kind != "equipped" && entry.kind != "bag") || entry.first < 0 || entry.first > 7)
                return false;
            value = {{"where", entry.kind},
                     {"name", name},
                     {"quality", qualities[entry.first]},
                     {"count", std::clamp<int64_t>(entry.second, 1, 1000)}};
            return true;
        }
        case StateTopic::Quests:
        {
            if (entry.kind != "active" && entry.kind != "complete" && entry.kind != "failed")
                return false;
            auto const needed = std::clamp<int64_t>(entry.second, 0, 10000);
            value = {{"title", name},
                     {"status", entry.kind},
                     {"done", std::clamp<int64_t>(entry.first, 0, needed)},
                     {"needed", needed}};
            return true;
        }
        case StateTopic::Cooldowns:
            if (entry.first <= 0)
                return false;
            value = {{"name", name}, {"seconds", std::min<int64_t>(entry.first, 604800)}};
            return true;
        case StateTopic::Strategies:
            if (!StrategyAllowed(entry.name) || (entry.kind != "combat" && entry.kind != "non_combat"))
                return false;
            value = {{"name", entry.name}, {"kind", entry.kind}};
            return true;
        case StateTopic::Spells:
            value = {{"name", name}, {"rank", std::clamp<int64_t>(entry.first, 0, 99)}};
            return true;
        case StateTopic::Plan:  // WT18b: one shape for the planner's topics; the row word says what a row is.
        case StateTopic::Options:
        case StateTopic::Route:
        case StateTopic::QuestPlan:
        case StateTopic::Bags:
        case StateTopic::Places:
            if (!PlannerRow(topic, entry.kind))
                return false;
            value = {{"row", entry.kind},
                     {"name", name},
                     {"value", std::clamp<int64_t>(entry.first, 0, UINT32_MAX)},
                     {"extra", std::clamp<int64_t>(entry.second, 0, UINT32_MAX)}};
            return true;
    }
    return false;
}
}  // namespace

char const* StateTopicName(StateTopic topic)
{
    switch (topic)
    {
        case StateTopic::Inventory:
            return "inventory";
        case StateTopic::Quests:
            return "quests";
        case StateTopic::Cooldowns:
            return "cooldowns";
        case StateTopic::Strategies:
            return "strategies";
        case StateTopic::Spells:
            return "spells";
        case StateTopic::Plan:
            return "plan";
        case StateTopic::Options:
            return "options";
        case StateTopic::Route:
            return "route";
        case StateTopic::QuestPlan:
            return "quest_plan";
        case StateTopic::Bags:
            return "bags";
        case StateTopic::Places:
            return "places";
    }
    return "";
}

char const* StateTopicRow(StateTopic topic, uint8_t code)
{
    static constexpr char const* plan[] = {"goal", "source", "reason", "stop"};
    static constexpr char const* options[] = {"option", "current"};
    static constexpr char const* route[] = {"total", "leg", "failed"};
    static constexpr char const* questPlan[] = {"take", "task", "turn_in", "carried", "deferred"};
    static constexpr char const* bags[] = {"free", "sell", "bag"};
    static constexpr char const* places[] = {"hearth", "flight_here", "flight_elsewhere"};
    auto const pick = [code](auto const& words) -> char const* { return code < std::size(words) ? words[code] : ""; };
    switch (topic)
    {
        case StateTopic::Plan:
            return pick(plan);
        case StateTopic::Options:
            return pick(options);
        case StateTopic::Route:
            return pick(route);
        case StateTopic::QuestPlan:
            return pick(questPlan);
        case StateTopic::Bags:
            return pick(bags);
        case StateTopic::Places:
            return pick(places);
        case StateTopic::Inventory:
        case StateTopic::Quests:
        case StateTopic::Cooldowns:
        case StateTopic::Strategies:
        case StateTopic::Spells:
            break;
    }
    return "";
}

std::size_t StateTopicLimit(StateTopic topic)
{
    switch (topic)
    {
        case StateTopic::Inventory:
            return 40;
        case StateTopic::Quests:
            return 25;
        case StateTopic::Cooldowns:
            return 20;
        case StateTopic::Strategies:
            return 40;
        case StateTopic::Spells:
            return 40;
        case StateTopic::Plan:  // WT18b: the limits playerbots' topic readers keep.
            return 8;
        case StateTopic::Options:
            return 5;
        case StateTopic::Route:
            return 12;
        case StateTopic::QuestPlan:
            return 12;
        case StateTopic::Bags:
            return 6;
        case StateTopic::Places:
            return 24;
    }
    return 0;
}

char const* ChannelErrorName(ChannelError error)
{
    switch (error)
    {
        case ChannelError::None:
            return "ok";
        case ChannelError::Unauthorized:
            return "unauthorized";
        case ChannelError::Inactive:
            return "inactive";
        case ChannelError::Invalid:
            return "invalid_input";
        case ChannelError::TooLarge:
            return "too_large";
        case ChannelError::Stale:
            return "stale";
        case ChannelError::Duplicate:
            return "duplicate";
        case ChannelError::Capacity:
            return "capacity";
        case ChannelError::Closed:
            return "closed";
        case ChannelError::Unknown:
            return "unknown_request";
        case ChannelError::Standing:
            return "invalid_input";
    }
    return "invalid_input";
}

ChannelPolicy::ChannelPolicy(std::size_t maxIds, std::size_t maxPending)
    : _validLimits(maxIds > 0 && maxIds <= ChannelMaxIds && maxPending > 0 && maxPending <= ChannelMaxPending),
      _maxIds(maxIds),
      _maxPending(maxPending)
{
}

void ChannelPolicy::Retire()
{
    _identity = {};
    _observed = {};
    _bearer.clear();
    _scopes.clear();
    _requests.clear();
    _commands.clear();
}

bool ChannelPolicy::Bind(Identity identity, std::string localBearer)
{
    Retire();
    if (!_validLimits || !Identifier(identity.instance) || !Identifier(identity.provider) ||
        !Identifier(identity.epoch) || localBearer.size() < 16 || localBearer.size() > 256 ||
        _epochs.size() >= ChannelMaxIds || !_epochs.insert(identity.epoch).second)
        return false;
    _identity = std::move(identity);
    _bearer = std::move(localBearer);
    return true;
}

bool ChannelPolicy::Authenticated(std::string const& bearer) const
{
    if (_bearer.empty() || bearer.size() > 256)
        return false;
    std::size_t difference = _bearer.size() ^ bearer.size();
    for (std::size_t i = 0; i < 256; ++i)
        difference |= (i < _bearer.size() ? static_cast<unsigned char>(_bearer[i]) : 0) ^
                      (i < bearer.size() ? static_cast<unsigned char>(bearer[i]) : 0);
    return difference == 0;
}

void ChannelPolicy::RemoveScope(uint64_t botGuid, std::string const& contextId)
{
    std::erase_if(_scopes, [&](ScopeEntry const& entry)
                  { return entry.scope.botGuid == botGuid && entry.scope.contextId == contextId; });
    for (auto& [id, entry] : _requests)
        if (entry.scope.botGuid == botGuid && entry.scope.contextId == contextId)
            entry.invalid = true;
}

void ChannelPolicy::Expire(Clock now)
{
    _observed = {std::max(_observed.unixMs, now.unixMs), std::max(_observed.steadyMs, now.steadyMs)};
    for (auto& [id, entry] : _requests)
        if (!Alive(entry.until, now) || !Alive(entry.until, _observed) || !ScopeCurrent(entry.scope, now))
            entry.invalid = true;
    std::erase_if(_scopes, [&](ScopeEntry const& entry) { return !Alive(entry.until, now); });
}

bool ChannelPolicy::ObserveScope(WorldScope scope, Clock now)
{
    _observed = {std::max(_observed.unixMs, now.unixMs), std::max(_observed.steadyMs, now.steadyMs)};
    // Heartbeats run on the world tick. Scan only the bounded scopes, not every retained replay reservation.
    // A genuine observation gap still permanently invalidates work before a matching scope can be refreshed.
    for (std::size_t i = 0; i < _scopes.size();)
        if (!Alive(_scopes[i].until, now) || !Alive(_scopes[i].until, _observed))
        {
            auto const expired = _scopes[i].scope;
            RemoveScope(expired.botGuid, expired.contextId);
        }
        else
            ++i;
    if (_bearer.empty() || scope.version != 1 || scope.instance != _identity.instance ||
        scope.provider != _identity.provider || scope.epoch != _identity.epoch || !scope.botGuid ||
        !scope.controllerGuid || !Identifier(scope.contextId) || !Identifier(scope.perceptionScope) ||
        !Identifier(scope.contextRevision) || !Identifier(scope.membershipRevision) ||
        !Identifier(scope.personaRevision) || (scope.channel != "direct" && scope.channel != "party") ||
        now.unixMs < 0 || now.unixMs > MaxSafeInteger - ScopeLifetimeMs || now.steadyMs < 0 ||
        now.steadyMs > std::numeric_limits<int64_t>::max() - ScopeLifetimeMs)
        return false;
    auto found =
        std::find_if(_scopes.begin(), _scopes.end(), [&](ScopeEntry const& entry)
                     { return entry.scope.botGuid == scope.botGuid && entry.scope.contextId == scope.contextId; });
    Deadline const until{now.unixMs + ScopeLifetimeMs, now.steadyMs + ScopeLifetimeMs};
    if (found != _scopes.end() && SameScope(found->scope, scope))
    {
        found->until = until;
        return true;
    }
    if (found != _scopes.end())
        RemoveScope(scope.botGuid, scope.contextId);
    if (_scopes.size() >= ChannelMaxScopes)
        return false;
    _scopes.push_back({std::move(scope), until});
    return true;
}

bool ChannelPolicy::ScopeCurrent(WorldScope const& scope, Clock now) const
{
    return !_bearer.empty() &&
           std::any_of(
               _scopes.begin(), _scopes.end(), [&](ScopeEntry const& entry)
               { return SameScope(entry.scope, scope) && Alive(entry.until, now) && Alive(entry.until, _observed); });
}

std::size_t ChannelPolicy::Pending() const
{
    return std::count_if(
        _requests.begin(), _requests.end(), [&](auto const& pair)
        { return !pair.second.terminal && !pair.second.invalid && Alive(pair.second.until, _observed); });
}

ChannelError ChannelPolicy::Open(WorldScope const& scope, std::string requestId, Deadline deadline, ChannelWork work,
                                 Clock now)
{
    Expire(now);
    if (!ScopeCurrent(scope, now))
        return ChannelError::Stale;
    if (!Identifier(requestId) || !ValidDeadline(deadline, now))
        return ChannelError::Invalid;
    if (_requests.contains(requestId))
        return ChannelError::Duplicate;
    if (!Alive(deadline, _observed))
        return ChannelError::Stale;
    if (_requests.size() >= _maxIds || Pending() >= _maxPending)
        return ChannelError::Capacity;
    ChannelEnvelope envelope{_identity.instance, _identity.provider, _identity.epoch, requestId, deadline.unixMs};
    _requests.emplace(std::move(requestId), RequestEntry{std::move(envelope), scope, deadline, work});
    return ChannelError::None;
}

ChannelError ChannelPolicy::OpenGeneration(WorldScope const& scope, std::string requestId, Deadline deadline, Clock now)
{
    return Open(scope, std::move(requestId), deadline, ChannelWork::Generation, now);
}

ChannelError ChannelPolicy::OpenEvents(WorldScope const& scope, std::string requestId, Deadline deadline,
                                       std::vector<ChannelObservedMark> const& events, Clock now, std::string& body)
{
    body.clear();
    if (events.empty() || events.size() > ChannelMaxEvents || !Identifier(requestId) || !ScopeCurrent(scope, now) ||
        !ValidDeadline(deadline, now))
        return ChannelError::Invalid;
    Json observations = Json::array();
    for (auto const& event : events)
    {
        if (!event.targetGuid || event.icon > 7)
            return ChannelError::Invalid;
        observations.push_back(
            {{"kind", "observed_mark"}, {"targetGuid", std::to_string(event.targetGuid)}, {"icon", event.icon}});
    }
    ChannelEnvelope envelope{scope.instance, scope.provider, scope.epoch, requestId, deadline.unixMs};
    auto json = EnvelopeJson(envelope, "events");
    json["controllerGuid"] = std::to_string(scope.controllerGuid);
    json["botGuid"] = std::to_string(scope.botGuid);
    json["events"] = std::move(observations);
    auto encoded = json.dump();
    if (encoded.size() > MaxRequestBytes)
        return ChannelError::TooLarge;
    auto const result = Open(scope, std::move(requestId), deadline, ChannelWork::Events, now);
    if (result == ChannelError::None)
        body = std::move(encoded);
    return result;
}

bool ChannelPolicy::Current(ChannelEnvelope const& envelope, WorldScope const& current, Clock now,
                            bool requireOpen) const
{
    auto const found = _requests.find(envelope.requestId);
    if (found == _requests.end())
        return false;
    auto const& entry = found->second;
    return !entry.invalid && (!requireOpen || !entry.terminal) && envelope.instance == _identity.instance &&
           envelope.provider == _identity.provider && envelope.epoch == _identity.epoch &&
           envelope.deadlineUnixMs == entry.envelope.deadlineUnixMs && SameScope(entry.scope, current) &&
           ScopeCurrent(current, now) && Alive(entry.until, now) && Alive(entry.until, _observed);
}

ChannelError ChannelPolicy::Receive(std::string const& localBearer, std::string const& body, WorldScope const& current,
                                    Clock now, ChannelMessage& message, std::string const& expectedRequestId)
{
    message = {};
    if (!Authenticated(localBearer))
        return ChannelError::Unauthorized;
    if (body.size() > MaxRequestBytes)
        return ChannelError::TooLarge;
    try
    {
        std::map<int, std::set<std::string>> keys;
        auto callback = [&](int depth, Json::parse_event_t event, Json& value)
        {
            if (depth > 16)
                throw std::runtime_error("invalid");
            if (event == Json::parse_event_t::object_start)
                keys[depth + 1].clear();
            if (event == Json::parse_event_t::key && !keys[depth].insert(value.get<std::string>()).second)
                throw std::runtime_error("invalid");
            return true;
        };
        auto const json = Json::parse(body, callback);
        if (!json.is_object() || Integer(json.at("version"), 1) != 1)
            return ChannelError::Invalid;
        ChannelEnvelope envelope{String(json.at("instance")), String(json.at("provider")), String(json.at("epoch")),
                                 String(json.at("requestId")),
                                 static_cast<int64_t>(Integer(json.at("deadlineUnixMs"), MaxSafeInteger))};
        if (!Identifier(envelope.requestId))
            return ChannelError::Invalid;
        if (envelope.instance != _identity.instance || envelope.provider != _identity.provider ||
            envelope.epoch != _identity.epoch)
            return ChannelError::Stale;
        auto const type = String(json.at("type"));
        ChannelPayload payload;
        if (type == "query")
        {
            auto const kind = String(json.at("query"));
            ChannelQuery query;
            if (kind == "bot_state")
            {
                // J5f-b: one topic of the bot's own state; an unknown topic is invalid_input. WT18b: route, and
                // only route, names the place it goes to.
                if (!ParseTopic(String(json.at("topic")), query.topic))
                    return ChannelError::Invalid;
                if (query.topic != StateTopic::Route
                        ? !Exact(json, {"version", "instance", "provider", "epoch", "requestId", "deadlineUnixMs",
                                        "type", "controllerGuid", "botGuid", "query", "topic"})
                        : !Exact(json, {"version", "instance", "provider", "epoch", "requestId", "deadlineUnixMs",
                                        "type", "controllerGuid", "botGuid", "query", "topic", "place"}))
                    return ChannelError::Invalid;
                if (query.topic == StateTopic::Route)
                    query.place = Place(json.at("place"));
                query.kind = QueryKind::BotState;
            }
            else if (kind != "observable_group" ||
                     !Exact(json, {"version", "instance", "provider", "epoch", "requestId", "deadlineUnixMs", "type",
                                   "controllerGuid", "botGuid", "query"}))
                return ChannelError::Invalid;
            query.controllerGuid = Guid(json.at("controllerGuid"));
            query.botGuid = Guid(json.at("botGuid"));
            if (query.controllerGuid != current.controllerGuid || query.botGuid != current.botGuid)
                return ChannelError::Stale;
            if (now.unixMs < 0 || now.steadyMs < 0 || envelope.deadlineUnixMs <= now.unixMs ||
                envelope.deadlineUnixMs - now.unixMs > MaxLifetimeMs ||
                now.steadyMs > std::numeric_limits<int64_t>::max() - (envelope.deadlineUnixMs - now.unixMs))
                return ChannelError::Stale;
            Deadline const deadline{envelope.deadlineUnixMs, now.steadyMs + (envelope.deadlineUnixMs - now.unixMs)};
            auto const result = Open(current, envelope.requestId, deadline, ChannelWork::Query, now);
            if (result != ChannelError::None)
                return result;
            message = {std::move(envelope), query};
            return ChannelError::None;
        }
        if (envelope.requestId != expectedRequestId)
            return ChannelError::Invalid;
        // Fully validate syntax before reserving any replay id or closing a request.
        if (type == "command")
            payload = Command(json);
        else if (type == "watch")
            payload = WatchMessage(json);
        else if (type == "generation.result")
        {
            if (!Exact(json,
                       {"version", "instance", "provider", "epoch", "requestId", "deadlineUnixMs", "type", "text"}))
                return ChannelError::Invalid;
            auto text = String(json.at("text"));
            if (!Text(text, MaxTextBytes))
                return ChannelError::Invalid;
            payload = ChannelText{std::move(text)};
        }
        else if (type == "events.result")
        {
            if (!Exact(json, {"version", "instance", "provider", "epoch", "requestId", "deadlineUnixMs", "type"}))
                return ChannelError::Invalid;
            payload = ChannelAck{};
        }
        else if (type == "failure")
        {
            if (!Exact(json,
                       {"version", "instance", "provider", "epoch", "requestId", "deadlineUnixMs", "type", "code"}))
                return ChannelError::Invalid;
            auto const code = String(json.at("code"));
            if (code != "unavailable" && code != "timeout" && code != "invalid_input" && code != "cancelled")
                return ChannelError::Invalid;
            payload = code == "unavailable"     ? ChannelFailure::Unavailable
                      : code == "timeout"       ? ChannelFailure::Timeout
                      : code == "invalid_input" ? ChannelFailure::InvalidInput
                                                : ChannelFailure::Cancelled;
        }
        else
            return ChannelError::Invalid;
        Expire(now);
        auto found = _requests.find(envelope.requestId);
        if (found == _requests.end())
            return ChannelError::Unknown;
        auto& entry = found->second;
        if (!Current(envelope, current, now, false))
            return ChannelError::Stale;
        if (entry.terminal)
            return ChannelError::Closed;
        if (auto const* command = std::get_if<ChannelCommand>(&payload))
        {
            if (entry.work == ChannelWork::Query || command->issuerGuid != entry.scope.controllerGuid ||
                command->botGuid != entry.scope.botGuid)
                return ChannelError::Invalid;
            if (_commands.contains(command->commandId))
                return ChannelError::Duplicate;
            if (_commands.size() >= _maxIds)
                return ChannelError::Capacity;
            _commands.emplace(command->commandId, CommandEntry{envelope.requestId});
        }
        else if (auto const* watch = std::get_if<ChannelWatch>(&payload))
        {
            // J6e-c: only on an open generation (or wake) route and for its own bot; the id is a command id.
            if (entry.work != ChannelWork::Generation || watch->botGuid != entry.scope.botGuid)
                return ChannelError::Invalid;
            if (_commands.contains(watch->commandId))
                return ChannelError::Duplicate;
            if (_commands.size() >= _maxIds)
                return ChannelError::Capacity;
            _commands.emplace(watch->commandId, CommandEntry{envelope.requestId});
        }
        else
        {
            if ((type == "generation.result" && entry.work != ChannelWork::Generation) ||
                (type == "events.result" && entry.work != ChannelWork::Events))
                return ChannelError::Invalid;
            entry.terminal = true;
        }
        message = {std::move(envelope), std::move(payload)};
        return ChannelError::None;
    }
    catch (StandingShape const&)
    {
        return ChannelError::Standing;
    }
    catch (std::exception const&)
    {
        return ChannelError::Invalid;
    }
}

ChannelError ChannelPolicy::FinishQuery(ChannelEnvelope const& envelope, WorldScope const& current,
                                        std::vector<Participant> const& observedUnits, Clock now, std::string& body)
{
    body.clear();
    if (!Current(envelope, current, now) || _requests.at(envelope.requestId).work != ChannelWork::Query)
        return ChannelError::Stale;
    if (observedUnits.size() > ChannelMaxUnits)
        return ChannelError::TooLarge;
    Json units = Json::array();
    std::set<uint64_t> seen;
    for (auto const& unit : observedUnits)
    {
        if (!unit.guid || !seen.insert(unit.guid).second || !Text(unit.name, 128) || unit.healthPercent < -1 ||
            unit.healthPercent > 100 ||
            (!unit.role.empty() && unit.role != "tank" && unit.role != "healer" && unit.role != "damage"))
            return ChannelError::Invalid;
        Json value = {{"guid", std::to_string(unit.guid)}, {"name", unit.name}};
        if (unit.healthPercent >= 0)
            value["healthPercent"] = unit.healthPercent;
        if (!unit.role.empty())
            value["role"] = unit.role;
        if (unit.visibleTargetGuid)
            value["visibleTargetGuid"] = std::to_string(unit.visibleTargetGuid);
        units.push_back(std::move(value));
    }
    try
    {
        auto json = EnvelopeJson(envelope, "query.result");
        json["units"] = std::move(units);
        auto encoded = json.dump();
        if (encoded.size() > MaxResponseBytes)
            return ChannelError::TooLarge;
        _requests.at(envelope.requestId).terminal = true;
        body = std::move(encoded);
        return ChannelError::None;
    }
    catch (std::exception const&)
    {
        return ChannelError::Invalid;
    }
}

ChannelError ChannelPolicy::FinishTopic(ChannelEnvelope const& envelope, WorldScope const& current, StateTopic topic,
                                        std::vector<StateEntry> const& entries, Clock now, std::string& body)
{
    body.clear();
    if (static_cast<std::size_t>(topic) >= StateTopicCount)
        return ChannelError::Invalid;
    if (!Current(envelope, current, now) || _requests.at(envelope.requestId).work != ChannelWork::Query)
        return ChannelError::Stale;
    try
    {
        // Entries in the game's order, unusable and repeated ones dropped, cut at the topic's count and byte bounds.
        Json list = Json::array();
        std::set<std::string> seen;
        std::size_t bytes = 2;  // The enclosing brackets.
        for (auto const& entry : entries)
        {
            if (list.size() >= StateTopicLimit(topic))
                break;
            Json value;
            if (!TopicEntry(topic, entry, value))
                continue;
            auto encoded = value.dump();
            auto const size = encoded.size() + (list.empty() ? 0 : 1);
            if (!seen.insert(std::move(encoded)).second)
                continue;
            if (bytes + size > ChannelMaxTopicBytes)
                break;
            bytes += size;
            list.push_back(std::move(value));
        }
        auto json = EnvelopeJson(envelope, "query.result");
        json["topic"] = StateTopicName(topic);
        json["entries"] = std::move(list);
        auto encoded = json.dump();
        if (encoded.size() > MaxResponseBytes)
            return ChannelError::TooLarge;
        _requests.at(envelope.requestId).terminal = true;
        body = std::move(encoded);
        return ChannelError::None;
    }
    catch (std::exception const&)
    {
        return ChannelError::Invalid;
    }
}

ChannelError ChannelPolicy::FinishFailure(ChannelEnvelope const& envelope, ChannelFailure failure,
                                          WorldScope const& current, Clock now, std::string& body)
{
    body.clear();
    if (!Current(envelope, current, now))
        return ChannelError::Stale;
    char const* code = nullptr;
    switch (failure)
    {
        case ChannelFailure::Unavailable:
            code = "unavailable";
            break;
        case ChannelFailure::Timeout:
            code = "timeout";
            break;
        case ChannelFailure::InvalidInput:
            code = "invalid_input";
            break;
        case ChannelFailure::Cancelled:
            code = "cancelled";
            break;
    }
    if (!code)
        return ChannelError::Invalid;
    auto json = EnvelopeJson(envelope, "failure");
    json["code"] = code;
    body = json.dump();
    _requests.at(envelope.requestId).terminal = true;
    return ChannelError::None;
}

ChannelError ChannelPolicy::FinishCommand(ChannelEnvelope const& envelope, std::string const& commandId,
                                          ChannelOutcome outcome, WorldScope const& current, Clock now,
                                          std::string& body, CommandRefusal refusal)
{
    body.clear();
    if (!Current(envelope, current, now, false))
        return ChannelError::Stale;
    auto found = _commands.find(commandId);
    if (found == _commands.end() || found->second.requestId != envelope.requestId)
        return ChannelError::Unknown;
    if (found->second.terminal)
        return ChannelError::Duplicate;
    if (outcome != ChannelOutcome::Applied && outcome != ChannelOutcome::Rejected && outcome != ChannelOutcome::Failed)
        return ChannelError::Invalid;
    auto json = EnvelopeJson(envelope, "command.result");
    json["commandId"] = commandId;
    json["outcome"] = outcome == ChannelOutcome::Applied    ? "applied"
                      : outcome == ChannelOutcome::Rejected ? "rejected"
                                                            : "failed";
    if (outcome != ChannelOutcome::Applied)
    {
        if (refusal == CommandRefusal::None)
            refusal = outcome == ChannelOutcome::Failed ? CommandRefusal::Failed : CommandRefusal::CannotNow;
        auto const words = DescribeRefusal(refusal);
        json["reason"] = words.code;
        json["reasonText"] = words.text;
    }
    body = json.dump();
    found->second.terminal = true;
    return ChannelError::None;
}

ChannelError ChannelPolicy::FinishWatch(ChannelEnvelope const& envelope, std::string const& commandId,
                                        WatchOutcome outcome, WorldScope const& current, Clock now, std::string& body,
                                        CommandRefusal refusal)
{
    body.clear();
    if (!Current(envelope, current, now, false))
        return ChannelError::Stale;
    auto found = _commands.find(commandId);
    if (found == _commands.end() || found->second.requestId != envelope.requestId)
        return ChannelError::Unknown;
    if (found->second.terminal)
        return ChannelError::Duplicate;
    if ((outcome == WatchOutcome::Rejected) != (refusal != CommandRefusal::None))
        return ChannelError::Invalid;
    auto json = EnvelopeJson(envelope, "watch.result");
    json["commandId"] = commandId;
    json["outcome"] = outcome == WatchOutcome::Armed     ? "armed"
                      : outcome == WatchOutcome::Cleared ? "cleared"
                                                         : "rejected";
    if (outcome == WatchOutcome::Rejected)
    {
        auto const words = DescribeRefusal(refusal);
        json["reason"] = words.code;
        json["reasonText"] = words.text;
    }
    body = json.dump();
    found->second.terminal = true;
    return ChannelError::None;
}
}  // namespace OllamaBridge
