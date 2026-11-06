#include "mod-ollama-chat_persona_store.h"

#include <limits>
#include <utility>

namespace OllamaPersona
{
namespace
{
bool SameRow(State const& left, State const& right)
{
    return left.botGuid == right.botGuid && left.revision == right.revision &&
           left.claudeAlternative == right.claudeAlternative && left.codexAlternative == right.codexAlternative &&
           left.accepted.has_value() == right.accepted.has_value() &&
           (!left.accepted ||
            (left.accepted->source == right.accepted->source && left.accepted->body == right.accepted->body));
}
}  // namespace

void Store::Configure(bool enabled)
{
    _enabled = enabled;
    _generation = 0;
    InvalidateAll();
}

bool Store::SetPort(StorePort* port)
{
    if (_port == port)
        return true;
    if (_pending)
        return false;
    InvalidateAll();
    _port = port;
    return true;
}

bool Store::Activate(uint64_t generation)
{
    if (!_enabled || !generation)
        return false;
    if (generation == _generation)
        return true;
    if (generation <= _lastGeneration)
        return false;
    InvalidateAll();
    _lastGeneration = generation;
    _generation = generation;
    return true;
}

void Store::Retire(uint64_t generation)
{
    if (!IsCurrent(generation))
        return;
    _generation = 0;
    InvalidateAll();
}

void Store::Forget(uint64_t botGuid)
{
    auto it = _entries.find(botGuid);
    if (it != _entries.end())
        Invalidate(it);
}

bool Store::IsCurrent(uint64_t generation) const { return _enabled && generation && generation == _generation; }

bool Store::HasSlot() const
{
    return _port && _pending < MaxStoreOperations && _lastOperation != std::numeric_limits<uint64_t>::max();
}

void Store::InvalidateAll()
{
    for (auto it = _entries.begin(); it != _entries.end();)
        it = Invalidate(it);
}

Store::Entries::iterator Store::Invalidate(Entries::iterator it)
{
    if (it->second.pending)
    {
        it->second.invalidated = true;
        return ++it;
    }
    UnlinkIdle(it->second);
    return _entries.erase(it);
}

void Store::UnlinkIdle(Entry const& entry)
{
    auto& idle = entry.state ? _ready : _cooling;
    idle.erase(entry.idle);
}

bool Store::MakeRoom(StoreTime now)
{
    if (_entries.size() < MaxStoreEntries)
        return true;
    if (!_cooling.empty() && _entries.at(_cooling.front()).retryAt <= now)
        Forget(_cooling.front());
    else if (!_ready.empty())
        Forget(_ready.front());
    else
        return false;
    return true;
}

void Store::BackOff(uint64_t botGuid, Entry& entry, StoreTime now)
{
    entry.retryAt = now + StoreRetryDelay;
    entry.idle = _cooling.insert(_cooling.end(), botGuid);
}

void Store::Prepare(uint64_t generation, uint64_t botGuid, StoreTime now)
{
    if (!IsCurrent(generation) || !botGuid)
        return;
    auto it = _entries.find(botGuid);
    if (it != _entries.end())
    {
        Entry& entry = it->second;
        if (entry.state)
            _ready.splice(_ready.end(), _ready, entry.idle);
        if (entry.pending || entry.state || now < entry.retryAt || !HasSlot())
            return;
        UnlinkIdle(entry);
    }
    else
    {
        if (!HasSlot() || !MakeRoom(now))
            return;
        it = _entries.try_emplace(botGuid).first;
    }

    Entry& entry = it->second;
    entry.id = ++_lastOperation;
    if (_port->TrySubmit({botGuid, entry.id, std::nullopt}))
    {
        entry.pending = true;
        ++_pending;
    }
    else
        BackOff(botGuid, entry, now);
}

StoreView Store::Observe(uint64_t generation, uint64_t botGuid) const
{
    if (!IsCurrent(generation))
        return {};
    auto it = _entries.find(botGuid);
    if (it == _entries.end() || it->second.invalidated)
        return {};
    Entry const& entry = it->second;
    if (entry.pending)
        return {StoreStatus::Loading};
    if (!entry.state)
        return {};
    return {entry.state->revision ? StoreStatus::Loaded : StoreStatus::Missing, &*entry.state, entry.id};
}

bool Store::SubmitUpdate(uint64_t generation, uint64_t stamp, bool acquainted, Update const& update)
{
    if (!IsCurrent(generation) || !HasSlot())
        return false;
    auto it = _entries.find(update.botGuid);
    if (it == _entries.end() || !it->second.state || stamp != it->second.id)
        return false;
    Entry& entry = it->second;
    UpdatePlan plan = PlanUpdate(*entry.state, acquainted, update);
    if (!plan.next)
        return false;
    uint64_t const id = ++_lastOperation;
    if (!_port->TrySubmit({update.botGuid, id, StoreWrite{update.expectedRevision, *plan.next}}))
        return false;
    UnlinkIdle(entry);
    entry.state.reset();
    entry.candidate = std::move(plan.next);
    entry.id = id;
    entry.pending = true;
    ++_pending;
    return true;
}

void Store::Finish(StoreCompletion completion, StoreTime now)
{
    auto it = _entries.find(completion.botGuid);
    if (it == _entries.end() || !it->second.pending || it->second.id != completion.id)
        return;
    Entry& entry = it->second;
    --_pending;
    if (entry.invalidated)
    {
        _entries.erase(it);
        return;
    }

    entry.pending = false;
    bool const found = completion.lookup == LookupStatus::Found && completion.row && completion.row->revision &&
                       completion.row->botGuid == completion.botGuid && ValidState(*completion.row);
    if (entry.candidate)
    {
        // Exact readback proves this row is present even if a concurrent writer supplied it.
        if (completion.committed && found && SameRow(*completion.row, *entry.candidate))
            entry.state = std::move(completion.row);
        entry.candidate.reset();
    }
    else if (found)
        entry.state = std::move(completion.row);
    else if (completion.lookup == LookupStatus::Missing && !completion.row)
    {
        entry.state.emplace();
        entry.state->botGuid = completion.botGuid;
    }

    if (entry.state)
        entry.idle = _ready.insert(_ready.end(), completion.botGuid);
    else
        BackOff(completion.botGuid, entry, now);
}

void Store::Pump(StoreTime now)
{
    if (!_port)
        return;
    for (std::size_t i = 0; i < MaxStoreCompletionsPerPump; ++i)
    {
        auto completion = _port->TryPop();
        if (!completion)
            return;
        Finish(std::move(*completion), now);
    }
}

Store& WorldStore()
{
    static Store store;
    return store;
}

CardContext ReadCard(Store const& store, uint64_t generation, uint64_t botGuid, bool personal, std::string& persona)
{
    CardContext card{CardStatus::Unready, personal};
    if (!personal)
        return card;
    StoreView const view = store.Observe(generation, botGuid);
    if (view.status == StoreStatus::Loading)
        card.status = CardStatus::Loading;
    if (!view.state)
        return card;
    card.revision = view.state->revision;
    card.stamp = view.stamp;
    if (view.status == StoreStatus::Missing)
    {
        card.status = CardStatus::Missing;
        return card;
    }
    Card const* accepted = SelectAccepted(*view.state, true);
    card.status = accepted ? CardStatus::Accepted : CardStatus::DraftOnly;
    if (accepted && persona.size() + CardSeparator.size() + accepted->body.size() <= MaxCardBytes)
    {
        persona += CardSeparator;
        persona += accepted->body;
    }
    return card;
}

std::string CardToken(CardContext const& card)
{
    char const* name = nullptr;
    switch (card.status)
    {
        case CardStatus::Unready:
            return {};
        case CardStatus::Loading:
            name = "loading";
            break;
        case CardStatus::Missing:
            name = "missing";
            break;
        case CardStatus::DraftOnly:
            name = "draft";
            break;
        case CardStatus::Accepted:
            name = "accepted";
            break;
    }
    return std::string("card:") + name + ":" + std::to_string(card.revision) + ":" + std::to_string(card.stamp);
}

std::optional<Source> ProviderSource(std::string_view provider)
{
    if (provider == "claude")
        return Source::Claude;
    if (provider == "codex")
        return Source::Codex;
    return std::nullopt;
}

ProposalStatus ProposeCard(Store& store, uint64_t generation, uint64_t botGuid, bool personal,
                           std::string_view provider, uint64_t expectedRevision, uint64_t stamp, std::string body)
{
    if (!store.Enabled())
        return ProposalStatus::Disabled;
    auto const source = ProviderSource(provider);
    if (!source)
        return ProposalStatus::Unauthenticated;
    if (!personal)
        return ProposalStatus::Ineligible;
    StoreView const view = store.Observe(generation, botGuid);
    if (!view.state || view.stamp != stamp || view.state->revision != expectedRevision)
        return ProposalStatus::Stale;
    Update const update{UpdateKind::ProposeAlternative, botGuid, expectedRevision, *source, std::move(body)};
    return store.SubmitUpdate(generation, stamp, true, update) ? ProposalStatus::Queued : ProposalStatus::Rejected;
}
}  // namespace OllamaPersona
