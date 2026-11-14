#include "mod-ollama-chat_bridge_channel.h"

#include <algorithm>
#include <charconv>
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

// LC3: the installed ability index and its encoded catalogue (null while none is installed).
struct InstalledAbilities
{
    AbilityIndex index;
    Json catalogue;
};

InstalledAbilities& Installed()
{
    static InstalledAbilities installed;
    return installed;
}

// J5e: a standing follow/stay shape (any params) is refused with its own reason, still invalid_input on the wire.
struct StandingShape : std::runtime_error
{
    StandingShape() : std::runtime_error("standing") {}
};

// LC3: a choice parameter's value, one of the act's choice words, as its index.
uint8_t Choice(Json const& value, AbilityParam const& param)
{
    auto const word = String(value);
    auto const found = std::find(param.choices.begin(), param.choices.end(), word);
    if (found == param.choices.end() || found - param.choices.begin() > UINT8_MAX)
        throw std::runtime_error("invalid");
    return static_cast<uint8_t>(found - param.choices.begin());
}

// LC3: a position's spot "x,y,z,map": three finite decimal numbers and a map id, comma-separated, nothing else.
ChannelPosition Spot(Json const& value)
{
    auto const text = Place(value);
    ChannelPosition spot;
    char const* at = text.data();
    char const* const end = text.data() + text.size();
    for (double* coordinate : {&spot.x, &spot.y, &spot.z})
    {
        auto const [next, error] = std::from_chars(at, end, *coordinate);
        if (error != std::errc() || next == end || *next != ',' || !std::isfinite(*coordinate))
            throw std::runtime_error("invalid");
        at = next + 1;
    }
    auto const [next, error] = std::from_chars(at, end, spot.mapId);
    if (error != std::errc() || next != end)
        throw std::runtime_error("invalid");
    return spot;
}

// LC3: the steps of a stepped command, 1..maxSteps acts of the installed catalogue. Each is {act, params} with exactly
// the act's parameters by their catalogue names: a unit as `name` (its name) or `nameGuid`, a quest as `name` (its
// title) or `nameId`, exactly one of the two; names and places have Place's text rules. A topic, a verb without a row
// and an unknown word are no act.
std::vector<ChannelStep> Steps(Json const& value)
{
    auto const& index = Installed().index;
    if (!value.is_array() || value.empty() || value.size() > index.maxSteps)
        throw std::runtime_error("invalid");
    std::vector<ChannelStep> steps;
    for (auto const& entry : value)
    {
        if (!Exact(entry, {"act", "params"}) || !entry.at("params").is_object())
            throw std::runtime_error("invalid");
        auto const act = String(entry.at("act"));
        auto const row = std::find_if(index.rows.begin(), index.rows.end(), [&act](AbilityRow const& candidate)
                                      { return !candidate.topic && candidate.name == act; });
        if (row == index.rows.end())
            throw std::runtime_error("invalid");
        auto const& params = entry.at("params");
        ChannelStep step;
        step.intent = static_cast<ChannelIntent>(row->code);
        for (auto const& param : row->params)
        {
            std::string const id = param.field == AbilityField::Target    ? param.name + "Guid"
                                   : param.field == AbilityField::QuestId ? param.name + "Id"
                                                                          : std::string();
            bool const named = params.contains(param.name);
            if (named == (!id.empty() && params.contains(id)))
                throw std::runtime_error("invalid");  // Neither form given, or both.
            auto const& given = params.at(named ? param.name : id);
            switch (param.field)
            {
                case AbilityField::Target:
                    if (named)
                        step.targetName = Place(given);
                    else
                        step.targetGuid = Guid(given);
                    break;
                case AbilityField::QuestId:
                    if (named)
                        step.questName = Place(given);
                    else if (!(step.questId = static_cast<uint32_t>(Integer(given, UINT32_MAX))))
                        throw std::runtime_error("invalid");
                    break;
                case AbilityField::PlaceName:
                    step.place = Place(given);
                    break;
                case AbilityField::Position:
                    step.position = Spot(given);
                    break;
                case AbilityField::Icon:
                    step.icon = Choice(given, param);
                    break;
                case AbilityField::Shape:
                    step.shape = Choice(given, param);
                    break;
                case AbilityField::Enabled:
                    if (!given.is_boolean())
                        throw std::runtime_error("invalid");
                    step.enabled = given.get<bool>();
                    break;
            }
        }
        if (params.size() != row->params.size())
            throw std::runtime_error("invalid");  // A key the act does not take.
        steps.push_back(std::move(step));
    }
    return steps;
}

ChannelCommand Command(Json const& body)
{
    // LC3: a stepped command carries `steps` in place of `intent` and `params`; both forms at once is neither.
    bool const stepped = body.contains("steps");
    if (stepped ? !Exact(body, {"version", "instance", "provider", "epoch", "requestId", "deadlineUnixMs", "type",
                                "commandId", "issuerGuid", "botGuid", "steps"})
                : !Exact(body, {"version", "instance", "provider", "epoch", "requestId", "deadlineUnixMs", "type",
                                "commandId", "issuerGuid", "botGuid", "intent", "params"}))
        throw std::runtime_error("invalid");
    ChannelCommand command;
    command.commandId = String(body.at("commandId"));
    command.issuerGuid = Guid(body.at("issuerGuid"));
    command.botGuid = Guid(body.at("botGuid"));
    if (!Identifier(command.commandId))
        throw std::runtime_error("invalid");
    if (stepped)
    {
        command.steps = Steps(body.at("steps"));
        return command;
    }
    auto const intent = String(body.at("intent"));
    auto const& params = body.at("params");
    // LC3: the verbs are playerbots' (the installed index), the acts' and the four without a row (refused later).
    auto const& verbs = Installed().index.verbs;
    auto const found =
        std::find_if(verbs.begin(), verbs.end(), [&](AbilityVerb const& verb) { return intent == verb.name; });
    if (found == verbs.end())
        throw std::runtime_error("invalid");
    command.intent = found->intent;
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

// LC3: any topic the installed catalogue lists; SetAbilityIndex took only topics with a StateTopic (an entry shape).
bool ParseTopic(std::string const& name, StateTopic& topic)
{
    for (auto const& row : Installed().index.rows)
        if (row.topic && row.name == name)
        {
            topic = static_cast<StateTopic>(row.code);
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
        case StateTopic::History:  // LC7: the planner shape and the entry's sentence in its own field.
        case StateTopic::Lately:
        case StateTopic::Known:
        {
            auto const text = TopicText(entry.text, ChannelMaxTopicText);
            if (!PlannerRow(topic, entry.kind) || text.empty())
                return false;
            value = {{"row", entry.kind},
                     {"name", name},
                     {"text", text},
                     {"value", std::clamp<int64_t>(entry.first, 0, UINT32_MAX)},
                     {"extra", std::clamp<int64_t>(entry.second, 0, UINT32_MAX)}};
            return true;
        }
    }
    return false;
}

// LC3: what this channel takes beyond the old wire, listed in its catalogue answer (not part of the hash). LC10:
// `facts`, the module sends input.facts, input.traits and the new state keys (around, servant, mount, deeds).
constexpr char const* ChannelAbilities[] = {"catalogue", "names", "steps", "facts"};
static_assert(MaxStateDeedBytes == ChannelMaxTopicText, "a deed on the wire is a whole record topic sentence");

std::string Hex16(uint64_t value)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (std::size_t i = text.size(); i-- > 0; value >>= 4)
        text[i] = digits[value & 0xF];
    return text;
}

// One row's parts field for field, as PlayerbotBridge::CatalogueText lists them.
Json CatalogueRow(AbilityRow const& row)
{
    Json params = Json::array();
    for (auto const& param : row.params)
    {
        Json value = {
            {"name", param.name}, {"kind", param.kind}, {"sentence", param.sentence}, {"choices", param.choices}};
        params.push_back(std::move(value));
    }
    Json refusals = Json::array();
    for (auto const& refusal : row.refusals)
    {
        Json value = {{"word", refusal.word}, {"sentence", refusal.sentence}};
        refusals.push_back(std::move(value));
    }
    return {{"kind", row.topic ? "topic" : "act"},
            {"name", row.name},
            {"sentence", row.sentence},
            {"params", std::move(params)},
            {"refusals", std::move(refusals)},
            {"effect", row.effect},
            {"done", row.done},
            {"state", row.state},
            {"event", row.event},
            {"limit", row.limit}};
}

Json CatalogueJson(AbilityIndex const& index)
{
    Json abilities = Json::array();
    for (char const* ability : ChannelAbilities)
        abilities.push_back(ability);
    Json rows = Json::array();
    for (auto const& row : index.rows)
        rows.push_back(CatalogueRow(row));
    return {{"hash", Hex16(index.hash)},
            {"version", index.version},
            {"abilities", std::move(abilities)},
            {"maxSteps", index.maxSteps},
            {"rows", std::move(rows)}};
}

// A wire word: 1..64 lower-case letters and underscores.
bool Word(std::string const& name)
{
    return !name.empty() && name.size() <= 64 &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) { return (c >= 'a' && c <= 'z') || c == '_'; });
}

// Every ChannelIntent one verb of its own; every row a word of its own, an act one of the verbs (its own intent), a
// topic one StateTopic.
bool ValidIndex(AbilityIndex const& index)
{
    constexpr std::size_t intents = ChannelIntentCount;
    if (!index.version || !index.maxSteps || index.rows.empty() || index.verbs.size() != intents)
        return false;
    std::set<std::string> verbs;
    std::set<ChannelIntent> taken;
    for (auto const& verb : index.verbs)
        if (!Word(verb.name) || static_cast<std::size_t>(verb.intent) >= intents || !verbs.insert(verb.name).second ||
            !taken.insert(verb.intent).second)
            return false;
    std::set<std::string> names;
    std::set<uint8_t> topics;
    for (auto const& row : index.rows)
    {
        auto const own = [&row](AbilityVerb const& verb)
        { return verb.name == row.name && static_cast<uint8_t>(verb.intent) == row.code; };
        if (!Word(row.name) || !names.insert(row.name).second ||
            (row.topic ? row.code >= StateTopicCount || !topics.insert(row.code).second
                       : std::none_of(index.verbs.begin(), index.verbs.end(), own)))
            return false;
    }
    return true;
}
}  // namespace

std::size_t SetAbilityIndex(AbilityIndex index)
{
    auto& installed = Installed();
    installed = {};
    if (!ValidIndex(index))
        return 0;
    // Only the catalogue query needs the encoded answer: one it cannot send leaves the verbs and topics standing.
    std::size_t bytes = 0;
    Json catalogue;
    try
    {
        Json encoded = CatalogueJson(index);
        bytes = encoded.dump().size();  // Throws on text that is not UTF-8.
        if (bytes <= ChannelMaxCatalogueBytes)
            catalogue = std::move(encoded);
    }
    catch (std::exception const&)
    {
        bytes = 0;
    }
    installed = {std::move(index), std::move(catalogue)};
    return bytes;
}

AbilityIndex const& Abilities() { return Installed().index; }

bool CatalogueInstalled() { return Installed().catalogue.is_object(); }

// LC3: the installed catalogue's word for the topic.
char const* StateTopicName(StateTopic topic)
{
    for (auto const& row : Installed().index.rows)
        if (row.topic && row.code == static_cast<uint8_t>(topic))
            return row.name.c_str();
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
    static constexpr char const* history[] = {"event", "zone", "dungeon", "boss"};
    static constexpr char const* lately[] = {"deed"};
    static constexpr char const* known[] = {"bot"};
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
        case StateTopic::History:
            return pick(history);
        case StateTopic::Lately:
            return pick(lately);
        case StateTopic::Known:
            return pick(known);
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
        case StateTopic::History:  // LC7: the limits playerbots' record readers keep.
            return 24;
        case StateTopic::Lately:
            return 12;
        case StateTopic::Known:
            return 12;
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
            else
            {
                // LC3: the catalogue takes observable_group's keys; it is plain data and names no unit.
                if ((kind != "observable_group" && kind != "catalogue") ||
                    !Exact(json, {"version", "instance", "provider", "epoch", "requestId", "deadlineUnixMs", "type",
                                  "controllerGuid", "botGuid", "query"}))
                    return ChannelError::Invalid;
                if (kind == "catalogue")
                    query.kind = QueryKind::Catalogue;
            }
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

ChannelError ChannelPolicy::FinishCatalogue(ChannelEnvelope const& envelope, WorldScope const& current, Clock now,
                                            std::string& body)
{
    body.clear();
    if (!Current(envelope, current, now) || _requests.at(envelope.requestId).work != ChannelWork::Query)
        return ChannelError::Stale;
    auto const& catalogue = Installed().catalogue;
    if (!catalogue.is_object())
        return ChannelError::Invalid;  // No index installed: the world answers unavailable before this.
    try
    {
        auto json = EnvelopeJson(envelope, "query.result");
        json["catalogue"] = catalogue;
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
                                          std::string& body, CommandRefusal refusal, std::span<ChannelStep const> steps,
                                          std::span<ChannelStepResult const> ran)
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
    // LC3: the steps that ran come first, each but the last applied; the order applied exactly when all of them did.
    auto const applied = [](ChannelStepResult const& step) { return step.outcome == ChannelOutcome::Applied; };
    if (!steps.empty() &&
        (ran.size() > steps.size() || !std::all_of(ran.begin(), ran.end() - (ran.empty() ? 0 : 1), applied) ||
         (outcome == ChannelOutcome::Applied) !=
             (ran.size() == steps.size() && std::all_of(ran.begin(), ran.end(), applied))))
        return ChannelError::Invalid;
    auto const word = [](ChannelOutcome value)
    {
        return value == ChannelOutcome::Applied ? "applied" : value == ChannelOutcome::Rejected ? "rejected" : "failed";
    };
    // A step's reason as the executor worded it; one it left unworded reads as the old form's cannot_now or failed.
    auto const reason = [](Json& json, ChannelStepResult const& step)
    {
        auto const words = DescribeRefusal(step.outcome == ChannelOutcome::Failed ? CommandRefusal::Failed
                                                                                  : CommandRefusal::CannotNow);
        json["reason"] = step.reason.empty() ? std::string(words.code) : step.reason;
        json["reasonText"] = step.reason.empty() ? std::string(words.text) : step.reasonText;
    };
    auto json = EnvelopeJson(envelope, "command.result");
    json["commandId"] = commandId;
    json["outcome"] = word(outcome);
    if (outcome != ChannelOutcome::Applied && !steps.empty() && !ran.empty() && !applied(ran.back()))
        reason(json, ran.back());  // The step that ended the order gives it its reason.
    else if (outcome != ChannelOutcome::Applied)
    {
        if (refusal == CommandRefusal::None)
            refusal = outcome == ChannelOutcome::Failed ? CommandRefusal::Failed : CommandRefusal::CannotNow;
        auto const words = DescribeRefusal(refusal);
        json["reason"] = words.code;
        json["reasonText"] = words.text;
    }
    if (!steps.empty())
    {
        Json list = Json::array();
        auto const& verbs = Installed().index.verbs;
        for (std::size_t index = 0; index < steps.size(); ++index)
        {
            auto const verb = std::find_if(verbs.begin(), verbs.end(), [&](AbilityVerb const& entry)
                                           { return entry.intent == steps[index].intent; });
            Json step = {{"act", verb == verbs.end() ? std::string() : verb->name}};
            if (index >= ran.size())
                step["outcome"] = "skipped";  // After the ending step, or the order was refused before any ran.
            else
            {
                step["outcome"] = word(ran[index].outcome);
                if (!applied(ran[index]))
                    reason(step, ran[index]);
            }
            list.push_back(std::move(step));
        }
        json["steps"] = std::move(list);
    }
    try
    {
        body = json.dump();
    }
    catch (std::exception const&)
    {
        return ChannelError::Invalid;  // Executor words that are not UTF-8: the command stays unanswered.
    }
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
