#include "mod-ollama-chat_persona_card.h"

#include <utf8.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace OllamaPersona
{
namespace
{
bool ValidBody(std::string const& body)
{
    if (body.empty() || body.size() > MaxCardBytes || body.find_first_not_of(" \t\r\n") == std::string::npos)
        return false;

    return std::none_of(body.begin(), body.end(), [](unsigned char byte)
                        { return (byte < 0x20 && byte != '\t' && byte != '\r' && byte != '\n') || byte == 0x7F; }) &&
           utf8::is_valid(body.begin(), body.end());
}

bool ValidSource(Source source)
{
    return source == Source::Manual || source == Source::Claude || source == Source::Codex;
}

// Only called for a validated native provider.
std::string State::* AlternativeSlot(Source source)
{
    return source == Source::Claude ? &State::claudeAlternative : &State::codexAlternative;
}
}  // namespace

bool ValidState(State const& state)
{
    bool const hasCards = state.accepted || !state.claudeAlternative.empty() || !state.codexAlternative.empty();
    return state.botGuid != 0 && (state.revision != 0) == hasCards &&
           (!state.accepted || (ValidSource(state.accepted->source) && ValidBody(state.accepted->body))) &&
           (state.claudeAlternative.empty() || ValidBody(state.claudeAlternative)) &&
           (state.codexAlternative.empty() || ValidBody(state.codexAlternative));
}

Card const* SelectAccepted(State const& state, bool acquainted)
{
    return acquainted && state.accepted ? &*state.accepted : nullptr;
}

UpdatePlan PlanUpdate(State const& state, bool acquainted, Update const& update)
{
    if (!acquainted)
        return {UpdateStatus::Ineligible, std::nullopt};
    if (update.botGuid != state.botGuid || !ValidState(state) || !ValidSource(update.source))
        return {UpdateStatus::Invalid, std::nullopt};
    if (update.expectedRevision != state.revision)
        return {UpdateStatus::Stale, std::nullopt};

    std::string State::* slot = nullptr;
    std::string const* body = &update.body;
    switch (update.kind)
    {
        case UpdateKind::ProposeAlternative:
            if (update.source == Source::Manual || !ValidBody(*body))
                return {UpdateStatus::Invalid, std::nullopt};
            slot = AlternativeSlot(update.source);
            if (state.*slot == *body)
                return {UpdateStatus::Unchanged, std::nullopt};
            break;
        case UpdateKind::AcceptAlternative:
            if (update.source == Source::Manual || !body->empty())
                return {UpdateStatus::Invalid, std::nullopt};
            if (state.accepted && state.accepted->source == Source::Manual)
                return {UpdateStatus::ManualProtected, std::nullopt};
            body = &(state.*AlternativeSlot(update.source));
            if (body->empty())
                return {UpdateStatus::MissingAlternative, std::nullopt};
            break;
        case UpdateKind::SetManual:
            if (update.source != Source::Manual || !ValidBody(*body))
                return {UpdateStatus::Invalid, std::nullopt};
            break;
        default:
            return {UpdateStatus::Invalid, std::nullopt};
    }

    if (!slot && state.accepted && state.accepted->source == update.source && state.accepted->body == *body)
        return {UpdateStatus::Unchanged, std::nullopt};
    if (state.revision == std::numeric_limits<uint64_t>::max())
        return {UpdateStatus::RevisionExhausted, std::nullopt};

    State next = state;
    ++next.revision;
    if (slot)
        next.*slot = *body;
    else
        next.accepted = Card{update.source, *body};
    return {UpdateStatus::Ready, std::move(next)};
}
}  // namespace OllamaPersona
