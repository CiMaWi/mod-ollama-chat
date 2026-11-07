#include "mod-ollama-chat_bridge.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>

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
        input.party.size() > 40 ||
        (input.card.offered ? !input.card.stamp || (input.card.accepted && !input.card.revision)
                            : input.card.revision || input.card.stamp || input.card.accepted))
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

bool Hex32(std::string const& value)
{
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](unsigned char c)
                                             { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool ValidScope(WorldScope const& scope, Settings const& settings, std::string const& epoch)
{
    return scope.version == 1 && scope.instance == settings.instance && scope.provider == settings.provider &&
           scope.epoch == epoch && scope.botGuid && scope.controllerGuid &&
           (scope.channel == "direct" || scope.channel == "party") && ValidIdentifier(scope.contextId) &&
           ValidIdentifier(scope.perceptionScope) && ValidIdentifier(scope.contextRevision) &&
           ValidIdentifier(scope.membershipRevision) && ValidIdentifier(scope.personaRevision);
}

nlohmann::json ScopeJson(WorldScope const& scope)
{
    return {{"version", 1},
            {"instance", scope.instance},
            {"provider", scope.provider},
            {"epoch", scope.epoch},
            {"botGuid", std::to_string(scope.botGuid)},
            {"controllerGuid", std::to_string(scope.controllerGuid)},
            {"contextId", scope.contextId},
            {"channel", scope.channel},
            {"perceptionScope", scope.perceptionScope},
            {"contextRevision", scope.contextRevision},
            {"membershipRevision", scope.membershipRevision},
            {"personaRevision", scope.personaRevision}};
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

// One card fact on the wire: control characters dropped, cut to MaxCardFactBytes on a character boundary, and left
// out when blank or not valid UTF-8 (the envelope dump would throw).
void PutCardFact(nlohmann::json& context, char const* key, std::string const& value)
{
    std::string fact;
    for (unsigned char byte : value)
        if (byte >= 0x20 && byte != 0x7F)
            fact.push_back(static_cast<char>(byte));
    if (fact.size() > MaxCardFactBytes)
    {
        std::size_t cut = MaxCardFactBytes;
        while (cut && (static_cast<unsigned char>(fact[cut]) & 0xC0) == 0x80)
            --cut;
        fact.resize(cut);
    }
    if (Blank(fact))
        return;
    try
    {
        static_cast<void>(nlohmann::json(fact).dump());
    }
    catch (nlohmann::json::exception const&)
    {
        return;
    }
    context[key] = std::move(fact);
}

// DS.12 `input.personaCard.context`: a flat object of the bot's in-world facts; absent facts are left out.
nlohmann::json CardContextJson(CardFacts const& facts)
{
    nlohmann::json context = nlohmann::json::object();
    PutCardFact(context, "name", facts.name);
    PutCardFact(context, "race", facts.race);
    PutCardFact(context, "class", facts.className);
    PutCardFact(context, "gender", facts.gender);
    if (facts.level)
        context["level"] = facts.level;
    PutCardFact(context, "zone", facts.zone);
    PutCardFact(context, "faction", facts.faction);
    PutCardFact(context, "guild", facts.guild);
    PutCardFact(context, "traits", facts.traits);
    return context;
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
char const* StateActivity(BotState const& state)
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

char const* StateMovement(BotState const& state)
{
    return state.stay      ? "stay"
           : state.follow  ? "follow"
           : state.guard   ? "guard"
           : state.grind   ? "grind"
           : state.passive ? "passive"
                           : "none";
}

// DS.14/J5f `input.state`: what the bot itself knows about its condition, place and doings.
nlohmann::json StateJson(BotState const& state)
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
        PutCardFact(target, "name", state.victimName);
        if (state.victimLevel)
            target["level"] = std::min<uint32_t>(state.victimLevel, 255);
        target["health"] = StatePercent(state.victimHealthPct);
        json["combat"] = {{"target", std::move(target)}};
    }
    char const* activity = StateActivity(state);
    nlohmann::json doing = {{"kind", activity}};
    if (std::string_view(activity) == "questing")
        PutCardFact(doing, "detail", state.questTitle);
    json["activity"] = std::move(doing);
    nlohmann::json where = nlohmann::json::object();
    PutCardFact(where, "zone", state.zone);
    if (state.area != state.zone)
        PutCardFact(where, "area", state.area);
    where["mapId"] = state.mapId;
    where["x"] = StateYards(state.x);
    where["y"] = StateYards(state.y);
    where["z"] = StateYards(state.z);
    json["where"] = std::move(where);
    // The partner only while in the bot's group or in its sight; distance and position only on the same map.
    if (state.partnerOnline && (state.partnerInGroup || state.partnerVisible))
    {
        nlohmann::json partner = nlohmann::json::object();
        PutCardFact(partner, "zone", state.partnerZone);
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
            PutCardFact(group, "follows", state.masterName);
        json["group"] = std::move(group);
    }
    json["movement"] = StateMovement(state);
    static char const* const planNames[] = {"movement", "formation", "save_mana", "focus_healing"};
    nlohmann::json plans = nlohmann::json::array();
    for (std::size_t kind = 0; kind < std::size(planNames); ++kind)
        if (state.plans & (1u << kind))
            plans.push_back(planNames[kind]);
    if (!plans.empty())
        json["plans"] = std::move(plans);
    json["bags"] = std::min<uint32_t>(state.freeBagSlots, 255);
    json["money"] = state.money;
    return json;
}
}  // namespace

std::string CardTraitWord(std::string const& personalityKey)
{
    static std::map<std::string, std::string> const words = {
        {"ROLEPLAYER", "dramatic"},
        {"LOOTGOBLIN", "greedy"},
        {"PVP_HARDCORE", "fierce"},
        {"RAIDER", "battle-hardened"},
        {"CASUAL", "easygoing"},
        {"TRADER", "shrewd"},
        {"GRUMPY_VETERAN", "gruff"},
        {"HEROIC_LEADER", "commanding"},
        {"TRICKSTER", "mischievous"},
        {"LONE_WOLF", "aloof"},
        {"FOOL", "silly"},
        {"ANCIENT_WISE_ONE", "wise"},
        {"BARD", "theatrical"},
        {"CONSPIRACY_THEORIST", "suspicious"},
        {"EDGE_LORD", "brooding"},
        {"FANATIC", "zealous"},
        {"HYPE_MAN", "boisterous"},
        {"PARANOID", "wary"},
        {"FLIRT", "charming"},
        {"RAGER", "hot-tempered"},
        {"YOUNG_APPRENTICE", "eager"},
        {"MENTOR", "patient"},
        {"SCHOLAR", "bookish"},
        {"WANNABE_VILLAIN", "scheming"},
        {"JOLLY_BEER_LOVER", "jolly"},
        {"GOBLIN_MERCHANT", "shrewd"},
        {"PIRATE", "roguish"},
        {"CHEF", "hearty"},
        {"POET", "poetic"},
    };
    auto const it = words.find(personalityKey);
    return it == words.end() ? std::string() : it->second;
}

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
                                     {"systemPrompt", input.systemPrompt},
                                     {"persona", input.persona},
                                     {"memory", input.memory},
                                     {"groupScope", input.groupScope},
                                     {"directAddress", input.directAddress},
                                     {"speakerControlsBot", input.speakerControlsBot},
                                     {"party", std::move(party)}}}};
        if (input.card.offered)
        {
            nlohmann::json card = {{"revision", std::to_string(input.card.revision)},
                                   {"accepted", input.card.accepted}};
            if (!input.card.accepted)
                card["context"] = CardContextJson(input.card.facts);
            envelope["input"]["personaCard"] = std::move(card);
        }
        if (input.state.present)
            envelope["input"]["state"] = StateJson(input.state);
        if (!input.wakeCondition.empty())  // J6e-c: a wake turn (never addressed); the watch that opened it.
            envelope["input"]["wake"] = {{"condition", input.wakeCondition}, {"watchId", input.wakeWatchId}};
        if (settings.provider == "codex" && settings.observerEnabled)
        {
            if (!ValidObserverSettings(settings) || !ValidScope(request.scope, settings, request.epoch) ||
                request.scope.botGuid != request.botGuid || request.scope.controllerGuid != request.controllerGuid ||
                request.scope.channel != input.channel)
                return false;
            envelope["input"]["version"] = 2;
            envelope["scope"] = ScopeJson(request.scope);
        }
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

bool BuildObserverOpen(Settings const& settings, std::string& body)
{
    body.clear();
    if (!ValidObserverSettings(settings))
        return false;
    body =
        nlohmann::json{
            {"version", 1}, {"type", "observer.open"}, {"instance", settings.instance}, {"provider", "codex"}}
            .dump();
    return true;
}

bool BuildObserverSnapshot(Settings const& settings, ObserverChallenge const& challenge, std::string const& epoch,
                           bool active, bool worldDrained, std::vector<WorldScope> const& scopes, std::string& body)
{
    body.clear();
    if (!ValidObserverSettings(settings) || !ValidIdentifier(challenge.serviceId) || !Hex32(challenge.incarnation) ||
        !Hex32(challenge.challenge) || !challenge.sequence || challenge.sequence >= 2147483647 || epoch.size() != 65 ||
        epoch.substr(0, 33) != challenge.incarnation + "." || !Hex32(epoch.substr(33)) || scopes.size() > 64 ||
        (!active && !scopes.empty()))
        return false;
    try
    {
        nlohmann::json entries = nlohmann::json::array();
        std::set<std::tuple<uint64_t, uint64_t, std::string>> peers;
        std::set<std::pair<uint64_t, std::string>> slots;
        for (auto const& scope : scopes)
        {
            if (!ValidScope(scope, settings, epoch) ||
                !peers.emplace(scope.botGuid, scope.controllerGuid, scope.channel).second ||
                !slots.emplace(scope.botGuid, scope.contextId).second)
                return false;
            entries.push_back(ScopeJson(scope));
        }
        std::string encoded = nlohmann::json{
            {"version", 1},
            {"type", "observer.snapshot"},
            {"instance", settings.instance},
            {"provider", "codex"},
            {"serviceId", challenge.serviceId},
            {"incarnation", challenge.incarnation},
            {"challenge", challenge.challenge},
            {"sequence", challenge.sequence},
            {"epoch", epoch},
            {"active", active},
            {"worldDrained", worldDrained},
            {"scopes",
             std::move(entries)}}.dump();
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

bool ParseObserverChallenge(Settings const& settings, std::string const& body, ObserverChallenge const* previous,
                            ObserverChallenge& challenge)
{
    challenge = {};
    if (!ValidObserverSettings(settings) || body.empty() || body.size() > MaxResponseBytes)
        return false;
    try
    {
        std::set<std::string> keys;
        auto const parsed = nlohmann::json::parse(
            body,
            [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value)
            {
                if (depth > 1 ||
                    (event == nlohmann::json::parse_event_t::key && !keys.insert(value.get<std::string>()).second))
                    throw std::runtime_error("observer_shape");
                return true;
            });
        if (!parsed.is_object() || parsed.size() != 9 || !parsed.contains("version") ||
            !parsed["version"].is_number_integer() || parsed["version"] != 1 ||
            !MatchesString(parsed, "type", "observer.challenge") ||
            !MatchesString(parsed, "instance", settings.instance) || !MatchesString(parsed, "provider", "codex") ||
            !parsed.contains("freshnessMs") || !parsed["freshnessMs"].is_number_integer() ||
            parsed["freshnessMs"] != 5000 || !parsed.contains("sequence") || !parsed["sequence"].is_number_integer() ||
            parsed["sequence"] < 1 || parsed["sequence"] > 2147483647 || !parsed.contains("serviceId") ||
            !parsed["serviceId"].is_string() || !parsed.contains("incarnation") || !parsed["incarnation"].is_string() ||
            !parsed.contains("challenge") || !parsed["challenge"].is_string())
            return false;
        ObserverChallenge next{parsed["serviceId"], parsed["incarnation"], parsed["challenge"], parsed["sequence"]};
        if (!ValidIdentifier(next.serviceId) || !Hex32(next.incarnation) || !Hex32(next.challenge) ||
            (!previous && next.sequence != 1) ||
            (previous && (previous->sequence >= 2147483647 || next.sequence != previous->sequence + 1 ||
                          next.serviceId != previous->serviceId || next.incarnation != previous->incarnation ||
                          next.challenge == previous->challenge)))
            return false;
        challenge = std::move(next);
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
        // The one optional key: a card proposal, only in a generation result for a request that offered the card.
        bool const proposal = parsed.is_object() && parsed.contains("personaProposal");
        if (!parsed.is_object() || parsed.size() != (proposal ? 9u : 8u) || !parsed.contains("version") ||
            !parsed["version"].is_number_integer() || parsed["version"] != 1 ||
            !MatchesString(parsed, "instance", settings.instance) ||
            !MatchesString(parsed, "provider", settings.provider) || !MatchesString(parsed, "epoch", request.epoch) ||
            !MatchesString(parsed, "requestId", request.requestId) || !parsed.contains("deadlineUnixMs") ||
            !parsed["deadlineUnixMs"].is_number_integer() || parsed["deadlineUnixMs"] != request.deadline.unixMs)
            return result;
        // A card is wanted only where the request carried the context (offered without an accepted card).
        if (proposal && (!request.input.card.offered || request.input.card.accepted ||
                         !MatchesString(parsed, "type", "generation.result")))
            return result;

        if (MatchesString(parsed, "type", "failure"))
        {
            if (MatchesString(parsed, "code", "unavailable") || MatchesString(parsed, "code", "timeout") ||
                MatchesString(parsed, "code", "invalid_input") || MatchesString(parsed, "code", "cancelled"))
                result.error = "bridge_terminal_failure";
            return result;
        }
        // DS.14: exactly one of text or "silent": true (an action-only answer, which never carries a card).
        bool const silent = parsed.contains("silent");
        if (!MatchesString(parsed, "type", "generation.result") || silent == parsed.contains("text"))
            return result;
        if (silent ? !parsed["silent"].is_boolean() || !parsed["silent"].get<bool>() || proposal
                   : !parsed["text"].is_string())
            return result;
        static std::string const wordless;
        auto const& text = silent ? wordless : parsed["text"].get_ref<std::string const&>();
        if (!silent &&
            (text.empty() || text.size() > MaxTextBytes || Blank(text) || text.find('\0') != std::string::npos))
            return result;
        if (proposal)
        {
            // Exactly {revision, body}: the offered revision as canonical decimal and a bounded nonblank body. The
            // parser has already refused ill-formed UTF-8.
            auto const& card = parsed["personaProposal"];
            if (!card.is_object() || card.size() != 2 ||
                !MatchesString(card, "revision", std::to_string(request.input.card.revision)) ||
                !card.contains("body") || !card["body"].is_string())
                return result;
            auto const& cardBody = card["body"].get_ref<std::string const&>();
            if (cardBody.empty() || cardBody.size() > MaxCardProposalBytes || Blank(cardBody) ||
                cardBody.find('\0') != std::string::npos)
                return result;
            result.personaProposal = cardBody;
        }
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

uint8_t ActivityCode(BotState const& state)
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
