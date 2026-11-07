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
}  // namespace

bool ValidState(State const& state)
{
    return state.botGuid != 0 && (!state.accepted || (state.revision != 0 && ValidSource(state.accepted->source) &&
                                                      ValidBody(state.accepted->body)));
}

UpdatePlan PlanUpdate(State const& state, Update const& update)
{
    if (update.botGuid != state.botGuid || !ValidState(state) || !ValidSource(update.source))
        return {UpdateStatus::Invalid, std::nullopt};
    if (update.expectedRevision != state.revision)
        return {UpdateStatus::Stale, std::nullopt};

    bool const same = state.accepted && state.accepted->source == update.source && state.accepted->body == update.body;
    switch (update.kind)
    {
        case UpdateKind::ApplyGenerated:
            if (update.source == Source::Manual || update.body.size() > MaxGeneratedCardBytes ||
                !ValidBody(update.body))
                return {UpdateStatus::Invalid, std::nullopt};
            if (state.accepted && state.accepted->source == Source::Manual)
                return {UpdateStatus::ManualProtected, std::nullopt};
            if (state.accepted)
                return {same ? UpdateStatus::Unchanged : UpdateStatus::HasCard, std::nullopt};
            break;
        case UpdateKind::SetManual:
            if (update.source != Source::Manual || !ValidBody(update.body))
                return {UpdateStatus::Invalid, std::nullopt};
            if (same)
                return {UpdateStatus::Unchanged, std::nullopt};
            break;
        default:
            return {UpdateStatus::Invalid, std::nullopt};
    }

    if (state.revision == std::numeric_limits<uint64_t>::max())
        return {UpdateStatus::RevisionExhausted, std::nullopt};

    State next = state;
    ++next.revision;
    next.accepted = Card{update.source, update.body};
    return {UpdateStatus::Ready, std::move(next)};
}
}  // namespace OllamaPersona
