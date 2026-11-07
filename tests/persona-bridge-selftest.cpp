// Server-free proof of the persona card transport: production Build/Parse from mod-ollama-chat_bridge.cpp, plus the
// dispatch's card delivery block (BEGIN/END BRIDGE CARD DELIVERY) extracted verbatim into persona-card-delivery.inc
// by the hub runner (scratchpad/bridge-shared/J14b4-check/run.cjs). The policy, the chat line and the handler's world
// function are recording stand-ins; no live-game, store or database proof.
#include <cstdlib>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mod-ollama-chat_bridge.h"
#include "mod-ollama-chat_bridge_world.h"
#include "mod-ollama-chat_persona_bridge.h"

// Defined with the HTTP client; only the Codex observer path calls it, which these fixtures never select.
bool OllamaBridge::ValidObserverSettings(Settings const&) { return false; }

namespace
{
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

OllamaBridge::Clock const Clock{1000000, 10000};

OllamaBridge::Settings Settings()
{
    OllamaBridge::Settings settings;
    settings.instance = "fixture-main";
    settings.provider = "fixture-a";
    settings.endpoint = "http://127.0.0.1:47800";
    settings.token = "synthetic-not-a-real-link-token";
    return settings;
}

OllamaBridge::Request Request()
{
    OllamaBridge::Request request;
    request.epoch = "fixture-epoch-a";
    request.requestId = "generation-1";
    request.deadline = OllamaBridge::CaptureDeadline(Clock, 10000);
    request.controllerGuid = 9007199254740993ULL;
    request.botGuid = std::numeric_limits<uint64_t>::max();
    request.input.speakerGuid = request.controllerGuid;
    request.input.channel = "direct";
    request.input.text = "Do you remember me?";
    request.input.systemPrompt = "World rules\n";
    request.input.persona = "A \"steady\" companion";
    request.input.memory = "We met by the inn.";
    request.input.speakerControlsBot = true;
    request.input.directAddress = true;
    request.input.observations = {"Your name is Fixture.", "The speaker is Friend."};
    return request;
}

OllamaBridge::CardFacts Facts()
{
    OllamaBridge::CardFacts facts;
    facts.name = "Fixture";
    facts.race = "Dwarf";
    facts.className = "Warrior";
    facts.gender = "female";
    facts.level = 42;
    facts.zone = "Dun Morogh";
    facts.faction = "Alliance";
    facts.traits = "Gruff";
    return facts;
}

// DS.12: a bot without a card is offered with its facts; an accepted card is only reported.
OllamaBridge::Request Offered(uint64_t revision = 5, bool accepted = true)
{
    auto request = Request();
    request.input.card = {true, revision, 77, accepted, accepted ? OllamaBridge::CardFacts{} : Facts()};
    return request;
}

nlohmann::json FactsJson()
{
    return {{"name", "Fixture"}, {"race", "Dwarf"},      {"class", "Warrior"},    {"gender", "female"},
            {"level", 42},       {"zone", "Dun Morogh"}, {"faction", "Alliance"}, {"traits", "Gruff"}};
}

// Today's generation envelope, built independently of the production code.
nlohmann::json Envelope(OllamaBridge::Settings const& settings, OllamaBridge::Request const& request)
{
    auto const& input = request.input;
    return {{"version", 1},
            {"instance", settings.instance},
            {"provider", settings.provider},
            {"epoch", request.epoch},
            {"requestId", request.requestId},
            {"deadlineUnixMs", request.deadline.unixMs},
            {"type", "generation"},
            {"controllerGuid", std::to_string(request.controllerGuid)},
            {"botGuid", std::to_string(request.botGuid)},
            {"input",
             {{"channel", input.channel},
              {"text", input.text},
              {"observations", input.observations},
              {"speakerGuid", std::to_string(input.speakerGuid)},
              {"systemPrompt", input.systemPrompt},
              {"persona", input.persona},
              {"memory", input.memory},
              {"groupScope", input.groupScope},
              {"directAddress", input.directAddress},
              {"speakerControlsBot", input.speakerControlsBot},
              {"party", nlohmann::json::array()}}}};
}

nlohmann::json Reply(OllamaBridge::Settings const& settings, OllamaBridge::Request const& request)
{
    return {{"version", 1},
            {"instance", settings.instance},
            {"provider", settings.provider},
            {"epoch", request.epoch},
            {"requestId", request.requestId},
            {"deadlineUnixMs", request.deadline.unixMs},
            {"type", "generation.result"},
            {"text", "Of course I do."}};
}

nlohmann::json Proposal(nlohmann::json revision, nlohmann::json body)
{
    return {{"revision", std::move(revision)}, {"body", std::move(body)}};
}

OllamaBridge::Result ParseJson(OllamaBridge::Request const& request, nlohmann::json const& reply)
{
    return OllamaBridge::Parse(Settings(), request, reply.dump(), Clock);
}

bool Invalid(OllamaBridge::Result const& result)
{
    return !result.ok && result.error == "bridge_invalid_response" && result.text.empty() && !result.personaProposal;
}

void BuildChecks()
{
    using namespace OllamaBridge;
    auto const settings = Settings();
    std::string body;
    auto plain = Request();
    Check(Build(settings, plain, Clock, body), "plain request builds");
    Check(body == Envelope(settings, plain).dump(), "no opportunity: request body byte-identical to today");
    Check(body.find("personaCard") == std::string::npos, "no opportunity: no card key");

    auto offered = Offered();
    Check(Build(settings, offered, Clock, body), "offered request builds");
    auto expected = Envelope(settings, offered);
    expected["input"]["personaCard"] = {{"revision", "5"}, {"accepted", true}};
    Check(body == expected.dump(), "accepted card: exact personaCard {revision, accepted}, no context");
    offered.input.card.facts = Facts();
    Check(Build(settings, offered, Clock, body) && body == expected.dump(),
          "accepted card: facts never go on the wire");
    Check(body.find("\"stamp\"") == std::string::npos, "opportunity: the store stamp never goes on the wire");

    auto missing = Offered(0, false);
    Check(Build(settings, missing, Clock, body), "missing row builds");
    expected = Envelope(settings, missing);
    expected["input"]["personaCard"] = {{"revision", "0"}, {"accepted", false}, {"context", FactsJson()}};
    Check(body == expected.dump(), "missing row: revision 0, not accepted, the bot's facts as context");
    auto sparse = Offered(3, false);
    sparse.input.card.facts = {};
    sparse.input.card.facts.race = "Gnome";
    Check(Build(settings, sparse, Clock, body) &&
              nlohmann::json::parse(body)["input"]["personaCard"]["context"] == nlohmann::json{{"race", "Gnome"}},
          "absent facts, blank strings and level 0 are left out");
    sparse.input.card.facts.className = " \t ";
    sparse.input.card.facts.zone = "Elwynn\x01 Forest\x7F\n";
    sparse.input.card.facts.guild = "Bad\xFF";
    sparse.input.card.facts.name = std::string(70, 'n');
    // 63 ASCII bytes and a two-byte character across the 64-byte cut: the whole character is dropped.
    sparse.input.card.facts.traits = std::string(63, 't') + "\xC3\xA9" + "x";
    Check(Build(settings, sparse, Clock, body), "dirty facts still build");
    auto const context = nlohmann::json::parse(body)["input"]["personaCard"]["context"];
    Check(context.size() == 4 && context["race"] == "Gnome" && context["zone"] == "Elwynn Forest" &&
              context["name"] == std::string(MaxCardFactBytes, 'n') && context["traits"] == std::string(63, 't') &&
              !context.contains("class") && !context.contains("guild"),
          "facts drop control bytes, cut at 64 bytes on a character boundary, and drop invalid UTF-8");
    sparse.input.card.facts.traits = std::string(62, 't') + "\xC3\xA9" + "x";
    Check(Build(settings, sparse, Clock, body) &&
              nlohmann::json::parse(body)["input"]["personaCard"]["context"]["traits"] ==
                  std::string(62, 't') + "\xC3\xA9",
          "a character ending exactly at the cut is kept");
    auto large = Offered(std::numeric_limits<uint64_t>::max(), false);
    Check(
        Build(settings, large, Clock, body) && body.find("\"revision\":\"18446744073709551615\"") != std::string::npos,
        "revision is canonical decimal uint64");

    auto bad = Offered();
    bad.input.card.stamp = 0;
    Check(!Build(settings, bad, Clock, body) && body.empty(), "offered without a store stamp refused");
    bad = Offered(0, true);
    Check(!Build(settings, bad, Clock, body), "accepted card at revision 0 refused");
    bad = Request();
    bad.input.card.revision = 3;
    Check(!Build(settings, bad, Clock, body), "unoffered card values refused");
    bad = Request();
    bad.input.card.stamp = 3;
    Check(!Build(settings, bad, Clock, body), "unoffered stamp refused");
    bad = Request();
    bad.input.card.accepted = true;
    Check(!Build(settings, bad, Clock, body), "unoffered accepted flag refused");

    // The opportunity counts against MaxRequestBytes: pad a plain request to just under the bound.
    auto big = Request();
    big.input.memory = std::string(8192, '\x01');  // Escaped as \u0001: six bytes each.
    big.input.systemPrompt = std::string(2000, '\x01');
    big.input.persona.clear();
    Check(Build(settings, big, Clock, body), "padding base builds");
    std::size_t const pad = MaxRequestBytes - body.size() - 20;
    Check(pad <= 8192, "padding fits the persona bound");
    big.input.persona = std::string(pad, 'a');
    Check(Build(settings, big, Clock, body) && body.size() == MaxRequestBytes - 20, "plain request 20 bytes under");
    big.input.card = {true, 5, 77, true, {}};
    Check(!Build(settings, big, Clock, body) && body.empty(), "the opportunity pushes it over MaxRequestBytes");
}

void ParseChecks()
{
    using namespace OllamaBridge;
    auto const settings = Settings();
    auto const plain = Request();
    auto const offered = Offered(5, false);
    auto const accepted = Offered();

    auto result = ParseJson(plain, Reply(settings, plain));
    Check(result.ok && result.text == "Of course I do." && !result.personaProposal, "text-only, no opportunity");
    result = ParseJson(offered, Reply(settings, offered));
    Check(result.ok && result.text == "Of course I do." && !result.personaProposal, "text-only with opportunity");

    auto reply = Reply(settings, offered);
    reply["personaProposal"] = Proposal("5", "Remembers the friend from the inn.");
    result = ParseJson(offered, reply);
    Check(result.ok && result.error.empty() && result.text == "Of course I do." && result.personaProposal &&
              *result.personaProposal == "Remembers the friend from the inn.",
          "valid proposal parsed beside unchanged text");
    Check(result.text.find("Remembers") == std::string::npos, "proposal never merges into text");

    auto const missing = Offered(0, false);
    reply = Reply(settings, missing);
    reply["personaProposal"] = Proposal("0", "First card.");
    result = ParseJson(missing, reply);
    Check(result.ok && result.personaProposal && *result.personaProposal == "First card.", "proposal for revision 0");

    reply = Reply(settings, plain);
    reply["personaProposal"] = Proposal("5", "Unasked.");
    Check(Invalid(ParseJson(plain, reply)), "proposal without an opportunity refused");
    reply = Reply(settings, accepted);
    reply["personaProposal"] = Proposal("5", "A replacement.");
    Check(Invalid(ParseJson(accepted, reply)), "proposal for a bot with an accepted card refused");
    result = ParseJson(accepted, Reply(settings, accepted));
    Check(result.ok && !result.personaProposal, "text-only with an accepted card");
    reply = Reply(settings, offered);
    reply["personaProposal"] = Proposal("5", std::string(OllamaPersona::MaxGeneratedCardBytes + 1, 'b'));
    result = ParseJson(offered, reply);
    Check(result.ok && result.text == "Of course I do." && result.personaProposal &&
              result.personaProposal->size() == OllamaPersona::MaxGeneratedCardBytes + 1,
          "an over-long card keeps the chat line; the module's card rules refuse it later");

    for (auto const& revision : {nlohmann::json("6"), nlohmann::json("05"), nlohmann::json("+5"), nlohmann::json(" 5"),
                                 nlohmann::json("5 "), nlohmann::json(""), nlohmann::json(5), nlohmann::json(5.0),
                                 nlohmann::json(nullptr), nlohmann::json::array({"5"})})
    {
        reply = Reply(settings, offered);
        reply["personaProposal"] = Proposal(revision, "Body.");
        Check(Invalid(ParseJson(offered, reply)), "wrong revision value or type refused");
    }

    std::string const exact(MaxCardProposalBytes, 'b');
    for (auto const& body :
         {nlohmann::json(""), nlohmann::json(" \t\r\n"), nlohmann::json(std::string(MaxCardProposalBytes + 1, 'b')),
          nlohmann::json(std::string("a\0b", 3)), nlohmann::json(7), nlohmann::json(nullptr),
          nlohmann::json::array({"a"}), nlohmann::json::object({{"text", "a"}})})
    {
        reply = Reply(settings, offered);
        reply["personaProposal"] = Proposal("5", body);
        Check(Invalid(ParseJson(offered, reply)), "empty, blank, oversize, NUL or non-string body refused");
    }
    reply = Reply(settings, offered);
    reply["personaProposal"] = Proposal("5", exact);
    result = ParseJson(offered, reply);
    Check(result.ok && result.personaProposal && result.personaProposal->size() == MaxCardProposalBytes,
          "body of exactly MaxCardProposalBytes accepted");
    reply["text"] = std::string(MaxTextBytes, 't');
    result = ParseJson(offered, reply);
    Check(result.ok && result.text.size() == MaxTextBytes && result.personaProposal,
          "maximum text and maximum body fit MaxResponseBytes");

    for (auto const& card : {nlohmann::json("5"), nlohmann::json(nullptr), nlohmann::json::array({"5", "Body."}),
                             nlohmann::json::object({{"revision", "5"}}), nlohmann::json::object({{"body", "Body."}}),
                             nlohmann::json::object({{"revision", "5"}, {"body", "Body."}, {"source", "claude"}}),
                             nlohmann::json::object({{"revision", "5"}, {"text", "Body."}})})
    {
        reply = Reply(settings, offered);
        reply["personaProposal"] = card;
        Check(Invalid(ParseJson(offered, reply)), "proposal not exactly {revision, body} refused");
    }

    // Surprise keys: a ninth key that is not the proposal, or a tenth beside it.
    reply = Reply(settings, offered);
    reply["personaCard"] = Proposal("5", "Body.");
    Check(Invalid(ParseJson(offered, reply)), "other ninth key refused");
    reply = Reply(settings, offered);
    reply["personaProposal"] = Proposal("5", "Body.");
    reply["memory"] = "x";
    Check(Invalid(ParseJson(offered, reply)), "tenth key refused");

    // Duplicate keys, raw: the existing guard sees nested objects as well as the top level.
    auto raw = Reply(settings, offered).dump();
    raw.pop_back();
    Check(Invalid(Parse(settings, offered, raw + R"(,"personaProposal":{"revision":"5","revision":"5","body":"B."}})",
                        Clock)),
          "duplicate key inside the proposal refused");
    Check(Invalid(Parse(settings, offered,
                        raw + R"(,"personaProposal":{"revision":"5","body":"B."},"personaProposal":{"revision":"5",)"
                              R"("body":"B."}})",
                        Clock)),
          "duplicate proposal key refused");
    Check(Invalid(Parse(settings, offered, raw + ",\"personaProposal\":{\"revision\":\"5\",\"body\":\"\xff\xfe\"}}",
                        Clock)),
          "ill-formed UTF-8 body refused");
    result = Parse(settings, offered, raw + R"(,"personaProposal":{"revision":"5","body":"Café."}})", Clock);
    Check(result.ok && result.personaProposal && *result.personaProposal == "Caf\xc3\xa9.", "escaped UTF-8 body kept");

    // Worst-case escaping fails closed on the response bound instead of being truncated.
    reply = Reply(settings, offered);
    reply["personaProposal"] = Proposal("5", std::string(MaxCardProposalBytes, '\x01'));
    Check(reply.dump().size() > MaxResponseBytes && Invalid(ParseJson(offered, reply)),
          "oversize encoded response refused");

    // A failure never carries a proposal; with one it is malformed, not a terminal failure.
    auto failure = Reply(settings, offered);
    failure.erase("text");
    failure["type"] = "failure";
    failure["code"] = "unavailable";
    result = ParseJson(offered, failure);
    Check(!result.ok && result.error == "bridge_terminal_failure", "plain failure unchanged");
    failure["personaProposal"] = Proposal("5", "Body.");
    Check(Invalid(ParseJson(offered, failure)), "failure with a proposal refused");

    // Replay or stale identity: the existing envelope checks reject the whole result, proposal included.
    std::vector<std::pair<char const*, nlohmann::json>> const stale{{"epoch", "fixture-epoch-old"},
                                                                    {"requestId", "generation-0"},
                                                                    {"deadlineUnixMs", offered.deadline.unixMs + 1},
                                                                    {"instance", "fixture-other"},
                                                                    {"provider", "fixture-b"}};
    for (auto const& [key, value] : stale)
    {
        reply = Reply(settings, offered);
        reply["personaProposal"] = Proposal("5", "Body.");
        reply[key] = value;
        Check(Invalid(ParseJson(offered, reply)), "old epoch, other request, deadline or identity refused");
    }
    reply = Reply(settings, offered);
    reply["personaProposal"] = Proposal("5", "Body.");
    result = Parse(settings, offered, reply.dump(), {Clock.unixMs + 20000, Clock.steadyMs + 20000});
    Check(!result.ok && result.error == "bridge_expired_or_invalid_request" && !result.personaProposal,
          "expired request refuses the proposal");
}

// --- Dispatch card delivery: recording stand-ins for the extracted block. ---------------------------------------

struct Recorder
{
    std::vector<std::string> events;
    std::set<uint64_t> spent;
    bool current = true;
    bool consumeAllowed = true;
    int held = 0;
    bool heldAtPropose = false;
    uint64_t revision = 0;
    uint64_t stamp = 0;
    std::string body;
    OllamaPersona::ProposalStatus status = OllamaPersona::ProposalStatus::Queued;
    int logs = 0;
} recorder;
}  // namespace

OllamaPersona::ProposalStatus OllamaChat_BridgeProposeCard(OllamaBridge::WorldTicket const&, uint64_t expectedRevision,
                                                           uint64_t stamp, std::string body)
{
    recorder.events.push_back("propose");
    recorder.heldAtPropose = recorder.held > 0;
    recorder.revision = expectedRevision;
    recorder.stamp = stamp;
    recorder.body = std::move(body);
    return recorder.status;
}

bool g_DebugEnabled = true;

namespace
{
template <typename... Args>
void LogInfo(Args const&...)
{
    ++recorder.logs;
}
#define LOG_INFO(...) LogInfo(__VA_ARGS__)

struct OllamaWorldSnapshot
{
};

struct OllamaChatRequest
{
    uint64_t botGuid = 0;
    OllamaBridge::WorldTicket bridgeTicket;
};

struct Completion
{
    OllamaBridge::WorldOutcome bridgeOutcome = OllamaBridge::WorldOutcome::Cancelled;
    OllamaChatRequest request;
    std::optional<std::string> personaProposal;
};

struct FakePolicy
{
    // One-shot like the policy: a spent or refused ticket never consumes again.
    bool Consume(OllamaBridge::WorldTicket const& ticket, OllamaBridge::Clock)
    {
        recorder.events.push_back("consume");
        if (!recorder.consumeAllowed || !recorder.spent.insert(ticket.generation).second)
            return false;
        ++recorder.held;
        return true;
    }

    void Cancel(OllamaBridge::WorldTicket const&)
    {
        recorder.events.push_back("cancel");
        --recorder.held;
    }
} g_bridgeWorld;

bool BridgeCurrent(Completion const&)
{
    recorder.events.push_back("current");
    return recorder.current;
}

void DeliverBridgeLine(Completion const&, OllamaWorldSnapshot const&)
{
    recorder.events.push_back(recorder.held > 0 ? "line" : "line-unheld");
}

#include "persona-card-delivery.inc"

Completion Generation(uint64_t ticket, OllamaBridge::WorldOutcome outcome, bool offered, bool proposal)
{
    Completion c;
    c.bridgeOutcome = outcome;
    c.request.botGuid = 42;
    c.request.bridgeTicket.generation = ticket;
    c.request.bridgeTicket.request = Offered(9, false);
    c.request.bridgeTicket.request.input.card.offered = offered;
    if (proposal)
        c.personaProposal = "A careful healer.";
    return c;
}

void Reset()
{
    auto spent = std::move(recorder.spent);
    recorder = {};
    recorder.spent = std::move(spent);
}

using Events = std::vector<std::string>;

void DeliveryChecks()
{
    using OllamaBridge::WorldOutcome;
    OllamaWorldSnapshot const world;

    Reset();
    auto const c = Generation(1, WorldOutcome::Success, true, true);
    DeliverBridge(c, world);
    Check(recorder.events == Events{"current", "consume", "line", "propose", "cancel"},
          "consume, line, proposal, then terminal cancel");
    Check(recorder.heldAtPropose && recorder.held == 0, "proposal staged while the consumed claim still authorizes");
    Check(recorder.revision == 9 && recorder.stamp == 77 && recorder.body == "A careful healer.",
          "proposal uses the request's captured revision and stamp");
    Check(recorder.logs == 1, "the log records the fixed status only");

    Reset();
    DeliverBridge(c, world);
    Check(recorder.events == Events{"current", "consume"}, "replay after cancel: no line, no proposal");

    Reset();
    recorder.consumeAllowed = false;
    DeliverBridge(Generation(2, WorldOutcome::Success, true, true), world);
    Check(recorder.events == Events{"current", "consume"}, "refused consume (retired or stale): nothing applied");

    Reset();
    recorder.current = false;
    DeliverBridge(Generation(3, WorldOutcome::Success, true, true), world);
    Check(recorder.events == Events{"current"}, "stale binding before consume: nothing applied");

    uint64_t ticket = 10;
    for (auto const outcome :
         {WorldOutcome::Unavailable, WorldOutcome::Empty, WorldOutcome::Filtered, WorldOutcome::Malformed,
          WorldOutcome::Timeout, WorldOutcome::CircuitOpen, WorldOutcome::QueueFull, WorldOutcome::Cancelled})
    {
        Reset();
        DeliverBridge(Generation(++ticket, outcome, true, true), world);
        Check(recorder.events == Events{"current", "consume", "line", "cancel"}, "no proposal on a non-success");
    }

    Reset();
    DeliverBridge(Generation(30, WorldOutcome::Success, true, false), world);
    Check(recorder.events == Events{"current", "consume", "line", "cancel"}, "no proposal returned: nothing staged");

    Reset();
    DeliverBridge(Generation(31, WorldOutcome::Success, false, true), world);
    Check(recorder.events == Events{"current", "consume", "line", "cancel"}, "not offered: never staged");

    Reset();
    auto withCard = Generation(32, WorldOutcome::Success, true, true);
    withCard.request.bridgeTicket.request.input.card.accepted = true;
    DeliverBridge(withCard, world);
    Check(recorder.events == Events{"current", "consume", "line", "cancel"} && !recorder.logs,
          "a bot with an accepted card: never staged");

    for (auto const status : {OllamaPersona::ProposalStatus::Stale, OllamaPersona::ProposalStatus::Invalid,
                              OllamaPersona::ProposalStatus::HasCard, OllamaPersona::ProposalStatus::ManualProtected,
                              OllamaPersona::ProposalStatus::Busy, OllamaPersona::ProposalStatus::Disabled,
                              OllamaPersona::ProposalStatus::Unauthenticated})
    {
        Reset();
        recorder.status = status;
        DeliverBridge(Generation(++ticket, WorldOutcome::Success, true, true), world);
        Check(recorder.events == Events{"current", "consume", "line", "propose", "cancel"} && recorder.held == 0 &&
                  recorder.logs == 1,
              "a refused proposal follows the line, logs its reason and still disposes the claim");
    }
}
}  // namespace

int main()
{
    BuildChecks();
    ParseChecks();
    DeliveryChecks();
    std::cout << "persona-bridge-selftest: " << checks << " checks passed\n";
    return 0;
}
