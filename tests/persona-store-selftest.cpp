#include <cstdlib>
#include <deque>
#include <iostream>
#include <utility>

#include "mod-ollama-chat_persona_store.h"

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
    port.Complete(second, LookupStatus::Found, State{OtherBot, 1, {}, "wrong guid", {}});
    store.Pump(now);
    Check(store.Observe(1, Bot).status == StoreStatus::Unavailable, "same low counter cannot substitute full GUID");

    now += StoreRetryDelay;
    store.Prepare(1, Bot, now);
    port.Complete(port.Take(), LookupStatus::Found, State{Bot, 0, {}, {}, {}});
    store.Pump(now);
    Check(!store.Observe(1, Bot).state, "zero revision row cannot masquerade as confirmed missing");
    now += StoreRetryDelay;
    store.Prepare(1, Bot, now);
    port.Complete(port.Take(), LookupStatus::Found, State{Bot, 1, Card{Source::Codex, ""}, {}, {}});
    store.Pump(now);
    Check(!store.Observe(1, Bot).state, "corrupt accepted slot is unavailable");
    now += StoreRetryDelay;
    store.Prepare(1, Bot, now);
    port.Complete(port.Take(), LookupStatus::Missing, State{Bot, 1, {}, "unexpected row", {}});
    store.Pump(now);
    Check(!store.Observe(1, Bot).state, "missing outcome cannot carry a row");

    now += StoreRetryDelay;
    LoadMissing(store, port, 1, Bot, now);
    auto missing = store.Observe(1, Bot);
    Check(missing.status == StoreStatus::Missing && missing.state && missing.stamp && missing.state->botGuid == Bot &&
              !missing.state->revision && !missing.state->accepted && missing.state->claudeAlternative.empty() &&
              missing.state->codexAlternative.empty(),
          "confirmed missing carries exact key and empty slots");
    port.Complete(second, LookupStatus::Missing);
    store.Pump(now);
    Check(store.Pending() == 0 && store.Observe(1, Bot).stamp == missing.stamp, "duplicate does not replace snapshot");
    store.Prepare(1, OtherBot, now);
    port.Complete(port.Take(), LookupStatus::Found, State{OtherBot, 8, {}, {}, "draft only"});
    store.Pump(now);
    auto loaded = store.Observe(1, OtherBot);
    Check(loaded.status == StoreStatus::Loaded && loaded.state && !loaded.state->accepted &&
              loaded.state->claudeAlternative.empty() && loaded.state->codexAlternative == "draft only",
          "valid draft-only row preserves nullable slots");
}

void CheckWrites()
{
    FakePort port;
    Store store;
    Enable(store, port);
    LoadMissing(store, port, 1, Bot, Start);
    uint64_t const firstStamp = store.Observe(1, Bot).stamp;
    Update proposal{UpdateKind::ProposeAlternative, Bot, 0, Source::Claude, "first draft"};
    Check(!store.SubmitUpdate(1, firstStamp, false, proposal), "unacquainted update refused");
    Check(!store.SubmitUpdate(2, firstStamp, true, proposal), "wrong generation refused");
    Check(!store.SubmitUpdate(1, firstStamp + 1, true, proposal), "wrong snapshot refused");
    proposal.expectedRevision = 1;
    Check(!store.SubmitUpdate(1, firstStamp, true, proposal), "wrong expected revision refused");
    proposal.expectedRevision = 0;
    Check(port.jobs.empty(), "refused writes queue no work");

    port.accepting = false;
    Check(!store.SubmitUpdate(1, firstStamp, true, proposal) && store.Observe(1, Bot).stamp == firstStamp,
          "port refusal preserves confirmed state");
    port.accepting = true;
    Check(store.SubmitUpdate(1, firstStamp, true, proposal), "absent insert queued");
    Check(!store.SubmitUpdate(1, firstStamp, true, proposal), "pending insert blocks second write");
    auto insert = port.Take();
    Check(insert.write && insert.write->priorRevision == 0 && insert.write->next.revision == 1,
          "first write requests absent insert");
    Check(store.Observe(1, Bot).status == StoreStatus::Loading && !store.Observe(1, Bot).state,
          "proposed row is hidden until confirmation");
    State conflicting{Bot, 1, Card{Source::Manual, "operator card"}, {}, "competing draft"};
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
    Check(store.SubmitUpdate(1, loaded.stamp, true, proposal), "existing-row CAS queued");
    auto cas = port.Take();
    Check(cas.write && cas.write->priorRevision == 1 && cas.write->next.revision == 2,
          "CAS uses exact prior revision and next row");
    port.Complete(cas, LookupStatus::Found, conflicting, true);
    store.Pump(now);
    Check(!store.Observe(1, Bot).state, "committed zero-row CAS with old readback stays unconfirmed");

    State newer{Bot, 3, Card{Source::Manual, "operator card"}, "newer draft", "competing draft"};
    now += StoreRetryDelay;
    store.Prepare(1, Bot, now);
    port.Complete(port.Take(), LookupStatus::Found, newer);
    store.Pump(now);
    loaded = store.Observe(1, Bot);
    proposal.expectedRevision = 3;
    Check(store.SubmitUpdate(1, loaded.stamp, true, proposal), "write after reload uses newest persisted revision");
    auto failedReadback = port.Take();
    port.Complete(failedReadback, LookupStatus::Failed, {}, true);
    store.Pump(now);
    Check(!store.Observe(1, Bot).state, "failed readback cannot publish even after successful commit");

    now += StoreRetryDelay;
    store.Prepare(1, Bot, now);
    port.Complete(port.Take(), LookupStatus::Found, newer);
    store.Pump(now);
    Check(store.SubmitUpdate(1, store.Observe(1, Bot).stamp, true, proposal), "candidate can be tried after reload");
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
    Check(store.SubmitUpdate(1, store.Observe(1, Bot).stamp, true, proposal), "newer-row race can queue");
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
    Check(store.SubmitUpdate(1, store.Observe(1, Bot).stamp, true, proposal), "exact-row race can queue");
    auto exact = port.Take();
    port.Complete(exact, LookupStatus::Found, exact.write->next, true);
    store.Pump(now);
    auto confirmed = store.Observe(1, Bot);
    Check(confirmed.state && confirmed.state->revision == 4 && confirmed.state->claudeAlternative == "first draft" &&
              confirmed.state->codexAlternative == "competing draft" &&
              confirmed.state->accepted->source == Source::Manual,
          "exact concurrent candidate proves persisted state and preserves independent slots");
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
    Update proposal{UpdateKind::ProposeAlternative, Bot, 0, Source::Codex, "retiring draft"};
    Check(oldStamp != sameRevisionStamp && !store.SubmitUpdate(1, oldStamp, true, proposal),
          "same-revision reload after logout invalidates old generation opportunity");
    Check(store.SubmitUpdate(1, sameRevisionStamp, true, proposal), "write can use freshly loaded snapshot");
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
    Check(!store.Observe(2, Bot).state && !store.Activate(2), "current retirement invalidates loaded views");
    Check(store.Activate(3), "new generation after retirement");
    store.Prepare(3, Bot, Start);
    auto retiredLoad = port.Take();
    store.Configure(false);
    port.Complete(retiredLoad, LookupStatus::Missing);
    store.Pump(Start);
    Check(store.Pending() == 0 && store.SetPort(nullptr), "disabled pump drains and permits port detachment");
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
    uint64_t const evictedStamp = store.Observe(1, 2).stamp;
    store.Prepare(1, 1, Start);
    store.Prepare(1, Bot, Start);
    auto pinned = port.Take();
    Check(store.Observe(1, 1).state && !store.Observe(1, 2).state, "capacity evicts the least recently used idle row");
    LoadMissing(store, port, 1, 2, Start);
    Check(store.Observe(1, 2).stamp != evictedStamp && store.Observe(1, Bot).status == StoreStatus::Loading,
          "eviction reload changes the stamp and keeps pending keys pinned");
    port.Complete(pinned, LookupStatus::Missing);
    store.Pump(Start);

    store.Configure(true);
    Check(store.Activate(2), "same-enabled reload binds a new generation");
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
}  // namespace

int main()
{
    CheckLoads();
    CheckWrites();
    CheckLifecycle();
    CheckCapacity();
    std::cout << "persona-store selftest: " << checks << " checks passed\n";
}
