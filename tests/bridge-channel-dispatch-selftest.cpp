// Actual production mailbox, world policy and channel policy; controlled world observation, no server/native I/O.
#include <atomic>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

#include "bridge-ability-double.h"
#include "mod-ollama-chat_bridge_world.h"

namespace
{
using namespace OllamaBridge;
using Json = nlohmann::json;
unsigned checks = 0;
void Check(bool value, char const* label)
{
    ++checks;
    if (!value)
        throw std::runtime_error(label);
}

struct Fixture
{
    Clock now{100000, 200000};
    Settings settings{"fixture", "codex", "http://127.0.0.1:47800", "synthetic-local-channel-token"};
    WorldPolicy world;
    DispatchGuard guard;
    bool visible = true;
    unsigned observations = 0;
    unsigned failAt = 0;
    WorldChannel channel{world,
                         [this](WorldTicket const&, DispatchGuard& current)
                         {
                             ++observations;
                             current = guard;
                             return visible && (!failAt || observations < failAt);
                         },
                         [this] { return now; }};
    WorldTicket ticket;

    explicit Fixture(bool enable = true)
    {
        channel.Configure(enable);
        world.Configured(true, settings, "epoch-1");
        Check(!channel.Activate(), "selection is not a native/producer grant");
        Check(!world.ObserveDrain(false), "held inference blocks activation");
        Check(world.ObserveDrain(channel.Drained()), "real selection");
        Check(channel.Activate() == enable, "channel requires explicit opt in");
        WorldScope scope{1, "fixture", "codex", "epoch-1", 11, 22, "context", "party", "visible", "c1", "m1"};
        Check(world.ObserveScope(scope, now), "trusted observed scope");
        Context input;
        input.speakerGuid = 22;
        input.channel = "party";
        input.text = "Ready?";
        ticket = world.Reserve(scope, input, now, 30000);
        Check(ticket.generation != 0, "actual world reservation");
        guard.scope = scope;
        guard.source = DispatchSource::Party;
        guard.speakerGuid = 22;
        guard.groupGuid = 33;
        guard.mapId = 1;
        guard.phaseMask = 1;
        guard.audience = {11, 22};
        guard.humanSpeaker = guard.authority = guard.perceptible = guard.audienceOnline = guard.nonRaid = true;
        Check(channel.Register(ticket, guard) == enable, "real registered generation and exact guard");
        Check(world.MarkQueued(ticket, now), "generation may remain blocked independently");
    }

    Json Wire(std::string type, std::string id) const
    {
        return {
            {"version", 1},           {"instance", "fixture"},      {"provider", "codex"},
            {"epoch", "epoch-1"},     {"requestId", std::move(id)}, {"deadlineUnixMs", ticket.request.deadline.unixMs},
            {"type", std::move(type)}};
    }
    std::string Query(std::string id) const
    {
        auto j = Wire("query", std::move(id));
        j["controllerGuid"] = "22";
        j["botGuid"] = "11";
        j["query"] = "observable_group";
        return j.dump();
    }
    std::string Command(std::string id) const
    {
        auto j = Wire("command", ticket.request.requestId);
        j["commandId"] = std::move(id);
        j["issuerGuid"] = "22";
        j["botGuid"] = "11";
        j["steps"] = Json::array({{{"act", "follow"}, {"params", Json::object()}}});
        return j.dump();
    }
    uint64_t Receive(std::string const& body)
    {
        uint64_t receipt = 0;
        Check(channel.Receive(ticket.request.requestId, body, receipt) == ChannelError::None,
              "authenticated producer admission");
        return receipt;
    }
    std::unique_ptr<ChannelPacket> Take(uint64_t receipt)
    {
        std::unique_ptr<ChannelPacket> packet;
        Check(channel.Take(receipt, packet) == ChannelError::None && packet, "authenticated completion operation");
        return packet;
    }
};

void PhysicalAndIndependent()
{
    Fixture f;
    auto heldInference = DispatchSlot::Acquire();
    std::vector<uint64_t> ids;
    // A distinct producer thread may receive while generation is held; no world observation occurs there.
    std::atomic<bool> accepted{true};
    unsigned const before = f.observations;
    std::thread producer(
        [&]
        {
            for (unsigned i = 0; i < WorldChannel::MaxPackets; ++i)
            {
                uint64_t id = 0;
                if (f.channel.Receive(f.ticket.request.requestId, f.Query("q" + std::to_string(i)), id) !=
                    ChannelError::None)
                    accepted = false;
                ids.push_back(id);
            }
        });
    producer.join();
    Check(accepted && f.observations == before, "producer neither blocks on inference nor reads world");
    Check(f.channel.Used() == 64, "physical receive capacity");
    uint64_t overflow = 7;
    Check(f.channel.Receive("route", "{", overflow) == ChannelError::Capacity, "physical bound before body parse");
    Check(f.channel.Update() == WorldChannel::TickBudget, "exact bounded world dequeue");
    Check(f.channel.Used() == 64 && heldInference, "queued completions retain slots while inference held");
    std::unique_ptr<ChannelPacket> unauthorized;
    auto reply = f.Take(ids.front());
    Check(reply->error == ChannelError::None && Json::parse(reply->body)["code"] == "unavailable",
          "missing filtered adapter fails explicitly closed");
    Check(f.channel.Used() == 64, "taking output does not release physical slot");
    f.channel.Configure(false);
    f.world.Configured(true, f.settings, "epoch-2");
    Check(f.channel.Used() == 1 && !f.channel.Drained(), "retirement disposes queues but retains actual held write");
    Check(!f.world.ObserveDrain(f.channel.Drained()), "held retired response blocks new epoch");
    reply.reset();
    Check(f.channel.Drained(), "actual write disposal releases last slot");
    heldInference = {};
    Check(f.world.ObserveDrain(f.channel.Drained()), "drained selection");
    f.channel.Configure(true);
    Check(f.channel.Activate(), "reconnect binds new selected epoch");
    Check(f.channel.Take(ids.front(), unauthorized) == ChannelError::Unknown,
          "old completion cannot revive across reconnect");
}

void GuardsAndTerminal()
{
    Fixture f;
    auto id = f.Receive(f.Command("effect"));
    Check(f.channel.Update() == 1, "command dequeued");
    auto reply = f.Take(id);
    Check(Json::parse(reply->body)["outcome"] == "rejected", "staged command grants no execution");
    reply.reset();
    id = f.Receive(f.Command("effect"));
    f.channel.Update();
    reply = f.Take(id);
    Check(reply->error == ChannelError::Duplicate && reply->body.empty(), "replay fixed error without payload");
    reply.reset();
    id = f.Receive("{SECRET-MALFORMED");
    f.channel.Update();
    reply = f.Take(id);
    Check(reply->error == ChannelError::Invalid && reply->body.empty(), "malformed no input echo");
    reply.reset();
    id = f.Receive(f.Query("lost-world"));
    f.visible = false;
    f.channel.Update();
    Check(f.channel.Drained(), "fresh missing-unit guard drops before admission");
    f.visible = true;
    id = f.Receive(f.Query("lost-publication"));
    f.failAt = f.observations + 3;
    f.channel.Update();
    Check(f.channel.Drained(), "fresh guard rechecked immediately before publication");
    f.failAt = 0;
    id = f.Receive(f.Query("closed"));
    f.channel.Close(f.ticket);
    f.channel.Update();
    Check(f.channel.Drained(), "parent terminal closes route before queued query");
}

void RouteCorrelation()
{
    for (std::string const type : {"generation.result", "failure", "command"})
    {
        Fixture f;
        Context input;
        input.speakerGuid = 22;
        input.channel = "party";
        input.text = "Ready?";
        auto const second = f.world.Reserve(f.ticket.scope, input, f.now, 30000);
        Check(second.generation && f.channel.Register(second, f.guard), "second same-scope route registered");
        auto message = f.Wire(type, second.request.requestId);
        if (type == "failure")
            message["code"] = "unavailable";
        if (type == "generation.result")
            message["text"] = "Hello.";
        if (type == "command")
        {
            message = Json::parse(f.Command("cross-route-effect"));
            message["requestId"] = second.request.requestId;
        }
        auto const wrongReceipt = f.Receive(message.dump());
        Check(f.channel.Update() == 1, "wrong-route message processed");
        auto wrong = f.Take(wrongReceipt);
        Check(wrong->error == ChannelError::Invalid && wrong->body.empty(), "wrong route rejected without echo");
        wrong.reset();
        Check(f.world.Current(second, f.now) && f.channel.Drained(), "rejected input preserves other world authority");

        // Queries deliberately have a new id; route correlation must not couple it to the generation id.
        auto const queryReceipt = f.Receive(f.Query("independent-query"));
        f.channel.Update();
        auto query = f.Take(queryReceipt);
        Check(query->error == ChannelError::None && Json::parse(query->body)["code"] == "unavailable",
              "original route remains usable with independent query id");
        query.reset();

        auto const send = [&]
        {
            uint64_t receipt = 0;
            Check(f.channel.Receive(second.request.requestId, message.dump(), receipt) == ChannelError::None,
                  "message admitted through its own route");
            f.channel.Update();
            return receipt;
        };
        auto right = f.Take(send());
        Check(type == "command"
                  ? right->error == ChannelError::None && Json::parse(right->body)["outcome"] == "rejected"
                  : right->error == ChannelError::Invalid && right->body.empty(),
              "a command stays refused; LCB6a: a generation result or failure is no channel type");
        right.reset();
        // LC.35: a replay on the open binding: a command id stays reserved; the refused types stay refused.
        auto again = f.Take(send());
        Check(again->error == (type == "command" ? ChannelError::Duplicate : ChannelError::Invalid),
              "a replay on the same route is refused");
        again.reset();
        f.channel.Close(second);
        f.world.Cancel(second);
        Check(!f.world.Current(second, f.now) && f.channel.Drained() && f.world.Pending() == 1,
              "closing releases only that route");
        auto const late = send();
        std::unique_ptr<ChannelPacket> gone;
        Check(f.channel.Take(late, gone) == ChannelError::Unknown && !gone && f.channel.Drained(),
              "a closed route answers nothing");
        Check(f.world.Current(f.ticket, f.now), "other generation authority remains current");
    }
}

// LC.35: a bound route registers once, and its command id stays refused while the route lives. LCB6a: an inbound
// failure is no channel type; refusing it closes nothing.
void BoundRouteStaysClosed()
{
    Fixture f;
    auto id = f.Receive(f.Command("once"));
    f.channel.Update();
    auto reply = f.Take(id);
    Check(reply->error == ChannelError::None && Json::parse(reply->body)["commandId"] == "once", "command staged once");
    reply.reset();
    auto failure = f.Wire("failure", f.ticket.request.requestId);
    failure["code"] = "cancelled";
    id = f.Receive(failure.dump());
    f.channel.Update();
    reply = f.Take(id);
    Check(reply->error == ChannelError::Invalid && reply->body.empty(),
          "an inbound failure is refused and closes nothing");
    reply.reset();
    Check(!f.channel.Register(f.ticket, f.guard), "a bound route registers once");
    id = f.Receive(f.Command("once"));
    f.channel.Update();
    reply = f.Take(id);
    Check(reply->error == ChannelError::Duplicate, "a replayed command id stays refused on the open route");
}

// LC.35: a long-running server keeps answering: 5,000 generation routes with 3 queries each in one epoch, each closed
// route leaving the world policy and the replay ledger.
void LongRunRoutes()
{
    Fixture f;
    Context input;
    input.speakerGuid = 22;
    input.channel = "party";
    input.text = "Ready?";
    for (unsigned i = 0; i < 5000; ++i)
    {
        auto const ticket = f.world.Reserve(f.ticket.scope, input, f.now, 30000);
        Check(ticket.generation && f.channel.Register(ticket, f.guard), "5000: every route registered");
        for (unsigned k = 0; k < 3; ++k)
        {
            uint64_t receipt = 0;
            Check(
                f.channel.Receive(ticket.request.requestId, f.Query("q-" + std::to_string(i) + "-" + std::to_string(k)),
                                  receipt) == ChannelError::None,
                "5000: every query received");
            f.channel.Update();
            Check(f.Take(receipt)->error == ChannelError::None, "5000: every query answered");
        }
        f.channel.Close(ticket);
        f.world.Cancel(ticket);
    }
    Check(f.world.Pending() == 1 && f.channel.Drained(), "5000: only the fixture's own route stays");
}

void ExpiryReloadAndDefaults()
{
    Fixture off(false);
    uint64_t id = 0;
    Check(off.channel.Receive(off.ticket.request.requestId, "{", id) == ChannelError::Inactive &&
              off.channel.Update() == 0,
          "off has no reverse admission or tick work");
    Fixture f;
    id = f.Receive(f.Query("expired"));
    f.now.steadyMs += WorldLivenessMs;
    f.now.unixMs -= 1000;
    Check(f.channel.Used() == 1, "logical expiry and rollback do not release physical queue");
    f.channel.Update();
    Check(f.channel.Drained(), "exact monotonic expiry disposed at bounded dequeue");
    Fixture completed;
    id = completed.Receive(completed.Query("pending-output"));
    completed.channel.Update();
    completed.now.steadyMs += WorldLivenessMs;
    Check(completed.channel.Used() == 1, "completed expiry does not pretend drain");
    std::unique_ptr<ChannelPacket> packet;
    Check(completed.channel.Take(id, packet) == ChannelError::Stale && completed.channel.Drained(),
          "expired poll actually disposes completion");
    Fixture reload;
    id = reload.Receive(reload.Query("retired"));
    reload.channel.Configure(true);
    reload.world.Configured(true, reload.settings, "epoch-2");
    Check(reload.channel.Receive(reload.ticket.request.requestId, "{", id) == ChannelError::Inactive,
          "reload revokes the old receiver immediately");
    Check(!reload.channel.Activate(), "reload never self acknowledges selection");
}

void HeartbeatReplayLedger()
{
    ChannelPolicy policy;
    Clock const now{100000, 200000};
    WorldScope scope{1, "fixture", "codex", "epoch", 11, 22, "context", "party", "visible", "c1", "m1"};
    Check(policy.Bind({"fixture", "codex", "", "epoch"}), "ledger bind");
    Check(policy.ObserveScope(scope, now), "ledger scope");
    std::string body;
    for (std::size_t i = 0; i < 5000; ++i)
    {
        auto const id = "r" + std::to_string(i);
        Check(policy.OpenGeneration(scope, id, {101000, 201000}, now) == ChannelError::None, "ledger reserve");
        if (i + 1 != 5000)
            Check(policy.FinishFailure({"fixture", "codex", "epoch", id, 101000}, ChannelFailure::Cancelled, scope, now,
                                       body) == ChannelError::None,
                  "ledger closes the route");
    }
    Check(policy.RequestReservations() == 1 && policy.Pending() == 1, "closed routes leave; the open one stays");
    Check(policy.ObserveScope(scope, {101000, 200100}), "heartbeat at wall deadline with live scope");
    Check(policy.Pending() == 0, "heartbeat logical expiry without request-table sweep");
    Check(!policy.Current({"fixture", "codex", "epoch", "r4999", 101000}, scope, now),
          "wall rollback cannot revive request after trusted heartbeat expiry");
    Check(policy.ObserveScope(scope, {106000, 206000}), "scope gap refresh");
    Check(!policy.Current({"fixture", "codex", "epoch", "r4999", 101000}, scope, now),
          "scope lapse and clock rollback never revive retained work");
}
}  // namespace

int main()
{
    if (!InstallTestAbilities())  // LC3: the acts and topics are the installed index's.
    {
        std::cerr << "FAIL: the ability double was refused\n";
        return 1;
    }
    try
    {
        PhysicalAndIndependent();
        GuardsAndTerminal();
        RouteCorrelation();
        BoundRouteStaysClosed();
        LongRunRoutes();
        ExpiryReloadAndDefaults();
        HeartbeatReplayLedger();
        std::cout << "PASS " << checks << " production channel dispatch checks\n";
    }
    catch (std::exception const& e)
    {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
