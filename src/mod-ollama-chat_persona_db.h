#ifndef MOD_OLLAMA_CHAT_PERSONA_DB_H
#define MOD_OLLAMA_CHAT_PERSONA_DB_H

#include <deque>
#include <functional>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mod-ollama-chat_persona_store.h"

namespace OllamaPersona
{
// Views borrow the prepared result only for the duration of its world-thread callback.
struct PersonaDbRow
{
    uint64_t requestedGuid;
    std::optional<uint64_t> botGuid;
    std::optional<uint64_t> revision;
    std::optional<uint8_t> acceptedSource;
    std::optional<std::string_view> acceptedBody;
    std::optional<std::string_view> claudeAlternative;
    std::optional<std::string_view> codexAlternative;
};

inline StoreCompletion DecodePersonaLookup(uint64_t botGuid, uint64_t id, bool committed,
                                           std::optional<PersonaDbRow> const& row)
{
    StoreCompletion result{botGuid, id, LookupStatus::Failed, std::nullopt, committed};
    if (!botGuid || !row || row->requestedGuid != botGuid)
        return result;
    if (!row->botGuid)
    {
        if (!row->revision && !row->acceptedSource && !row->acceptedBody && !row->claudeAlternative &&
            !row->codexAlternative)
            result.lookup = LookupStatus::Missing;
        return result;
    }
    auto const bounded = [](std::optional<std::string_view> const& body)
    { return !body || (!body->empty() && body->size() <= MaxCardBytes); };
    if (*row->botGuid != botGuid || !row->revision || !*row->revision || !row->acceptedSource ||
        row->acceptedBody.has_value() != (*row->acceptedSource != 0) || !bounded(row->acceptedBody) ||
        !bounded(row->claudeAlternative) || !bounded(row->codexAlternative))
        return result;

    State state{botGuid, *row->revision, std::nullopt, std::string(row->claudeAlternative.value_or("")),
                std::string(row->codexAlternative.value_or(""))};
    if (row->acceptedBody)
        state.accepted = Card{static_cast<Source>(*row->acceptedSource), std::string(*row->acceptedBody)};
    if (ValidState(state))
    {
        result.lookup = LookupStatus::Found;
        result.row = std::move(state);
    }
    return result;
}

// The prepared backend starts asynchronous work and invokes callbacks only in Poll on world.
class PersonaDbIo
{
public:
    using LookupCallback = std::function<void(std::optional<PersonaDbRow> const&)>;
    using CommitCallback = std::function<void(bool)>;

    virtual ~PersonaDbIo() = default;
    virtual void Lookup(uint64_t botGuid, LookupCallback callback) = 0;
    virtual void Commit(std::vector<StoreOperation> const& writes, CommitCallback callback) = 0;
    virtual void Poll() = 0;
    // Join issued SQL work and dispose of its callbacks, without starting additional queries.
    virtual void Close() = 0;
};

// Bounded world-thread adapter. A slot includes staged, issued and ready-for-disposal work.
class PersonaDbPort final : public StorePort
{
public:
    explicit PersonaDbPort(PersonaDbIo& io) : _io(io) {}

    bool TrySubmit(StoreOperation operation) override
    {
        auto const botGuid = operation.botGuid;
        auto const id = operation.id;
        if (_closed || !botGuid || !id || _pending.size() >= MaxStoreOperations)
            return false;
        if (operation.write)
        {
            auto const& write = *operation.write;
            if (write.next.botGuid != botGuid || write.priorRevision == std::numeric_limits<uint64_t>::max() ||
                write.next.revision != write.priorRevision + 1 || !ValidState(write.next))
                return false;
        }
        if (!_pending.try_emplace(botGuid, id).second)
            return false;
        if (operation.write)
            _staged.push_back(std::move(operation));
        else
            StartLookup(botGuid, id, false);
        return true;
    }

    std::optional<StoreCompletion> TryPop() override
    {
        if (_ready.empty())
            return std::nullopt;
        StoreCompletion result = std::move(_ready.front());
        _ready.pop_front();
        _pending.erase(result.botGuid);
        return result;
    }

    void Pump()
    {
        if (_closed)
            return;
        if (!_staged.empty())
        {
            std::vector<std::pair<uint64_t, uint64_t>> keys;
            keys.reserve(_staged.size());
            for (auto const& write : _staged)
                keys.emplace_back(write.botGuid, write.id);
            _io.Commit(_staged,
                       [this, keys = std::move(keys)](bool committed)
                       {
                           for (auto const& [botGuid, id] : keys)
                           {
                               if (committed)
                                   StartLookup(botGuid, id, true);
                               else
                                   Fail(botGuid, id);
                           }
                       });
            _staged.clear();
        }
        _io.Poll();
    }

    // Retire a staged write before physical enqueue. Issued work remains pinned through terminal disposal.
    void DiscardStaged(std::optional<uint64_t> botGuid = {})
    {
        std::erase_if(_staged,
                      [&](StoreOperation const& write)
                      {
                          if (botGuid && *botGuid != write.botGuid)
                              return false;
                          Fail(write.botGuid, write.id);
                          return true;
                      });
    }

    void Close()
    {
        _closed = true;
        _io.Close();
        _staged.clear();
        _ready.clear();
        for (auto const& [botGuid, id] : _pending)
            Fail(botGuid, id);
    }

private:
    void StartLookup(uint64_t botGuid, uint64_t id, bool committed)
    {
        _io.Lookup(botGuid, [this, botGuid, id, committed](std::optional<PersonaDbRow> const& row)
                   { _ready.push_back(DecodePersonaLookup(botGuid, id, committed, row)); });
    }

    void Fail(uint64_t botGuid, uint64_t id) { _ready.push_back({botGuid, id, LookupStatus::Failed, std::nullopt}); }

    PersonaDbIo& _io;
    std::unordered_map<uint64_t, uint64_t> _pending;
    std::vector<StoreOperation> _staged;
    std::deque<StoreCompletion> _ready;
    bool _closed = false;
};

// Registers the keep-alive that pings the persona pool with the core's own pools.
void AddPersonaDbScripts();
// World-thread lifecycle. Open only on enabled startup, after Characters schema setup.
void StartPersonaDb();
void PumpPersonaDb();
// Call before retiring the generation (all staged writes) or forgetting a bot (its staged write).
void DiscardStagedPersonaWrites(std::optional<uint64_t> botGuid = {});
void StopPersonaDb();
}  // namespace OllamaPersona

#endif
