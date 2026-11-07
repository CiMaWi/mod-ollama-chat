#include <cstdlib>
#include <deque>
#include <iostream>
#include <utility>

#include "mod-ollama-chat_persona_db.h"

namespace
{
using namespace OllamaPersona;

constexpr uint64_t Bot = 0xF000000000000042;
constexpr uint64_t OtherBot = 0xE000000000000042;
constexpr StoreTime Start{};
std::size_t checks = 0;

void Check(bool condition, char const* label)
{
    ++checks;
    if (!condition)
    {
        std::cerr << "FAIL: " << label << '\n';
        std::exit(1);
    }
}

class FakePort : public StorePort
{
public:
    bool TrySubmit(StoreOperation operation) override
    {
        ++submissions;
        if (!accepting)
            return false;
        jobs.push_back(std::move(operation));
        return true;
    }

    std::optional<StoreCompletion> TryPop() override
    {
        if (ready.empty())
            return std::nullopt;
        StoreCompletion result = std::move(ready.front());
        ready.pop_front();
        return result;
    }

    StoreOperation Take()
    {
        Check(!jobs.empty(), "a physical job is available");
        StoreOperation job = std::move(jobs.front());
        jobs.pop_front();
        return job;
    }

    void Complete(StoreOperation const& job, LookupStatus lookup, std::optional<State> row = {}, bool committed = false)
    {
        ready.push_back({job.botGuid, job.id, lookup, std::move(row), committed});
    }

    bool accepting = true;
    std::size_t submissions = 0;
    std::deque<StoreOperation> jobs;
    std::deque<StoreCompletion> ready;
};

void Enable(Store& store, FakePort& port)
{
    Check(store.SetPort(&port), "attach port");
    store.Configure(true);
    Check(store.Activate(1), "activate first generation");
}

void LoadMissing(Store& store, FakePort& port, uint64_t generation, uint64_t botGuid, StoreTime now)
{
    store.Prepare(generation, botGuid, now);
    auto job = port.Take();
    Check(job.botGuid == botGuid && !job.write, "missing lookup uses the full GUID");
    port.Complete(job, LookupStatus::Missing);
    store.Pump(now);
}

void CheckLoads()
{
    FakePort port;
    Store store;
    Check(!store.Enabled() && !store.Activate(1), "feature defaults off");
    store.Prepare(1, Bot, Start);
    Check(!port.submissions, "disabled preparation has no I/O");
    store.Configure(true);
    Check(store.Activate(1), "enabled generation can bind before port startup");
    store.Prepare(1, Bot, Start);
    Check(store.Observe(1, Bot).status == StoreStatus::Unavailable, "missing port fails closed");
    Check(store.SetPort(&port), "attach available port");
    store.Prepare(1, Bot, Start);
    store.Prepare(1, Bot, Start);
    Check(store.Observe(1, Bot).status == StoreStatus::Loading, "pending lookup is loading");
    Check(port.submissions == 1 && store.Pending() == 1, "lookup deduplicated");
    auto first = port.Take();
    port.Complete(first, LookupStatus::Failed);
    store.Pump(Start);
    Check(store.Observe(1, Bot).status == StoreStatus::Unavailable, "lookup failure is unavailable");
    store.Prepare(1, Bot, Start + StoreRetryDelay - std::chrono::milliseconds(1));
    Check(port.submissions == 1, "lookup cannot retry before cooldown");

    auto now = Start + StoreRetryDelay;
    store.Prepare(1, Bot, now);
    auto second = port.Take();
    port.Complete(first, LookupStatus::Missing);
    store.Pump(now);
    Check(store.Pending() == 1 && store.Observe(1, Bot).status == StoreStatus::Loading, "stale reply keeps newer pin");
    port.Complete(second, LookupStatus::Found, State{OtherBot, 1, {}});
    store.Pump(now);
    Check(store.Observe(1, Bot).status == StoreStatus::Unavailable, "same low counter cannot substitute full GUID");

    now += StoreRetryDelay;
    store.Prepare(1, Bot, now);
    port.Complete(port.Take(), LookupStatus::Found, State{Bot, 0, {}});
    store.Pump(now);
    Check(!store.Observe(1, Bot).state, "zero revision row cannot masquerade as confirmed missing");
    now += StoreRetryDelay;
    store.Prepare(1, Bot, now);
    port.Complete(port.Take(), LookupStatus::Found, State{Bot, 1, Card{Source::Codex, ""}});
    store.Pump(now);
    Check(!store.Observe(1, Bot).state, "corrupt accepted slot is unavailable");
    now += StoreRetryDelay;
    store.Prepare(1, Bot, now);
    port.Complete(port.Take(), LookupStatus::Missing, State{Bot, 1, {}});
    store.Pump(now);
    Check(!store.Observe(1, Bot).state, "missing outcome cannot carry a row");

    now += StoreRetryDelay;
    LoadMissing(store, port, 1, Bot, now);
    auto missing = store.Observe(1, Bot);
    Check(missing.status == StoreStatus::Missing && missing.state && missing.stamp && missing.state->botGuid == Bot &&
              !missing.state->revision && !missing.state->accepted,
          "confirmed missing carries exact key and no card");
    port.Complete(second, LookupStatus::Missing);
    store.Pump(now);
    Check(store.Pending() == 0 && store.Observe(1, Bot).stamp == missing.stamp, "duplicate does not replace snapshot");
    store.Prepare(1, OtherBot, now);
    port.Complete(port.Take(), LookupStatus::Found, State{OtherBot, 8, {}});
    store.Pump(now);
    auto loaded = store.Observe(1, OtherBot);
    Check(
        loaded.status == StoreStatus::Loaded && loaded.state && !loaded.state->accepted && loaded.state->revision == 8,
        "a row from before J14e that held only drafts loads as no card");
}

void CheckWrites()
{
    FakePort port;
    Store store;
    Enable(store, port);
    LoadMissing(store, port, 1, Bot, Start);
    uint64_t const firstStamp = store.Observe(1, Bot).stamp;
    Update proposal{UpdateKind::SetManual, Bot, 0, Source::Manual, "first card"};
    Check(!store.SubmitUpdate(2, firstStamp, proposal), "wrong generation refused");
    Check(!store.SubmitUpdate(1, firstStamp + 1, proposal), "wrong snapshot refused");
    proposal.expectedRevision = 1;
    Check(!store.SubmitUpdate(1, firstStamp, proposal), "wrong expected revision refused");
    proposal.expectedRevision = 0;
    Check(port.jobs.empty(), "refused writes queue no work");

    port.accepting = false;
    Check(!store.SubmitUpdate(1, firstStamp, proposal) && store.Observe(1, Bot).stamp == firstStamp,
          "port refusal preserves confirmed state");
    port.accepting = true;
    Check(store.SubmitUpdate(1, firstStamp, proposal), "absent insert queued");
    Check(!store.SubmitUpdate(1, firstStamp, proposal), "pending insert blocks second write");
    auto insert = port.Take();
    Check(insert.write && insert.write->priorRevision == 0 && insert.write->next.revision == 1,
          "first write requests absent insert");
    Check(store.Observe(1, Bot).status == StoreStatus::Loading && !store.Observe(1, Bot).state,
          "proposed row is hidden until confirmation");
    State conflicting{Bot, 1, Card{Source::Manual, "operator card"}};
    port.Complete(insert, LookupStatus::Found, conflicting, false);
    store.Pump(Start);
    Check(!store.Observe(1, Bot).state, "failed insert conflict does not publish a row");

    auto now = Start + StoreRetryDelay;
    store.Prepare(1, Bot, now);
    port.Complete(port.Take(), LookupStatus::Found, conflicting);
    store.Pump(now);
    auto loaded = store.Observe(1, Bot);
    Check(loaded.state && loaded.state->revision == 1 && loaded.state->accepted->source == Source::Manual,
          "ordinary reload may publish the conflicting persisted row");
    proposal.expectedRevision = 1;
    Check(store.SubmitUpdate(1, loaded.stamp, proposal), "existing-row CAS queued");
    auto cas = port.Take();
    Check(cas.write && cas.write->priorRevision == 1 && cas.write->next.revision == 2,
          "CAS uses exact prior revision and next row");
    port.Complete(cas, LookupStatus::Found, conflicting, true);
    store.Pump(now);
    Check(!store.Observe(1, Bot).state, "committed zero-row CAS with old readback stays unconfirmed");

    State newer{Bot, 3, Card{Source::Manual, "operator card"}};
    now += StoreRetryDelay;
    store.Prepare(1, Bot, now);
    port.Complete(port.Take(), LookupStatus::Found, newer);
    store.Pump(now);
    loaded = store.Observe(1, Bot);
    proposal.expectedRevision = 3;
    Check(store.SubmitUpdate(1, loaded.stamp, proposal), "write after reload uses newest persisted revision");
    auto failedReadback = port.Take();
    port.Complete(failedReadback, LookupStatus::Failed, {}, true);
    store.Pump(now);
    Check(!store.Observe(1, Bot).state, "failed readback cannot publish even after successful commit");

    now += StoreRetryDelay;
    store.Prepare(1, Bot, now);
    port.Complete(port.Take(), LookupStatus::Found, newer);
    store.Pump(now);
    Check(store.SubmitUpdate(1, store.Observe(1, Bot).stamp, proposal), "candidate can be tried after reload");
    auto mismatched = port.Take();
    State changedProvenance = mismatched.write->next;
    changedProvenance.accepted->source = Source::Claude;
    port.Complete(mismatched, LookupStatus::Found, changedProvenance, true);
    store.Pump(now);
    Check(!store.Observe(1, Bot).state, "matching revision and text with wrong provenance do not confirm");

    now += StoreRetryDelay;
    store.Prepare(1, Bot, now);
    port.Complete(port.Take(), LookupStatus::Found, newer);
    store.Pump(now);
    Check(store.SubmitUpdate(1, store.Observe(1, Bot).stamp, proposal), "newer-row race can queue");
    auto lostRace = port.Take();
    State afterRace = lostRace.write->next;
    ++afterRace.revision;
    port.Complete(lostRace, LookupStatus::Found, afterRace, true);
    store.Pump(now);
    Check(!store.Observe(1, Bot).state, "newer row does not confirm the submitted candidate");

    now += StoreRetryDelay;
    store.Prepare(1, Bot, now);
    port.Complete(port.Take(), LookupStatus::Found, newer);
    store.Pump(now);
    Check(store.SubmitUpdate(1, store.Observe(1, Bot).stamp, proposal), "exact-row race can queue");
    auto exact = port.Take();
    port.Complete(exact, LookupStatus::Found, exact.write->next, true);
    store.Pump(now);
    auto confirmed = store.Observe(1, Bot);
    Check(confirmed.state && confirmed.state->revision == 4 && confirmed.state->accepted->source == Source::Manual &&
              confirmed.state->accepted->body == "first card",
          "exact concurrent candidate proves persisted state");
}

void CheckLifecycle()
{
    FakePort port;
    Store store;
    Enable(store, port);
    LoadMissing(store, port, 1, Bot, Start);
    uint64_t const oldStamp = store.Observe(1, Bot).stamp;
    store.Forget(Bot);
    LoadMissing(store, port, 1, Bot, Start);
    uint64_t const sameRevisionStamp = store.Observe(1, Bot).stamp;
    Update proposal{UpdateKind::ApplyGenerated, Bot, 0, Source::Codex, "A retiring smith."};
    Check(oldStamp != sameRevisionStamp && !store.SubmitUpdate(1, oldStamp, proposal),
          "same-revision reload after logout invalidates old generation opportunity");
    Check(store.SubmitUpdate(1, sameRevisionStamp, proposal), "write can use freshly loaded snapshot");
    auto retiredWrite = port.Take();
    store.Forget(Bot);
    store.Configure(false);
    Check(!store.Enabled() && !store.Observe(1, Bot).state && store.Pending() == 1, "disable keeps pending write pin");
    Check(!store.SetPort(nullptr), "port cannot detach while retired work is outstanding");
    store.Configure(true);
    Check(!store.Activate(1) && store.Activate(2), "reload cannot reactivate the retired generation");
    store.Retire(1);
    Check(!store.Activate(1), "out-of-order old retirement cannot restore old generation");
    LoadMissing(store, port, 2, OtherBot, Start);
    Check(store.Observe(2, OtherBot).state, "out-of-order old retirement leaves current generation usable");
    store.Prepare(2, Bot, Start);
    Check(port.jobs.empty() && store.Pending() == 1, "retired bot remains physically pinned in newer generation");
    port.Complete(retiredWrite, LookupStatus::Found, retiredWrite.write->next, true);
    store.Pump(Start);
    Check(!store.Observe(2, Bot).state && store.Pending() == 0, "retired commit drains without publication");
    store.Prepare(2, Bot, Start);
    auto reload = port.Take();
    port.Complete(reload, LookupStatus::Found, retiredWrite.write->next);
    store.Pump(Start);
    Check(store.Observe(2, Bot).state && store.Observe(2, Bot).state->revision == 1,
          "new generation reconciles actual persisted state through a fresh read");
    store.Retire(2);
    Check(!store.Observe(2, Bot).state && !store.Activate(2), "a retired generation reads nothing");
    Check(store.Activate(3), "new generation after retirement");
    // J5e: a bridge restart keeps the loaded row; only the current generation reads or updates it.
    Check(store.Observe(3, Bot).state && store.Observe(3, Bot).state->revision == 1 && store.Pending() == 0 &&
              port.jobs.empty(),
          "a loaded row survives retirement and a new generation without a new read");
    Check(!store.Observe(2, Bot).state &&
              !store.SubmitUpdate(2, store.Observe(3, Bot).stamp,
                                  Update{UpdateKind::SetManual, Bot, 1, Source::Manual, "old ticket"}),
          "an older generation's read and update stay refused");
    store.Forget(Bot);
    store.Prepare(3, Bot, Start);
    auto retiredLoad = port.Take();
    store.Configure(false);
    port.Complete(retiredLoad, LookupStatus::Missing);
    store.Pump(Start);
    Check(store.Pending() == 0 && store.SetPort(nullptr), "disabled pump drains and permits port detachment");
}

// J5e: `.ollama reload` (same enabled state) and a new bridge generation keep loaded rows; the generation gate stays.
void CheckReloadKeepsRows()
{
    FakePort port;
    Store store;
    Enable(store, port);
    LoadMissing(store, port, 1, Bot, Start);
    uint64_t const stamp = store.Observe(1, Bot).stamp;
    store.Configure(true);
    Check(!store.Observe(1, Bot).state, "a reload unbinds the generation until the bridge activates one");
    Check(store.Activate(2) && store.Observe(2, Bot).state && store.Observe(2, Bot).stamp == stamp &&
              port.jobs.empty() && store.Pending() == 0,
          "a same-enabled reload keeps the loaded row for the next generation without a read");
    store.Prepare(2, Bot, Start);
    Check(port.jobs.empty(), "a kept row needs no new read");
    Check(
        !store.Observe(1, Bot).state &&
            !store.SubmitUpdate(1, stamp, Update{UpdateKind::ApplyGenerated, Bot, 0, Source::Codex, "An old ticket."}),
        "an old-generation read and update are refused");
    Check(store.Activate(3) && store.Observe(3, Bot).state && !store.Observe(2, Bot).state,
          "a newer generation keeps the row and refuses the previous one");
    store.Configure(false);
    store.Configure(true);
    Check(store.Activate(4) && !store.Observe(4, Bot).state, "an off/on change empties the rows");
}

void CheckCapacity()
{
    FakePort port;
    Store store;
    Enable(store, port);
    for (uint64_t guid = 1; guid <= MaxStoreOperations; ++guid)
        store.Prepare(1, guid, Start);
    store.Prepare(1, Bot, Start);
    Check(port.submissions == MaxStoreOperations && store.Pending() == MaxStoreOperations, "physical queue capped");
    auto first = port.Take();
    for (std::size_t i = 0; i < MaxStoreCompletionsPerPump; ++i)
        port.Complete(first, LookupStatus::Missing);
    while (!port.jobs.empty())
        port.Complete(port.Take(), LookupStatus::Missing);
    store.Pump(Start);
    Check(store.Pending() == MaxStoreOperations - 1 && port.ready.size() == MaxStoreOperations - 1,
          "duplicate callbacks count against the bounded pump");
    while (!port.ready.empty())
        store.Pump(Start);
    Check(!store.Pending(), "later pumps drain remaining physical operations");

    for (uint64_t guid = MaxStoreOperations + 1; guid <= MaxStoreEntries; ++guid)
        LoadMissing(store, port, 1, guid, Start);
    for (uint32_t i = 0; i < MaxCardOffers; ++i)
        Check(store.TakeOffer(1, 2), "offers before an eviction");
    uint64_t const evictedStamp = store.Observe(1, 2).stamp;
    store.Prepare(1, 1, Start);
    store.Prepare(1, Bot, Start);
    auto pinned = port.Take();
    Check(store.Observe(1, 1).state && !store.Observe(1, 2).state, "capacity evicts the least recently used idle row");
    Check(!store.TakeOffer(1, 2), "an eviction keeps the bot's card opportunity count");
    LoadMissing(store, port, 1, 2, Start);
    Check(store.Observe(1, 2).stamp != evictedStamp && store.Observe(1, Bot).status == StoreStatus::Loading,
          "eviction reload changes the stamp and keeps pending keys pinned");
    port.Complete(pinned, LookupStatus::Missing);
    store.Pump(Start);

    store.Configure(false);  // J5e: only an off/on change empties the rows; the cooling check needs empty rows.
    store.Configure(true);
    Check(store.Activate(2), "off/on reload binds a new generation");
    port.accepting = false;
    for (uint64_t guid = 1; guid <= MaxStoreEntries; ++guid)
        store.Prepare(2, guid, Start);
    auto const rejectedCount = port.submissions;
    store.Prepare(2, Bot, Start);
    store.Prepare(2, 1, Start);
    Check(port.submissions == rejectedCount && !store.Pending(), "cooling entries cannot be evicted to bypass retry");
    port.accepting = true;
    LoadMissing(store, port, 2, Bot, Start + StoreRetryDelay);
    Check(store.Observe(2, Bot).state, "expired cooldown frees space for a new bot");
}

// J14e (DS.12): at most MaxCardOffers card opportunities per bot in one bridge generation.
void CheckOffers()
{
    FakePort port;
    Store store;
    Check(!store.TakeOffer(1, Bot), "a disabled store offers nothing");
    Enable(store, port);
    Check(!store.TakeOffer(2, Bot) && !store.TakeOffer(1, 0), "another generation or no bot is offered nothing");
    for (uint32_t i = 0; i < MaxCardOffers; ++i)
    {
        Check(store.CanOffer(1, Bot) && store.CanOffer(1, Bot), "checking an offer does not use it up");
        Check(store.TakeOffer(1, Bot), "an offer within the bound");
    }
    Check(!store.CanOffer(1, Bot) && !store.CanOffer(2, Bot) && !store.CanOffer(1, 0), "no offer left to check");
    Check(!store.TakeOffer(1, Bot), "the fourth offer in a generation is refused");
    Check(store.TakeOffer(1, OtherBot), "the bound is per bot");
    store.Forget(Bot);
    for (uint32_t i = 0; i < MaxCardOffers; ++i)
        Check(store.TakeOffer(1, Bot), "forget erases the bot's count");
    Check(!store.TakeOffer(1, Bot), "a forgotten bot gets the same bound again");
    Check(store.Activate(2) && !store.TakeOffer(1, Bot), "a new generation refuses the old one");
    for (uint32_t i = 0; i < MaxCardOffers; ++i)
        Check(store.TakeOffer(2, Bot), "a new generation resets the count");
    Check(!store.TakeOffer(2, Bot), "the bound holds in the new generation");
    store.Configure(true);
    Check(!store.TakeOffer(2, Bot) && store.Activate(3), "a reload unbinds the generation");
    for (uint32_t i = 0; i < MaxCardOffers; ++i)
        Check(store.TakeOffer(3, Bot), "a reload resets the count");
    Check(!store.TakeOffer(3, Bot), "the bound holds after a reload");
}

void CheckCards()
{
    Store store;
    FakePort port;
    std::string const voice = "A quiet traveler.";
    std::string persona = voice;
    auto const read = [&](uint64_t generation, uint64_t guid)
    {
        persona = voice;
        return ReadCard(store, generation, guid, persona);
    };
    auto const propose = [&](std::string_view provider, uint64_t revision, uint64_t stamp, std::string body)
    { return ProposeCard(store, 1, Bot, provider, revision, stamp, std::move(body)); };

    CardContext card = read(1, Bot);
    Check(card.status == CardStatus::Unready && persona == voice && CardToken(card).empty(),
          "closed gate leaves voice and signature unchanged");
    Check(propose("claude", 0, 1, "A smith.") == ProposalStatus::Disabled, "closed gate refuses proposals");
    Enable(store, port);
    card = read(1, Bot);
    Check(card.status == CardStatus::Unready && persona == voice && CardToken(card).empty(),
          "unobserved card stays voice only");
    store.Prepare(1, Bot, Start);
    card = read(1, Bot);
    Check(card.status == CardStatus::Loading && CardToken(card) == "card:loading:0:0", "loading is distinct");
    auto job = port.Take();
    port.Complete(job, LookupStatus::Missing);
    store.Pump(Start);
    card = read(1, Bot);
    Check(card.status == CardStatus::Missing && card.revision == 0 && card.stamp == job.id && persona == voice &&
              CardToken(card) == "card:missing:0:" + std::to_string(job.id),
          "confirmed missing row is revision zero and distinct");
    Check(read(2, Bot).status == CardStatus::Unready, "another generation reads nothing");

    uint64_t const missingStamp = card.stamp;
    Check(ProviderSource("claude") == Source::Claude && ProviderSource("codex") == Source::Codex &&
              !ProviderSource("Claude") && !ProviderSource("manual") && !ProviderSource(""),
          "provenance maps only exact authenticated provider names");
    Check(propose("ollama", 0, missingStamp, "A smith.") == ProposalStatus::Unauthenticated,
          "unknown provider cannot propose");
    Check(propose("claude", 1, missingStamp, "A smith.") == ProposalStatus::Stale &&
              propose("claude", 0, missingStamp + 1, "A smith.") == ProposalStatus::Stale &&
              ProposeCard(store, 2, Bot, "claude", 0, missingStamp, "A smith.") == ProposalStatus::Stale,
          "wrong revision, stamp or generation is stale");
    Check(propose("claude", 0, missingStamp, " ") == ProposalStatus::Invalid &&
              propose("claude", 0, missingStamp, std::string("A\0smith", 7)) == ProposalStatus::Invalid &&
              propose("claude", 0, missingStamp, "A \x1B[31msmith") == ProposalStatus::Invalid &&
              propose("claude", 0, missingStamp, "A smith \xC3") == ProposalStatus::Invalid &&
              propose("claude", 0, missingStamp, std::string(MaxGeneratedCardBytes + 1, 's')) ==
                  ProposalStatus::Invalid &&
              port.jobs.empty(),
          "a card breaking the module's rules is refused without I/O");
    port.accepting = false;
    Check(propose("codex", 0, missingStamp, "A smith from Goldshire.") == ProposalStatus::Busy &&
              store.Observe(1, Bot).stamp == missingStamp,
          "a refused write slot is busy and keeps the confirmed row");
    port.accepting = true;
    std::string const longest(MaxGeneratedCardBytes, 's');
    Check(PlanUpdate(*store.Observe(1, Bot).state, {UpdateKind::ApplyGenerated, Bot, 0, Source::Claude, longest})
                  .status == UpdateStatus::Ready,
          "a card of exactly the generated bound is valid");
    Check(propose("codex", 0, missingStamp, "A smith from Goldshire.") == ProposalStatus::Queued,
          "a valid generated card is applied without an acceptance step");
    auto write = port.Take();
    Check(write.write && write.write->priorRevision == 0 && write.write->next.revision == 1 &&
              write.write->next.accepted && write.write->next.accepted->source == Source::Codex &&
              write.write->next.accepted->body == "A smith from Goldshire.",
          "the write is the accepted card at revision one with the provider's provenance");
    Check(read(1, Bot).status == CardStatus::Loading &&
              propose("codex", 0, missingStamp, "Again.") == ProposalStatus::Stale,
          "pending write reads as loading and its old snapshot is stale");
    port.Complete(write, LookupStatus::Found, write.write->next, true);
    store.Pump(Start);
    card = read(1, Bot);
    Check(card.status == CardStatus::Accepted && card.revision == 1 && card.stamp == write.id && card.included &&
              persona == voice + "\n\nA smith from Goldshire." &&
              CardToken(card) == "card:accepted:1:" + std::to_string(write.id),
          "a confirmed card follows the voice from the next read");
    Check(propose("claude", 1, card.stamp, "A different smith.") == ProposalStatus::HasCard &&
              propose("codex", 1, card.stamp, "A smith from Goldshire.") == ProposalStatus::HasCard &&
              port.jobs.empty(),
          "a provider never replaces a card, a second apply is refused");
    std::string const acceptedToken = CardToken(card);
    store.Forget(Bot);
    store.Prepare(1, Bot, Start);
    auto reload = port.Take();
    port.Complete(reload, LookupStatus::Found, write.write->next);
    store.Pump(Start);
    CardContext const reloaded = read(1, Bot);
    Check(reloaded.revision == 1 && persona == voice + "\n\nA smith from Goldshire." &&
              CardToken(reloaded) != acceptedToken,
          "same-revision reload with unchanged text changes the signed token");
    Check(propose("codex", 1, card.stamp, "Old view.") == ProposalStatus::Stale,
          "proposal on a replaced snapshot is stale");

    Check(store.SubmitUpdate(1, reloaded.stamp, {UpdateKind::SetManual, Bot, 1, Source::Manual, "An owner's smith."}),
          "the owner's edit may replace a generated card");
    write = port.Take();
    port.Complete(write, LookupStatus::Found, write.write->next, true);
    store.Pump(Start);
    card = read(1, Bot);
    Check(card.status == CardStatus::Accepted && card.revision == 2 && persona == voice + "\n\nAn owner's smith.",
          "the owner's card follows the voice");
    Check(propose("claude", 2, card.stamp, "A smith.") == ProposalStatus::ManualProtected && port.jobs.empty(),
          "a manual card is never replaced by a provider");

    uint64_t const legacyBot = 0xD000000000000042;
    store.Prepare(1, legacyBot, Start);
    auto legacy = port.Take();
    port.Complete(legacy, LookupStatus::Found, State{legacyBot, 4, {}});
    store.Pump(Start);
    persona = voice;
    card = ReadCard(store, 1, legacyBot, persona);
    Check(card.status == CardStatus::NoCard && card.revision == 4 && !card.included && persona == voice &&
              CardToken(card) == "card:none:4:" + std::to_string(legacy.id),
          "a row without a card reads as no card and adds nothing to the voice");
    Check(ProposeCard(store, 1, legacyBot, "claude", 4, card.stamp, "A wary miner.") == ProposalStatus::Queued,
          "a row without a card takes a generated card");
    write = port.Take();
    Check(write.write && write.write->priorRevision == 4 && write.write->next.revision == 5 &&
              write.write->next.accepted->source == Source::Claude,
          "the card is written by compare and swap at the next revision");
    port.Complete(write, LookupStatus::Found, write.write->next, true);
    store.Pump(Start);

    Check(std::string_view(ProposalStatusName(ProposalStatus::Queued)) == "queued" &&
              std::string_view(ProposalStatusName(ProposalStatus::Invalid)) == "invalid" &&
              std::string_view(ProposalStatusName(ProposalStatus::HasCard)) == "has_card" &&
              std::string_view(ProposalStatusName(ProposalStatus::ManualProtected)) == "manual_protected" &&
              std::string_view(ProposalStatusName(ProposalStatus::Busy)) == "busy",
          "refusal reasons have log names");

    std::string const multibyte =
        "Gr\xC3\xBC\xC3\x9F"
        "e aus Azeroth.";
    store.Prepare(1, OtherBot, Start);
    auto load = port.Take();
    port.Complete(load, LookupStatus::Found, State{OtherBot, 1, Card{Source::Manual, multibyte}});
    store.Pump(Start);
    std::string const exact(MaxCardBytes - CardSeparator.size() - multibyte.size(), 'v');
    persona = exact;
    card = ReadCard(store, 1, OtherBot, persona);
    Check(card.status == CardStatus::Accepted && card.included && persona.size() == MaxCardBytes &&
              persona.ends_with(multibyte),
          "voice and card fill the 8192-byte budget exactly");
    persona = exact + "v";
    card = ReadCard(store, 1, OtherBot, persona);
    Check(card.status == CardStatus::Accepted && !card.included && persona == exact + "v",
          "voice keeps priority and a card that does not fit is omitted whole, never cut");
}

class FakePersonaDb final : public PersonaDbIo
{
public:
    void Lookup(uint64_t botGuid, LookupCallback callback) override
    {
        Check(lookups.try_emplace(botGuid, std::move(callback)).second, "one issued query per bot");
    }

    void Commit(std::vector<StoreOperation> const& writes, CommitCallback callback) override
    {
        Check(!commit && !writes.empty(), "one nonempty transaction per proof round");
        batch = writes;
        commit = std::move(callback);
    }

    void Poll() override {}

    void Close() override
    {
        closedWithCommit = static_cast<bool>(commit);
        lookups.clear();
        commit = {};
    }

    void Read(uint64_t botGuid, std::optional<PersonaDbRow> const& row)
    {
        auto issued = lookups.extract(botGuid);
        Check(!issued.empty(), "reply to an issued query");
        issued.mapped()(row);
    }

    void FinishCommit(bool committed)
    {
        Check(static_cast<bool>(commit), "reply to an issued transaction");
        auto done = std::move(commit);
        commit = {};
        done(committed);
    }

    std::unordered_map<uint64_t, LookupCallback> lookups;
    std::vector<StoreOperation> batch;
    CommitCallback commit;
    bool closedWithCommit = false;
};

PersonaDbRow MissingRow(uint64_t botGuid) { return {botGuid, {}, {}, {}, {}}; }

PersonaDbRow NoCardRow(uint64_t botGuid, uint64_t revision) { return {botGuid, botGuid, revision, uint8_t{0}, {}}; }

PersonaDbRow ManualRow(uint64_t botGuid, uint64_t revision, std::string_view body)
{
    return {botGuid, botGuid, revision, uint8_t{1}, body};
}

PersonaDbRow ClaudeRow(uint64_t botGuid, uint64_t revision, std::string_view body)
{
    return {botGuid, botGuid, revision, uint8_t{2}, body};
}

void CheckPreparedDecode()
{
    auto decode = [](std::optional<PersonaDbRow> const& row) { return DecodePersonaLookup(Bot, 7, false, row); };
    auto const missing = decode(MissingRow(Bot));
    Check(missing.lookup == LookupStatus::Missing && !missing.row && missing.botGuid == Bot && missing.id == 7,
          "matching seed with all joined fields NULL confirms absence");
    Check(decode({}).lookup == LookupStatus::Failed, "null result cannot confirm absence");
    Check(decode(MissingRow(OtherBot)).lookup == LookupStatus::Failed,
          "a different full seed GUID cannot prove absence");
    auto row = MissingRow(Bot);
    row.acceptedSource = uint8_t{0};
    Check(decode(row).lookup == LookupStatus::Failed, "partial nullable row is corrupt");

    row = NoCardRow(Bot, 0xF000000000000000);
    auto const found = decode(row);
    Check(found.lookup == LookupStatus::Found && found.row && found.row->botGuid == Bot &&
              found.row->revision == *row.revision && !found.row->accepted,
          "preserve full GUID, unsigned revision and absent accepted slot");
    row.botGuid = OtherBot;
    Check(decode(row).lookup == LookupStatus::Failed, "wrong joined GUID is corrupt");
    row.botGuid = Bot;
    row.revision = 0;
    Check(decode(row).lookup == LookupStatus::Failed, "persisted revision zero cannot become absence");
    row.revision = 1;
    row.acceptedBody = "Manual";
    Check(decode(row).lookup == LookupStatus::Failed, "body with absent source fails closed");
    row.acceptedSource = uint8_t{9};
    Check(decode(row).lookup == LookupStatus::Failed, "unknown provenance fails closed");
    row.acceptedSource = uint8_t{1};
    auto const accepted = decode(row);
    Check(accepted.row && accepted.row->accepted && accepted.row->accepted->source == Source::Manual &&
              accepted.row->accepted->body == "Manual",
          "accepted slot decodes with its provenance");
    row.acceptedSource = uint8_t{3};
    Check(decode(row).row && decode(row).row->accepted->source == Source::Codex, "a generated card decodes as such");
    row.acceptedBody = "";
    Check(decode(row).lookup == LookupStatus::Failed, "nonnull empty card is corrupt");
    row.acceptedBody = std::string_view("x\0y", 3);
    Check(decode(row).lookup == LookupStatus::Failed, "embedded null is preserved for validation");
    std::string const large(MaxCardBytes + 1, 'x');
    row.acceptedBody = large;
    Check(decode(row).lookup == LookupStatus::Failed, "oversize result is rejected");
}

void CheckPreparedWrites()
{
    FakePersonaDb io;
    PersonaDbPort port(io);
    Store store;
    Check(store.SetPort(&port), "attach prepared port");
    store.Configure(true);
    Check(store.Activate(1), "activate prepared store");
    auto const ready = [&](uint64_t guid, PersonaDbRow const& row, StoreTime now)
    {
        store.Prepare(1, guid, now);
        io.Read(guid, row);
        store.Pump(now);
    };
    auto const propose = [&](uint64_t guid, uint64_t revision, std::string const& body)
    {
        Check(store.SubmitUpdate(1, store.Observe(1, guid).stamp,
                                 {UpdateKind::SetManual, guid, revision, Source::Manual, body}),
              "queue revision-bound proposal");
    };
    ready(Bot, MissingRow(Bot), Start);
    ready(OtherBot, ManualRow(OtherBot, 7, "Old"), Start);
    propose(Bot, 0, "New");
    propose(OtherBot, 7, "Update");
    Check(!io.commit && io.lookups.empty(), "staged writes do not issue before world pump");
    port.Pump();
    Check(io.batch.size() == 2 && io.batch[0].write->priorRevision == 0 && io.batch[0].write->next.revision == 1 &&
              io.batch[1].write->priorRevision == 7 && io.batch[1].write->next.revision == 8,
          "batch combines distinct insert and CAS update with exact prior revisions");
    Check(io.lookups.empty(), "readback waits for the transaction callback");
    io.FinishCommit(true);
    Check(io.lookups.size() == 2, "successful commit issues independent fresh lookups");
    io.Read(OtherBot, ManualRow(OtherBot, 7, "Old"));
    store.Pump(Start);
    Check(!store.Observe(1, OtherBot).state && store.Observe(1, Bot).status == StoreStatus::Loading,
          "committed zero-row CAS is unavailable while its peer remains pinned");
    io.Read(Bot, ManualRow(Bot, 1, "New"));
    store.Pump(Start);
    Check(store.Observe(1, Bot).status == StoreStatus::Loaded && !store.Pending(),
          "exact persisted candidate publishes even if concurrent attribution is unknown");

    auto const retry = Start + StoreRetryDelay;
    ready(OtherBot, MissingRow(OtherBot), retry);
    propose(OtherBot, 0, "Conflict");
    propose(Bot, 1, "Rolled back");
    port.Pump();
    io.FinishCommit(false);
    store.Pump(retry);
    Check(io.lookups.empty() && !store.Pending() && !store.Observe(1, Bot).state && !store.Observe(1, OtherBot).state,
          "failed insert can roll back whole batch, requiring ordinary reload of both rows");

    auto const later = retry + StoreRetryDelay;
    ready(Bot, ManualRow(Bot, 1, "New"), later);
    ready(OtherBot, ManualRow(OtherBot, 10, "Actual"), later);
    propose(Bot, 1, "Planned");
    propose(OtherBot, 10, "Planned");
    port.Pump();
    io.FinishCommit(true);
    io.Read(Bot, {});
    io.Read(OtherBot, ManualRow(OtherBot, 12, "Newer"));
    store.Pump(later);
    Check(!store.Observe(1, Bot).state && !store.Observe(1, OtherBot).state,
          "failed readback and newer persisted row cannot publish");
}

void CheckPreparedLifecycle()
{
    FakePersonaDb io;
    PersonaDbPort port(io);
    auto const stage = [&](uint64_t guid, uint64_t id)
    { return port.TrySubmit({guid, id, StoreWrite{0, State{guid, 1, Card{Source::Claude, "Draft"}}}}); };
    Check(stage(Bot, 1) && stage(OtherBot, 2), "stage two distinct bots");
    port.DiscardStaged(Bot);
    Check(!stage(Bot, 3), "discarded write still pins its key through disposal");
    auto cancelled = port.TryPop();
    Check(cancelled && cancelled->id == 1 && cancelled->lookup == LookupStatus::Failed,
          "discarded write returns terminal failure");
    port.Pump();
    Check(io.batch.size() == 1 && io.batch[0].botGuid == OtherBot, "discard before enqueue preserves other staged bot");
    port.DiscardStaged();
    Check(!port.TryPop() && !stage(OtherBot, 4), "retirement cannot unpin an issued transaction");
    Check(stage(Bot, 3), "another operation may stage after disposal");
    port.Close();
    Check(io.closedWithCommit, "shutdown joins issued transaction before terminal disposal");
    Check(port.TryPop().has_value() && port.TryPop().has_value() && !port.TryPop() && !stage(Bot, 5),
          "shutdown disposes each issued or unissued operation once and closes admission");

    FakePersonaDb saturatedIo;
    PersonaDbPort saturated(saturatedIo);
    for (uint64_t i = 1; i <= MaxStoreOperations; ++i)
        Check(saturated.TrySubmit({i, i, {}}), "bounded lookup accepted");
    Check(!saturated.TrySubmit({Bot, Bot, {}}), "port stops at physical capacity");
    saturatedIo.Read(1, MissingRow(1));
    Check(!saturated.TrySubmit({Bot, Bot, {}}), "ready result continues to count against capacity");
    Check(saturated.TryPop().has_value() && saturated.TrySubmit({Bot, Bot, {}}),
          "terminal disposal releases the physical slot");
}

void CheckPreparedCardLifecycle()
{
    FakePersonaDb io;
    PersonaDbPort port(io);
    Store store;
    Check(store.SetPort(&port), "attach prepared port for lifecycle helpers");
    store.Configure(true);
    Check(store.Activate(1), "activate lifecycle store");
    auto const ready = [&](uint64_t guid)
    {
        store.Prepare(1, guid, Start);
        io.Read(guid, MissingRow(guid));
        store.Pump(Start);
    };
    auto const stage = [&](uint64_t guid)
    {
        Check(ProposeCard(store, 1, guid, "codex", 0, store.Observe(1, guid).stamp, "A staged smith.") ==
                  ProposalStatus::Queued,
              "stage a generated card");
    };
    ready(Bot);
    ready(OtherBot);
    stage(Bot);
    stage(OtherBot);
    ForgetPersona(store, &port, Bot);
    store.Pump(Start);
    Check(store.Pending() == 1 && !store.Observe(1, Bot).state &&
              store.Observe(1, OtherBot).status == StoreStatus::Loading,
          "forget discards only that bot's staged write");
    port.Pump();
    Check(io.batch.size() == 1 && io.batch[0].botGuid == OtherBot, "a forgotten write never reaches SQL");

    ready(Bot);
    stage(Bot);
    RetirePersonas(store, &port, 1);
    store.Pump(Start);
    Check(store.Pending() == 1 && !store.Observe(1, Bot).state && !store.Observe(1, OtherBot).state,
          "retirement discards staged writes, invalidates views and keeps the issued write pinned");
    port.Pump();
    Check(io.batch.size() == 1 && io.batch[0].botGuid == OtherBot, "a retired staged write never reaches SQL");
    io.FinishCommit(true);
    io.Read(OtherBot, ClaudeRow(OtherBot, 1, "Committed late"));
    store.Pump(Start);
    // J5e: retirement no longer empties the rows, but a write hides its row until a matching readback. The discarded
    // write and the late readback that differs from the staged candidate publish nothing; both rows back off and
    // reload from SQL later.
    Check(store.Pending() == 0 && store.Activate(2), "the late commit drains and a new generation binds");
    Check(!store.Observe(2, Bot).state, "after retirement the discarded write publishes nothing");
    Check(!store.Observe(2, OtherBot).state, "after retirement a late mismatched commit publishes nothing");
    ForgetPersona(store, nullptr, Bot);
    RetirePersonas(store, nullptr, 2);
    Check(store.Pending() == 0 && !store.Activate(2), "helpers work without a session");
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc == 1)
    {
        CheckLoads();
        CheckWrites();
        CheckLifecycle();
        CheckReloadKeepsRows();
        CheckCapacity();
        CheckOffers();
        CheckCards();
    }
    else if (argc != 2 || std::string_view(argv[1]) != "--prepared-db")
        return 2;
    CheckPreparedDecode();
    CheckPreparedWrites();
    CheckPreparedLifecycle();
    CheckPreparedCardLifecycle();
    std::cout << "persona-store selftest: " << checks << " checks passed\n";
}
