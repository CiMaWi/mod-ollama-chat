// The handler's persona card adapter block (BEGIN/END BRIDGE PERSONA CARD) is extracted verbatim into
// persona-adapter.inc by the hub runner (scratchpad/bridge-shared/J14b3-check/run.cjs) and compiled against the
// production persona store. The bridge world is a controlled stand-in; no live-game proof. J14e (DS.12): the card is
// read for every speaker and channel, and a bot without a card is offered at most MaxCardOffers times a generation.
#include <cstdlib>
#include <iostream>
#include <string>

#include "mod-ollama-chat_persona_store.h"

namespace
{
using namespace OllamaPersona;

constexpr uint64_t BotGuid = 0x0000000000000042;
constexpr uint64_t OtherGuid = 0x0000000000000009;
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

struct WorldPolicy
{
    uint64_t Generation() const { return generation; }
    uint64_t generation = 1;
} world;
WorldPolicy& OllamaDispatch_BridgeWorld() { return world; }
}  // namespace

namespace OllamaBridge
{
enum class DispatchSource : uint8_t
{
    Unsupported,
    Whisper,
    Party,
    Say,
    Yell
};

// Field names of the production CardOpportunity; the adapter sets them by name, so a rename fails here too.
struct CardOpportunity
{
    bool offered = false;
    uint64_t revision = 0;
    uint64_t stamp = 0;
    bool accepted = false;
};
}  // namespace OllamaBridge

namespace
{
struct BridgeView
{
    struct
    {
        struct
        {
            uint64_t botGuid = BotGuid;
        } scope;
        OllamaBridge::DispatchSource source = OllamaBridge::DispatchSource::Whisper;
    } guard;
    struct
    {
        std::string persona;
    } context;
    std::string signature;
    CardContext card;
};

#include "persona-adapter.inc"

class FakePort : public StorePort
{
public:
    bool TrySubmit(StoreOperation operation) override
    {
        job = std::move(operation);
        return true;
    }
    std::optional<StoreCompletion> TryPop() override
    {
        auto result = std::move(ready);
        ready.reset();
        return result;
    }

    std::optional<StoreOperation> job;
    std::optional<StoreCompletion> ready;
};

std::string const Voice = "A quiet traveler.";
std::string const Body = "A smith from Goldshire.";

BridgeView Capture(OllamaBridge::DispatchSource source)
{
    BridgeView view;
    view.guard.source = source;
    view.context.persona = Voice;
    BridgeCard(view);
    return view;
}

void Load(Store& store, FakePort& port, uint64_t generation, StoreCompletion completion)
{
    store.Prepare(generation, BotGuid, {});
    Check(port.job && port.job->botGuid == BotGuid, "load request uses the full GUID");
    completion.id = port.job->id;
    port.job.reset();
    port.ready = std::move(completion);
    store.Pump({});
}
}  // namespace

int main()
{
    using From = OllamaBridge::DispatchSource;

    BridgeView view = Capture(From::Whisper);
    Check(view.card.status == CardStatus::Unready && view.context.persona == Voice &&
              !BridgeCardOpportunity(view.card, BotGuid).offered,
          "closed gate: voice only, no opportunity");

    Store& store = WorldStore();
    FakePort port;
    Check(store.SetPort(&port), "attach port");
    store.Configure(true);
    Check(store.Activate(1), "activate generation");
    view = Capture(From::Whisper);
    Check(view.card.status == CardStatus::Unready && view.context.persona == Voice && CardToken(view.card).empty() &&
              !BridgeCardOpportunity(view.card, BotGuid).offered,
          "without an observation: voice only, no opportunity");

    store.Prepare(1, BotGuid, {});
    view = Capture(From::Whisper);
    Check(view.card.status == CardStatus::Loading && !BridgeCardOpportunity(view.card, BotGuid).offered,
          "a loading card offers nothing");
    Check(port.job && port.job->botGuid == BotGuid, "load request uses the full GUID");
    port.ready = StoreCompletion{BotGuid, port.job->id, LookupStatus::Missing, std::nullopt};
    port.job.reset();
    store.Pump({});

    for (From const source : {From::Whisper, From::Party, From::Say})
    {
        view = Capture(source);
        OllamaBridge::CardOpportunity const offer = BridgeCardOpportunity(view.card, BotGuid);
        Check(view.card.status == CardStatus::Missing && view.context.persona == Voice && offer.offered &&
                  offer.revision == 0 && offer.stamp != 0 && offer.stamp == view.card.stamp && !offer.accepted,
              "a bot without a card is offered a card on any channel and speaker");
    }
    view = Capture(From::Yell);
    Check(view.card.status == CardStatus::Missing && !BridgeCardOpportunity(view.card, BotGuid).offered,
          "the fourth opportunity in a generation is not offered");
    Check(BridgeCardOpportunity(view.card, OtherGuid).offered, "the bound counts per bot");
    world.generation = 2;
    Check(store.Activate(2), "activate the next bridge generation");
    view = Capture(From::Whisper);
    Check(view.card.status == CardStatus::Missing && BridgeCardOpportunity(view.card, BotGuid).offered,
          "a new bridge generation offers again");

    store.Forget(BotGuid);
    Load(store, port, 2,
         StoreCompletion{BotGuid, 0, LookupStatus::Found, State{BotGuid, 1, Card{Source::Claude, Body}}});
    for (int i = 0; i < 5; ++i)
    {
        view = Capture(i % 2 ? From::Party : From::Say);
        OllamaBridge::CardOpportunity const offer = BridgeCardOpportunity(view.card, BotGuid);
        Check(view.card.status == CardStatus::Accepted && view.card.included && view.card.revision == 1 &&
                  view.context.persona == Voice + "\n\n" + Body && offer.offered && offer.accepted &&
                  offer.revision == 1 && offer.stamp == view.card.stamp,
              "an included accepted card is reported as accepted on any channel, without a bound");
    }
    BridgeView crowded;
    crowded.guard.source = From::Whisper;
    crowded.context.persona = std::string(MaxCardBytes - CardSeparator.size() - Body.size() + 1, 'v');
    std::string const crowdedVoice = crowded.context.persona;
    BridgeCard(crowded);
    Check(crowded.card.status == CardStatus::Accepted && !crowded.card.included &&
              crowded.context.persona == crowdedVoice && !BridgeCardOpportunity(crowded.card, BotGuid).offered,
          "an accepted card left out as too long offers nothing: a provider never replaces a card");

    store.Forget(BotGuid);
    Load(store, port, 2, StoreCompletion{BotGuid, 0, LookupStatus::Found, State{BotGuid, 4, {}}});
    view = Capture(From::Whisper);
    OllamaBridge::CardOpportunity const legacy = BridgeCardOpportunity(view.card, BotGuid);
    Check(view.card.status == CardStatus::NoCard && view.context.persona == Voice && legacy.offered &&
              legacy.revision == 4 && !legacy.accepted,
          "a row without a card (pre-J14e drafts) is offered a card at its revision");

    world.generation = 3;
    view = Capture(From::Whisper);
    Check(view.card.status == CardStatus::Unready && view.context.persona == Voice &&
              !BridgeCardOpportunity(view.card, BotGuid).offered,
          "another bridge generation reads nothing and offers nothing");
    store.Configure(false);
    world.generation = 2;
    view = Capture(From::Whisper);
    Check(view.card.status == CardStatus::Unready && view.context.persona == Voice, "disabled store is voice only");
    std::cout << "persona-adapter selftest: " << checks << " checks passed\n";
}
