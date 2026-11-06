// The handler's persona card adapter block (BEGIN/END BRIDGE PERSONA CARD) is extracted verbatim into
// persona-adapter.inc by the hub runner (scratchpad/bridge-shared/J14b3-check/run.cjs) and compiled against the
// production persona store. Players, acquaintance and the bridge world are controlled stand-ins; no live-game proof.
#include <cstdlib>
#include <iostream>
#include <string>

#include "mod-ollama-chat_persona_store.h"

namespace
{
using namespace OllamaPersona;

constexpr uint64_t BotGuid = 0x0000000000000042;
constexpr uint64_t MasterGuid = 0x0000000000000007;
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

struct ObjectGuid
{
    uint64_t raw;
    uint32_t GetCounter() const { return static_cast<uint32_t>(raw); }
};

struct Player
{
    uint64_t guid;
    ObjectGuid GetGUID() const { return {guid}; }
};

struct Acquaintance
{
};

struct AcquaintanceMgr
{
    static AcquaintanceMgr& Instance()
    {
        static AcquaintanceMgr manager;
        return manager;
    }
    bool IsLoaded() const { return loaded; }
    Acquaintance const* Find(uint32_t player, uint32_t bot) const
    {
        ++finds;
        return loaded && player == rowPlayer && bot == rowBot ? &row : nullptr;
    }

    bool loaded = true;
    uint32_t rowPlayer = static_cast<uint32_t>(MasterGuid);
    uint32_t rowBot = static_cast<uint32_t>(BotGuid);
    Acquaintance row;
    mutable unsigned finds = 0;
};

struct PlayerbotAIConfig
{
    bool AcquaintanceEnable = true;
} playerbotConfig;
#define sPlayerbotAIConfig playerbotConfig

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
}

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

BridgeView Capture(Player* bot, Player* speaker, Player* controller, OllamaBridge::DispatchSource source)
{
    BridgeView view;
    view.guard.source = source;
    view.context.persona = Voice;
    BridgeCard(bot, speaker, controller, view);
    return view;
}
}  // namespace

int main()
{
    using From = OllamaBridge::DispatchSource;
    Player bot{BotGuid};
    Player master{MasterGuid};
    Player other{OtherGuid};
    auto& acquaintance = AcquaintanceMgr::Instance();

    BridgeView view = Capture(&bot, &master, &master, From::Whisper);
    Check(view.card.status == CardStatus::Unready && !view.card.personal && view.context.persona == Voice &&
              acquaintance.finds == 0,
          "closed gate: no acquaintance read, voice only");

    Store& store = WorldStore();
    FakePort port;
    Check(store.SetPort(&port), "attach port");
    store.Configure(true);
    Check(store.Activate(1), "activate generation");
    view = Capture(&bot, &master, &master, From::Whisper);
    Check(view.card.status == CardStatus::Unready && view.card.personal && view.context.persona == Voice &&
              CardToken(view.card).empty(),
          "personal pair without an observation stays voice only");

    Check(
        BridgeCardPersonal(&bot, &master, &master, From::Say) && BridgeCardPersonal(&bot, &master, &master, From::Yell),
        "addressed say and yell from the controller are personal");
    Check(!BridgeCardPersonal(&bot, &other, &master, From::Whisper), "a non-controlling speaker is not personal");
    Check(!BridgeCardPersonal(&bot, &master, &master, From::Party), "party scope is never personal");
    playerbotConfig.AcquaintanceEnable = false;
    Check(!BridgeCardPersonal(&bot, &master, &master, From::Whisper), "disabled acquaintance fails closed");
    playerbotConfig.AcquaintanceEnable = true;
    acquaintance.loaded = false;
    Check(!BridgeCardPersonal(&bot, &master, &master, From::Whisper), "unloaded acquaintance fails closed");
    acquaintance.loaded = true;
    acquaintance.rowPlayer = static_cast<uint32_t>(OtherGuid);
    Check(!BridgeCardPersonal(&bot, &master, &master, From::Whisper) &&
              !BridgeCardPersonal(&bot, &other, &other, From::Party),
          "another player's pair never qualifies");
    Check(BridgeCardPersonal(&bot, &other, &other, From::Whisper), "the exact pair's row qualifies");
    acquaintance.rowPlayer = static_cast<uint32_t>(MasterGuid);

    store.Prepare(1, BotGuid, {});
    Check(port.job && port.job->botGuid == BotGuid, "load request uses the full GUID");
    port.ready = StoreCompletion{BotGuid, port.job->id, LookupStatus::Found,
                                 State{BotGuid, 1, Card{Source::Manual, Body}, "", ""}};
    store.Pump({});
    view = Capture(&bot, &master, &master, From::Whisper);
    Check(view.card.status == CardStatus::Accepted && view.card.revision == 1 &&
              view.context.persona == Voice + "\n\n" + Body,
          "controller with acquaintance gets the accepted card");
    BridgeView const party = Capture(&bot, &master, &master, From::Party);
    BridgeView const bystander = Capture(&bot, &other, &master, From::Whisper);
    Check(party.card.status == CardStatus::Unready && party.context.persona == Voice &&
              bystander.card.status == CardStatus::Unready && bystander.context.persona == Voice &&
              CardToken(party.card).empty() && CardToken(bystander.card).empty(),
          "party scope and other speakers get no card text and no card token");
    world.generation = 2;
    view = Capture(&bot, &master, &master, From::Whisper);
    Check(view.card.status == CardStatus::Unready && view.context.persona == Voice,
          "another bridge generation reads nothing");
    store.Configure(false);
    world.generation = 1;
    view = Capture(&bot, &master, &master, From::Whisper);
    Check(view.card.status == CardStatus::Unready && view.context.persona == Voice, "disabled store is voice only");
    std::cout << "persona-adapter selftest: " << checks << " checks passed\n";
}
