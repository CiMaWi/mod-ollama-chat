// Offline production value-policy checks. No server, game objects, native observer or delivery proof.
#include <iostream>
#include <stdexcept>

#include "mod-ollama-chat_bridge_world.h"

namespace
{
using namespace OllamaBridge;
unsigned checks = 0;
Clock const now{100000, 200000};

void Check(bool condition, char const* label)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(label);
}

Settings Config() { return {"fixture", "codex", "http://127.0.0.1:47800", "synthetic-game-link-bearer-only"}; }

WorldScope Scope(WorldPolicy const& policy)
{
    auto const selected = policy.Selected();
    return {1,       selected.instance, selected.provider, selected.epoch, 11,         22, "conversation",
            "party", "visible-party",   "context-1",       "membership-1", "persona-1"};
}

Context Input()
{
    Context input;
    input.speakerGuid = 22;
    input.channel = "party";
    input.text = "Shall we continue?";
    return input;
}

void Select(WorldPolicy& policy, std::string epoch)
{
    policy.Configured(true, Config(), std::move(epoch));
    Check(policy.Selected().epoch.empty(), "publication does not select before drain");
    Check(policy.ObserveDrain(true, RetirementProof::GenerationOnlyNoCaptures), "select after drain");
}

void Lifecycle()
{
    WorldPolicy policy;
    policy.Configured(true, Config(), "world-epoch-1");
    Check(!policy.ObserveDrain(false, RetirementProof::GenerationOnlyNoCaptures), "old worker still draining");
    Check(!policy.ObserveDrain(true, RetirementProof::Unknown), "unknown restoration fails closed");
    Check(policy.Selected().epoch.empty(), "no provisional activation");
    Check(policy.ObserveDrain(true, RetirementProof::GenerationOnlyNoCaptures), "drain selects module circuit");
    auto scope = Scope(policy);
    Check(policy.ObserveScope(scope, now), "complete trusted scope accepted");
    auto old = policy.Reserve(scope, Input(), now, 1000);
    Check(policy.MarkQueued(old, now), "queued old work");
    auto changed = Config();
    changed.token += "-rotated";
    uint64_t const generation = policy.Generation();
    policy.Configured(true, changed, "world-epoch-2");
    Check(policy.Generation() == generation + 1, "token-only reload advances generation");
    Check(policy.Pending() == 0 && policy.Selected().epoch.empty(), "retire clears reservations and selection");
    Check(!policy.Complete(old, WorldOutcome::Success, now), "late outcome cannot cross reload");
    Check(!policy.ObserveDrain(false, RetirementProof::GenerationOnlyNoCaptures), "reload waits asynchronously");
    Check(policy.ObserveDrain(true, RetirementProof::GenerationOnlyNoCaptures), "real drain permits new selection");
    Check(policy.Snapshot().token == changed.token, "owned new token snapshot");
    Check(!policy.ObserveScope(scope, now), "old epoch scope rejected");
    scope = Scope(policy);
    Check(policy.ObserveScope(scope, now), "new epoch scope");
    auto fresh = policy.Reserve(scope, Input(), now, 1000);
    Check(fresh.request.requestId != old.request.requestId, "ids differ across reload");
    policy.Configured(false, changed, "unused");
    Check(policy.Selected().epoch.empty() && !policy.Current(fresh, now), "disable invalidates without update");
    Check(!policy.ObserveDrain(true, RetirementProof::GenerationOnlyNoCaptures), "disabled stays unselected");
    Select(policy, "world-epoch-restart");
    Check(policy.ObserveScope(Scope(policy), now), "restart fresh scope");
    auto restarted = policy.Reserve(Scope(policy), Input(), now, 1000);
    Check(restarted.request.requestId != old.request.requestId, "restart fresh id");
    policy.Shutdown();
    Check(!policy.Current(restarted, now) && policy.Selected().epoch.empty(), "shutdown immediate invalidation");
    auto invalid = Config();
    invalid.token.clear();
    policy.Configured(true, invalid, "world-epoch-invalid");
    Check(!policy.ObserveDrain(true, RetirementProof::GenerationOnlyNoCaptures), "invalid config fails closed");
}

void OutcomesAndScopes()
{
    WorldPolicy policy;
    Select(policy, "world-epoch-scope");
    auto scope = Scope(policy);
    auto invalid = scope;
    invalid.personaRevision.clear();
    Check(!policy.ObserveScope(invalid, now), "partial scope rejected");
    Check(policy.ObserveScope(scope, now), "scope observed");
    auto first = policy.Reserve(scope, Input(), now, 1000);
    Check(first.generation != 0, "reserve actual production builder-valid request");
    Check(!policy.Complete(first, WorldOutcome::Success, now), "success requires queue admission");
    Check(policy.MarkQueued(first, now) && !policy.MarkQueued(first, now), "one queue admission");
    auto tampered = first;
    ++tampered.request.deadline.unixMs;
    Check(!policy.Current(tampered, now), "completion cannot extend captured deadline");
    Check(policy.Complete(first, WorldOutcome::Success, now), "one completion");
    Check(!policy.Complete(first, WorldOutcome::Unavailable, now), "duplicate cannot stage fallback");
    Check(policy.Consume(first, now) && !policy.Consume(first, now), "one terminal effect reservation");
    auto full = policy.Reserve(scope, Input(), now, 1000);
    Check(policy.Complete(full, WorldOutcome::QueueFull, now), "queue-full terminal without worker");
    Check(policy.Consume(full, now), "queue-full completion consumed");
    auto cancelled = policy.Reserve(scope, Input(), now, 1000);
    policy.Cancel(cancelled);
    Check(!policy.Current(cancelled, now), "explicit cancellation");
    auto revision = policy.Reserve(scope, Input(), now, 1000);
    scope.membershipRevision = "membership-2";
    Check(policy.ObserveScope(scope, now) && !policy.Current(revision, now), "group revision revokes old reservation");
    auto live = policy.Reserve(scope, Input(), now, 10000);
    Clock const boundary{now.unixMs + WorldLivenessMs, now.steadyMs + WorldLivenessMs};
    Check(!policy.Current(live, boundary), "exact liveness boundary fails closed");
    Check(policy.ObserveScope(scope, boundary), "heartbeat after gap starts fresh observation");
    Check(!policy.Current(live, boundary), "heartbeat cannot resurrect old pending work");
    policy.Expire(boundary);
    Check(policy.Pending() == 0, "expiry releases bounded pending storage");
    Check(policy.ObserveScope(scope, now), "scope renewed");
    auto deadline = policy.Reserve(scope, Input(), now, 1);
    Check(!policy.Current(deadline, {now.unixMs, now.steadyMs + 1}), "monotonic expiry despite UTC rollback");
    policy.InvalidateScope(scope.botGuid, scope.contextId);
    Check(!policy.Current(deadline, now), "explicit scope invalidation");
    Check(policy.ObserveScope(scope, now), "new scope after invalidation");
    auto oversized = Input();
    oversized.text.assign(MaxTextBytes + 1, 'x');
    Check(policy.Reserve(scope, oversized, now, 1000).generation == 0, "no silently clipped context");
}

void Capacity()
{
    WorldPolicy policy;
    Select(policy, "world-epoch-capacity");
    auto const scope = Scope(policy);
    Check(policy.ObserveScope(scope, now), "capacity scope");
    std::vector<WorldTicket> pending;
    for (std::size_t i = 0; i < MaxWorldPending; ++i)
        pending.push_back(policy.Reserve(scope, Input(), now, 1000));
    Check(pending.back().generation != 0 && policy.Pending() == MaxWorldPending, "pending capacity reached");
    Check(policy.Reserve(scope, Input(), now, 1000).generation == 0, "pending capacity rejects without eviction");
    for (auto const& ticket : pending)
        policy.Cancel(ticket);
    WorldTicket last;
    for (uint64_t i = MaxWorldPending; i < MaxEpochRequests; ++i)
    {
        last = policy.Reserve(scope, Input(), now, 1000);
        if (!last.generation)
            throw std::runtime_error("premature id capacity");
        policy.Cancel(last);
    }
    Check(last.request.requestId != pending.front().request.requestId, "cancelled ids never reused");
    Check(policy.Pending() == 0 && policy.Reserve(scope, Input(), now, 1000).generation == 0,
          "id capacity does not recycle terminal reservations");
    Select(policy, "world-epoch-capacity-2");
    Check(policy.ObserveScope(Scope(policy), now), "new epoch capacity scope");
    auto const next = policy.Reserve(Scope(policy), Input(), now, 1000);
    Check(next.generation && next.request.requestId != last.request.requestId, "fresh epoch restores capacity safely");
    for (std::size_t i = 1; i < MaxWorldScopes; ++i)
    {
        auto another = Scope(policy);
        another.contextId = "conversation-" + std::to_string(i);
        if (!policy.ObserveScope(another, now))
            throw std::runtime_error("premature scope capacity");
    }
    auto excess = Scope(policy);
    excess.contextId = "excess";
    Check(!policy.ObserveScope(excess, now), "scope capacity rejects without eviction");
}
}  // namespace

int main()
{
    try
    {
        Lifecycle();
        OutcomesAndScopes();
        Capacity();
        std::cout << "PASS " << checks << " production world-policy checks\n";
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
