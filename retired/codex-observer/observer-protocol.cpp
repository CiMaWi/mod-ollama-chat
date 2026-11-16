// Parked code of the retired Codex bridge's world observer (CS.20, LC.35). Not compiled, not tested.
// Cut verbatim from src/mod-ollama-chat_bridge.cpp at b81bf8c by LCB0.

// Anonymous-namespace helpers:
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

// In Build(), after the wake field:
        if (settings.provider == "codex" && settings.observerEnabled)
        {
            if (!ValidObserverSettings(settings) || !ValidScope(request.scope, settings, request.epoch) ||
                request.scope.botGuid != request.botGuid || request.scope.controllerGuid != request.controllerGuid ||
                request.scope.channel != input.channel)
                return false;
            envelope["input"]["version"] = 2;
            envelope["scope"] = ScopeJson(request.scope);
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

