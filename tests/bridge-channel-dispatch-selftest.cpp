// Actual production mailbox, world policy and channel policy; controlled world observation, no server/native I/O.
#include <atomic>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

#include "bridge-ability-double.h"
#include "mod-ollama-chat_bridge_query.h"
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
    QueryWorldSource* const source = nullptr;
    WorldChannel channel{world,
                         [this](WorldTicket const&, DispatchGuard& current)
                         {
                             ++observations;
                             current = guard;
                             return visible && (!failAt || observations < failAt);
                         },
                         [this] { return now; }, source};
    WorldTicket ticket;

    explicit Fixture(bool enable = true, QueryWorldSource* with = nullptr) : source(with)
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
    // LCB6b: the answer a listener thread waits on.
    std::shared_ptr<ChannelAnswer> Receive(std::string const& body)
    {
        auto answer = std::make_shared<ChannelAnswer>();
        Check(channel.Receive(ticket.request.requestId, body, answer) == ChannelError::None,
              "authenticated producer admission");
        return answer;
    }
    static std::unique_ptr<ChannelPacket> Take(std::shared_ptr<ChannelAnswer> const& answer)
    {
        std::unique_ptr<ChannelPacket> packet;
        Check(answer->Wait(std::chrono::steady_clock::now(), packet) && packet, "authenticated completion operation");
        return packet;
    }
    // The answer's word; its packet is disposed of here, as the listener's write does.
    static ChannelError Word(std::shared_ptr<ChannelAnswer> const& answer) { return Take(answer)->error; }
};

void PhysicalAndIndependent()
{
    Fixture f;
    auto heldInference = DispatchSlot::Acquire();
    std::vector<std::shared_ptr<ChannelAnswer>> ids;
    // A distinct producer thread may receive while generation is held; no world observation occurs there.
    std::atomic<bool> accepted{true};
    unsigned const before = f.observations;
    std::thread producer(
        [&]
        {
            for (unsigned i = 0; i < WorldChannel::MaxPackets; ++i)
            {
                auto id = std::make_shared<ChannelAnswer>();
                if (f.channel.Receive(f.ticket.request.requestId, f.Query("q" + std::to_string(i)), id) !=
                    ChannelError::None)
                    accepted = false;
                ids.push_back(std::move(id));
            }
        });
    producer.join();
    Check(accepted && f.observations == before, "producer neither blocks on inference nor reads world");
    Check(f.channel.Used() == 64, "physical receive capacity");
    auto const overflow = std::make_shared<ChannelAnswer>();
    Check(f.channel.Receive(f.ticket.request.requestId, "{", overflow) == ChannelError::Capacity,
          "physical bound before body parse");
    Check(f.channel.Update() == WorldChannel::TickBudget, "exact bounded world dequeue");
    Check(f.channel.Used() == 64 && heldInference, "queued completions retain slots while inference held");
    auto reply = f.Take(ids.front());
    Check(reply->error == ChannelError::None && Json::parse(reply->body)["code"] == "unavailable",
          "missing filtered adapter fails explicitly closed");
    Check(f.channel.Used() == 64, "taking output does not release physical slot");
    f.channel.Configure(false);
    f.world.Configured(true, f.settings, "epoch-2");
    bool retired = true;
    for (std::size_t i = 1; i < ids.size(); ++i)
        retired =
            Fixture::Word(ids[i]) == (i < WorldChannel::TickBudget ? ChannelError::None : ChannelError::Inactive) &&
            retired;
    Check(retired, "LCB6b: retirement answers every queued packet inactive");
    ids.clear();
    Check(f.channel.Used() == 1 && !f.channel.Drained(), "retirement disposes queues but retains actual held write");
    Check(!f.world.ObserveDrain(f.channel.Drained()), "held retired response blocks new epoch");
    reply.reset();
    Check(f.channel.Drained(), "actual write disposal releases last slot");
    heldInference = {};
    Check(f.world.ObserveDrain(f.channel.Drained()), "drained selection");
    f.channel.Configure(true);
    Check(f.channel.Activate(), "reconnect binds new selected epoch");
    auto const old = std::make_shared<ChannelAnswer>();
    Check(f.channel.Receive(f.ticket.request.requestId, f.Query("after"), old) == ChannelError::Closed,
          "the old epoch's route is answered closed after reconnect");
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
    Check(Fixture::Word(id) == ChannelError::Stale, "LCB6b: a missing unit before the read is answered stale");
    Check(f.channel.Drained(), "fresh missing-unit guard drops before admission");
    f.visible = true;
    unsigned const seen = f.observations;
    id = f.Receive(f.Query("one-look"));
    f.channel.Update();
    Check(Fixture::Word(id) == ChannelError::None && f.observations == seen + 1,
          "LCB6b: one fresh capture per query packet, just before its read");
    id = f.Receive(f.Query("closed"));
    f.channel.Close(f.ticket);
    Check(Fixture::Word(id) == ChannelError::Closed, "LCB6b: a close answers its queued query closed at once");
    Check(f.channel.Update() == 0 && f.channel.Drained(), "parent terminal closes route before queued query");
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
            auto receipt = std::make_shared<ChannelAnswer>();
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
        auto const late = std::make_shared<ChannelAnswer>();
        Check(f.channel.Receive(second.request.requestId, message.dump(), late) == ChannelError::Closed &&
                  f.channel.Drained(),
              "LCB6b: a closed route is answered closed at once");
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
            auto const receipt = std::make_shared<ChannelAnswer>();
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
    auto id = std::make_shared<ChannelAnswer>();
    Check(off.channel.Receive(off.ticket.request.requestId, "{", id) == ChannelError::Inactive &&
              off.channel.Update() == 0,
          "off has no reverse admission or tick work");
    Fixture f;
    id = f.Receive(f.Query("expired"));
    f.now.steadyMs += WorldLivenessMs;
    f.now.unixMs -= 1000;
    Check(f.channel.Used() == 1, "logical expiry and rollback do not release physical queue");
    f.channel.Update();
    Check(Fixture::Word(id) == ChannelError::Stale, "LCB6b: a packet older than its liveness is answered stale");
    Check(f.channel.Drained(), "exact monotonic expiry disposed at bounded dequeue");
    Fixture abandoned;
    {
        auto const answer = abandoned.Receive(abandoned.Query("pending-output"));
        abandoned.channel.Update();
        Check(abandoned.channel.Used() == 1, "an answer holds its slot while its waiter holds it");
    }
    Check(abandoned.channel.Drained(), "a waiter that gave up disposes its answer and the slot");
    {
        auto const answer = abandoned.Receive(abandoned.Query("gave-up"));
    }
    Check(abandoned.channel.Used() == 1 && abandoned.channel.Update() == 1 && abandoned.channel.Drained(),
          "a packet whose waiter gave up is disposed at its dequeue");
    Fixture reload;
    id = reload.Receive(reload.Query("retired"));
    reload.channel.Configure(true);
    Check(Fixture::Word(id) == ChannelError::Inactive && reload.channel.Drained(),
          "LCB6b: a reload answers the queued packet inactive");
    reload.world.Configured(true, reload.settings, "epoch-2");
    id = std::make_shared<ChannelAnswer>();
    Check(reload.channel.Receive(reload.ticket.request.requestId, "{", id) == ChannelError::Inactive,
          "reload revokes the old receiver immediately");
    Check(!reload.channel.Activate(), "reload never self acknowledges selection");
}

// LCB6b exit: WorldPolicy is asked once per query packet, just before its read, and once more for a command, after
// its executor. At e68c5a6 (counted from the source, not run) a bot_state or catalogue packet took 8 such calls and an
// observable_group packet 18.
struct ReadSource : QueryWorldSource
{
    bool GroupMembers(WorldTicket const&, uint64_t, std::vector<uint64_t>& members) override
    {
        members = {11, 22};
        return true;
    }
    bool ResolveVisible(WorldTicket const&, uint64_t guid, VisibleUnit& unit) override
    {
        unit = {guid, 33, "unit-" + std::to_string(guid), 70};
        return true;
    }
    bool ReadTopic(WorldTicket const&, StateTopic, std::string const&, std::vector<StateEntry>& entries) override
    {
        entries.clear();
        return true;
    }
};

void OneCheckPerPacket()
{
    ReadSource source;
    Fixture f(true, &source);
    auto const query = [&](char const* kind, char const* id)
    {
        auto json = Json::parse(f.Query(id));
        json["query"] = kind;
        if (std::string(kind) == "bot_state")
            json["topic"] = "spells";
        return json.dump();
    };
    struct Case
    {
        std::string body;
        char const* type;
        uint64_t checks;
        char const* label;
    };
    for (auto const& c :
         {Case{query("observable_group", "q-group"), "query.result", 1,
               "LCB6b: one WorldPolicy check per observable_group packet"},
          Case{query("bot_state", "q-state"), "query.result", 1, "LCB6b: one WorldPolicy check per bot_state packet"},
          Case{query("catalogue", "q-catalogue"), "", 1, "LCB6b: one WorldPolicy check per catalogue packet"},
          Case{f.Command("c-1"), "command.result", 2,
               "LCB6b: a command is checked before its effect and after its executor"}})
    {
        uint64_t const before = f.world.Checks();
        auto const answer = f.Receive(c.body);
        Check(f.channel.Update() == 1, "one packet per tick");
        uint64_t const checks = f.world.Checks() - before;
        auto const packet = Fixture::Take(answer);
        auto const body = Json::parse(packet->body, nullptr, false);
        Check(packet->error == ChannelError::None && body.is_object() && body.value("code", "") != "unavailable" &&
                  (!*c.type || body.value("type", "") == c.type),
              "the packet was read or executed");
        std::cout << "count " << c.label << ": " << checks << "\n";
        Check(checks == c.checks, c.label);
    }
}

void HeartbeatReplayLedger()
{
    ChannelPolicy policy;
    Clock const now{100000, 200000};
    WorldScope scope{1, "fixture", "codex", "epoch", 11, 22, "context", "party", "visible", "c1", "m1"};
    Check(policy.Bind({"fixture", "codex", "", "epoch"}), "ledger bind");
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
    policy.Expire({101000, 200100});
    Check(policy.Pending() == 0 && policy.RequestReservations() == 0, "the wall deadline ends the open route");
    Check(!policy.Current({"fixture", "codex", "epoch", "r4999", 101000}, scope, now),
          "wall rollback cannot revive request after trusted expiry");
    Check(policy.OpenGeneration(scope, "r4999", {101000, 201000}, now) == ChannelError::Stale,
          "the trusted clock's high-water mark keeps an expired id from reopening");
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
        OneCheckPerPacket();
        std::cout << "PASS " << checks << " production channel dispatch checks\n";
    }
    catch (std::exception const& e)
    {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
