// Actual production mailbox, world policy and channel policy; controlled world observation, no server/native I/O.
#include <atomic>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

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
    Settings settings{"fixture", "codex", "http://127.0.0.1:47800", "synthetic-local-channel-token", false, "", ""};
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
        Check(!world.ObserveDrain(false, RetirementProof::NoLiveCaptures), "held inference blocks activation");
        Check(!world.ObserveDrain(true, RetirementProof::Unknown), "unknown restoration blocks activation");
        Check(world.ObserveDrain(channel.Drained(), RetirementProof::NoLiveCaptures), "real selection");
        Check(channel.Activate() == enable, "channel requires explicit opt in");
        WorldScope scope{1, "fixture", "codex", "epoch-1", 11, 22, "context", "party", "visible", "c1", "m1", "p1"};
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
        j["intent"] = "stay";
        j["params"] = Json::object();
        return j.dump();
    }
    uint64_t Receive(std::string const& body)
    {
        uint64_t receipt = 0;
        Check(channel.Receive(settings.token, ticket.request.requestId, body, receipt) == ChannelError::None,
              "authenticated producer admission");
        return receipt;
    }
    std::unique_ptr<ChannelPacket> Take(uint64_t receipt)
    {
        std::unique_ptr<ChannelPacket> packet;
        Check(channel.Take(settings.token, receipt, packet) == ChannelError::None && packet,
              "authenticated completion operation");
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
                if (f.channel.Receive(f.settings.token, f.ticket.request.requestId, f.Query("q" + std::to_string(i)),
                                      id) != ChannelError::None)
                    accepted = false;
                ids.push_back(id);
            }
        });
    producer.join();
    Check(accepted && f.observations == before, "producer neither blocks on inference nor reads world");
    Check(f.channel.Used() == 64, "physical receive capacity");
    uint64_t overflow = 7;
    Check(
        f.channel.Receive("wrong", "", std::string(MaxRequestBytes + 1, 'x'), overflow) == ChannelError::Unauthorized &&
            !overflow,
        "auth before capacity/size/parse");
    Check(f.channel.Receive(f.settings.token, "route", "{", overflow) == ChannelError::Capacity,
          "physical bound before body parse");
    Check(f.channel.Update() == WorldChannel::TickBudget, "exact bounded world dequeue");
    Check(f.channel.Used() == 64 && heldInference, "queued completions retain slots while inference held");
    std::unique_ptr<ChannelPacket> unauthorized;
    Check(f.channel.Take("wrong", ids.front(), unauthorized) == ChannelError::Unauthorized,
          "poll operation authenticates independently");
    auto reply = f.Take(ids.front());
    Check(reply->error == ChannelError::None && Json::parse(reply->body)["code"] == "unavailable",
          "missing filtered adapter fails explicitly closed");
    Check(f.channel.Used() == 64, "taking output does not release physical slot");
    f.channel.Configure(false);
    f.world.Configured(true, f.settings, "epoch-2");
    Check(f.channel.Used() == 1 && !f.channel.Drained(), "retirement disposes queues but retains actual held write");
    Check(!f.world.ObserveDrain(f.channel.Drained(), RetirementProof::NoLiveCaptures),
          "held retired response blocks new epoch");
    reply.reset();
    Check(f.channel.Drained(), "actual write disposal releases last slot");
    heldInference = {};
    Check(f.world.ObserveDrain(f.channel.Drained(), RetirementProof::NoLiveCaptures), "drained selection");
    f.channel.Configure(true);
    Check(f.channel.Activate(), "reconnect binds new selected epoch");
    Check(f.channel.Take(f.settings.token, ids.front(), unauthorized) == ChannelError::Unknown,
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
    for (auto const* type : {"events.result", "failure", "command"})
    {
        Fixture f;
        Context input;
        input.speakerGuid = 22;
        input.channel = "party";
        input.text = "Ready?";
        auto const event = f.world.Reserve(f.ticket.scope, input, f.now, 30000);
        uint64_t outbound = 0;
        Check(f.channel.RegisterEvents(event, f.guard, {{44, 3}}, outbound), "second same-scope route registered");
        auto message = f.Wire(type, event.request.requestId);
        if (std::string(type) == "failure")
            message["code"] = "unavailable";
        if (std::string(type) == "command")
        {
            message = Json::parse(f.Command("cross-route-effect"));
            message["requestId"] = event.request.requestId;
        }
        auto const wrongReceipt = f.Receive(message.dump());
        Check(f.channel.Update() == 1, "wrong-route message processed");
        auto wrong = f.Take(wrongReceipt);
        Check(wrong->error == ChannelError::Invalid && wrong->body.empty(), "wrong route rejected without echo");
        wrong.reset();
        Check(f.world.Current(event, f.now) && f.channel.Used() == 1,
              "rejected input preserves other world authority and queued event");

        // Queries deliberately have a new id; route correlation must not couple it to the generation id.
        auto const queryReceipt = f.Receive(f.Query("independent-query"));
        f.channel.Update();
        auto query = f.Take(queryReceipt);
        Check(query->error == ChannelError::None && Json::parse(query->body)["code"] == "unavailable",
              "original route remains usable with independent query id");
        query.reset();

        uint64_t rightReceipt = 0;
        Check(f.channel.Receive(f.settings.token, event.request.requestId, message.dump(), rightReceipt) ==
                  ChannelError::None,
              "same message admitted through correct route");
        f.channel.Update();
        if (std::string(type) == "command")
        {
            auto command = f.Take(rightReceipt);
            Check(command->error == ChannelError::None && Json::parse(command->body)["outcome"] == "rejected",
                  "wrong route did not reserve command replay id; execution stays closed");
            command.reset();
            Check(f.world.Current(event, f.now), "accepted proposal does not terminate event");
            auto const ack = f.Wire("events.result", event.request.requestId).dump();
            Check(f.channel.Receive(f.settings.token, event.request.requestId, ack, rightReceipt) == ChannelError::None,
                  "event terminal after correctly routed command");
            f.channel.Update();
        }
        Check(!f.world.Current(event, f.now) && f.channel.Drained() && f.world.Pending() == 1,
              "valid terminal releases event authority and queued outbound packet only");
        std::unique_ptr<ChannelPacket> cancelled;
        Check(f.channel.Take(f.settings.token, outbound, cancelled) == ChannelError::Unknown,
              "terminal cancels queued event receipt");
        Check(f.world.Current(f.ticket, f.now), "other generation authority remains current");
    }
}

void ExpiryReloadAndDefaults()
{
    Fixture off(false);
    uint64_t id = 0;
    Check(
        off.channel.Receive(off.settings.token, off.ticket.request.requestId, "{", id) == ChannelError::Unauthorized &&
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
    Check(completed.channel.Take(completed.settings.token, id, packet) == ChannelError::Stale &&
              completed.channel.Drained(),
          "expired poll actually disposes completion");
    Fixture reload;
    id = reload.Receive(reload.Query("retired"));
    reload.channel.Configure(true);
    reload.world.Configured(true, reload.settings, "epoch-2");
    Check(reload.channel.Receive(reload.settings.token, reload.ticket.request.requestId, "{", id) ==
              ChannelError::Unauthorized,
          "reload revokes old authenticated receiver immediately");
    Check(!reload.channel.Activate(), "reload never self acknowledges selection");
}

void HeartbeatReplayLedger()
{
    ChannelPolicy policy;
    Clock const now{100000, 200000};
    WorldScope scope{1, "fixture", "codex", "epoch", 11, 22, "context", "party", "visible", "c1", "m1", "p1"};
    Check(policy.Bind({"fixture", "codex", "", "epoch"}, "synthetic-local-channel-token"), "ledger bind");
    Check(policy.ObserveScope(scope, now), "ledger scope");
    std::string body;
    for (std::size_t i = 0; i < ChannelMaxIds; ++i)
    {
        auto const id = "r" + std::to_string(i);
        Check(policy.OpenGeneration(scope, id, {101000, 201000}, now) == ChannelError::None, "ledger reserve");
        if (i + 1 != ChannelMaxIds)
            Check(policy.FinishFailure({"fixture", "codex", "epoch", id, 101000}, ChannelFailure::Cancelled, scope, now,
                                       body) == ChannelError::None,
                  "ledger retain closed id");
    }
    Check(policy.RequestReservations() == 4096 && policy.Pending() == 1, "full replay ledger retained");
    Check(policy.ObserveScope(scope, {101000, 200100}), "heartbeat at wall deadline with live scope");
    Check(policy.Pending() == 0, "heartbeat logical expiry without request-table sweep");
    Check(!policy.Current({"fixture", "codex", "epoch", "r4095", 101000}, scope, now),
          "wall rollback cannot revive request after trusted heartbeat expiry");
    Check(policy.ObserveScope(scope, {106000, 206000}), "scope gap refresh");
    Check(!policy.Current({"fixture", "codex", "epoch", "r4095", 101000}, scope, now),
          "scope lapse and clock rollback never revive retained work");
}
}  // namespace

int main()
{
    try
    {
        PhysicalAndIndependent();
        GuardsAndTerminal();
        RouteCorrelation();
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
