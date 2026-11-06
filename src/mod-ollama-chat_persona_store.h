#ifndef MOD_OLLAMA_CHAT_PERSONA_STORE_H
#define MOD_OLLAMA_CHAT_PERSONA_STORE_H

#include <chrono>
#include <list>
#include <unordered_map>

#include "mod-ollama-chat_persona_card.h"

namespace OllamaPersona
{
constexpr std::size_t MaxStoreEntries = 256;
constexpr std::size_t MaxStoreOperations = 16;
constexpr std::size_t MaxStoreCompletionsPerPump = 8;
constexpr auto StoreRetryDelay = std::chrono::seconds(30);
using StoreTime = std::chrono::steady_clock::time_point;

struct StoreWrite
{
    uint64_t priorRevision;
    State next;
};

struct StoreOperation
{
    uint64_t botGuid;
    uint64_t id;
    std::optional<StoreWrite> write;
};

enum class LookupStatus : uint8_t
{
    Failed,
    Missing,
    Found
};

struct StoreCompletion
{
    uint64_t botGuid;
    uint64_t id;
    LookupStatus lookup;
    std::optional<State> row;
    bool committed = false;
};

// Both port methods run on world and must return without waiting for I/O. The port owns accepted values and
// returns one terminal completion per accepted operation when its physical work has finished. A write reports
// the transaction outcome and a fresh lookup issued after successful commit. Missing requires explicit proof.
class StorePort
{
public:
    virtual ~StorePort() = default;
    virtual bool TrySubmit(StoreOperation operation) = 0;
    virtual std::optional<StoreCompletion> TryPop() = 0;
};

enum class StoreStatus : uint8_t
{
    Unavailable,
    Loading,
    Missing,
    Loaded
};

struct StoreView
{
    StoreStatus status = StoreStatus::Unavailable;
    // World-only borrow. Copy any needed data before a store mutation or handing work to another thread.
    State const* state = nullptr;
    // Unique to a validated cache snapshot, including reloads of the same persisted revision.
    uint64_t stamp = 0;
};

// The world thread owns every call. The caller revalidates the pair, authority, provenance and ticket before
// using a view or submitting an update. Activation uses the trusted bridge's monotonic world generation.
class Store
{
public:
    Store() = default;
    Store(Store const&) = delete;
    Store& operator=(Store const&) = delete;

    void Configure(bool enabled);
    bool Enabled() const { return _enabled; }
    // The caller keeps the port alive while attached. Replacement and detachment require terminal drain.
    bool SetPort(StorePort* port);
    bool Activate(uint64_t generation);
    void Retire(uint64_t generation);
    void Forget(uint64_t botGuid);
    void Prepare(uint64_t generation, uint64_t botGuid, StoreTime now);
    StoreView Observe(uint64_t generation, uint64_t botGuid) const;
    // True means queued. Persisted state is available only through a subsequent confirmed observation.
    bool SubmitUpdate(uint64_t generation, uint64_t stamp, bool acquainted, Update const& update);
    void Pump(StoreTime now);
    std::size_t Pending() const { return _pending; }

private:
    struct Entry
    {
        std::optional<State> state;
        std::optional<State> candidate;
        std::list<uint64_t>::iterator idle;
        StoreTime retryAt{};
        uint64_t id = 0;
        bool pending = false;
        bool invalidated = false;
    };

    using Entries = std::unordered_map<uint64_t, Entry>;

    bool IsCurrent(uint64_t generation) const;
    bool HasSlot() const;
    void InvalidateAll();
    Entries::iterator Invalidate(Entries::iterator it);
    void UnlinkIdle(Entry const& entry);
    bool MakeRoom(StoreTime now);
    void BackOff(uint64_t botGuid, Entry& entry, StoreTime now);
    void Finish(StoreCompletion completion, StoreTime now);

    Entries _entries;
    std::list<uint64_t> _ready;
    std::list<uint64_t> _cooling;
    StorePort* _port = nullptr;
    std::size_t _pending = 0;
    uint64_t _lastOperation = 0;
    uint64_t _lastGeneration = 0;
    uint64_t _generation = 0;
    bool _enabled = false;
};

Store& WorldStore();
}  // namespace OllamaPersona

#endif
