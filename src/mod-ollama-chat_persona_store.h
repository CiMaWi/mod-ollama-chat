#ifndef MOD_OLLAMA_CHAT_PERSONA_STORE_H
#define MOD_OLLAMA_CHAT_PERSONA_STORE_H

#include <chrono>
#include <list>
#include <string_view>
#include <unordered_map>

#include "mod-ollama-chat_persona_card.h"

namespace OllamaPersona
{
constexpr std::size_t MaxStoreEntries = 256;
constexpr std::size_t MaxStoreOperations = 16;
constexpr std::size_t MaxStoreCompletionsPerPump = 8;
constexpr auto StoreRetryDelay = std::chrono::seconds(30);
// DS.12: card opportunities per bot without a card in one bridge generation.
constexpr uint32_t MaxCardOffers = 3;
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
    // Drops the bot's row and its card opportunity count.
    void Forget(uint64_t botGuid);
    void Prepare(uint64_t generation, uint64_t botGuid, StoreTime now);
    StoreView Observe(uint64_t generation, uint64_t botGuid) const;
    // True means queued. Persisted state is available only through a subsequent confirmed observation.
    bool SubmitUpdate(uint64_t generation, uint64_t stamp, Update const& update);
    // True while the bot has card opportunities left in the current generation (does not count one).
    bool CanOffer(uint64_t generation, uint64_t botGuid) const;
    // Counts one card opportunity of the current generation; false once MaxCardOffers are used. The count is
    // reset by a new generation or reconfiguration and erased by Forget, not by a cache eviction.
    bool TakeOffer(uint64_t generation, uint64_t botGuid);
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
    std::unordered_map<uint64_t, uint32_t> _offers;  // Card opportunities of the current generation.
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

// Card context of one bridge capture. Unready means voice only: gate closed or no current observation; it adds
// nothing to the context or its signature. A loaded row without an accepted card is NoCard (a row from before
// J14e that held only drafts).
enum class CardStatus : uint8_t
{
    Unready,
    Loading,
    Missing,  // Confirmed absent row, revision zero.
    NoCard,
    Accepted
};

struct CardContext
{
    CardStatus status = CardStatus::Unready;
    uint64_t revision = 0;
    uint64_t stamp = 0;
    bool included = false;  // The persona text ends with the accepted body; false when none or it did not fit.
};

constexpr std::string_view CardSeparator = "\n\n";

// World only, cache only, no I/O. DS.12: a card is the bot's own data, read for every speaker and channel; the
// player never sees it. The accepted body is appended to the voice only if both fit in MaxCardBytes: the voice keeps
// priority and a card is never truncated. An omitted card still reads Accepted, with included false.
CardContext ReadCard(Store const& store, uint64_t generation, uint64_t botGuid, std::string& persona);
// Signature field of a capture, empty when Unready. Revision and stamp are canonical decimal.
std::string CardToken(CardContext const& card);

enum class ProposalStatus : uint8_t
{
    Queued,
    Disabled,
    Unauthenticated,
    Stale,
    Invalid,          // The body breaks the card rules.
    HasCard,          // The bot already has a card.
    ManualProtected,  // The owner's card is never replaced.
    Busy              // No write slot; nothing was staged.
};

char const* ProposalStatusName(ProposalStatus status);

// Provenance comes only from the authenticated provider of the bridge ticket, never from generated text.
std::optional<Source> ProviderSource(std::string_view provider);
// World only. DS.12: applies a provider's generated card directly (no acceptance step) to a bot without a card whose
// current observation still shows the expected revision and stamp. Queued is not persisted: the card is in the
// persona text once a fresh lookup confirms the write. A provider never replaces an existing card.
ProposalStatus ProposeCard(Store& store, uint64_t generation, uint64_t botGuid, std::string_view provider,
                           uint64_t expectedRevision, uint64_t stamp, std::string body);
}  // namespace OllamaPersona

#endif
