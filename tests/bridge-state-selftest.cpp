// LCB4: the bot's state, facts and wake exactly as Build encodes them in `input`. Links the production bridge unit
// and calls Build and ActivityCode only; no game headers, no HTTP peer. Moved here from bridge-selftest (J5f, J6e-c,
// LC10) so the wire of every state key is proven by a runnable test. The state and the lasting facts are
// playerbots' own structs (BridgeBotState.h, BridgeBotFacts.h); OpenKeyChecks proves that a key playerbots adds
// reaches the wire with no module edit.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mod-ollama-chat_bridge.h"

namespace
{
unsigned checks = 0;

void Check(bool condition, char const* label)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(label);
}

OllamaBridge::Settings Settings()
{
    return {"fixture-main", "fixture-a", "http://127.0.0.1:47800", "synthetic-not-a-real-link-token"};
}

OllamaBridge::Request Request()
{
    OllamaBridge::Request request;
    request.epoch = "fixture-epoch-a";
    request.requestId = "generation-1";
    request.deadline = OllamaBridge::CaptureDeadline({1000000, 10000}, 10000);
    request.controllerGuid = 9007199254740993ULL;
    request.botGuid = std::numeric_limits<uint64_t>::max();
    request.input.speakerGuid = request.controllerGuid;
    request.input.channel = "party";
    request.input.text = "Stay by me.\n\"quoted\"\\";
    request.input.memory = "We met by the inn.\\";
    request.input.groupScope = "party:9007199254740993";
    request.input.speakerControlsBot = true;
    request.input.directAddress = true;
    request.input.observations = {"A visible mark: star", "Someone said \"wait\"."};
    request.input.party = {{request.botGuid, "Fixture \"Bot\"", "healer", 80, request.controllerGuid}};
    return request;
}

// J5f (DS.14 (d)): `input.state` exactly as Build encodes it. Absent unless present; every number bounded and every
// string a card fact (64 bytes, no controls, valid UTF-8); the partner only in the bot's group or sight (yards and
// position only on the same map); the combat target only the bot's own visible victim while alive and fighting.
using PlayerbotBridge::AroundKind;

PlayerbotBridge::AroundEntry Around(std::string name, AroundKind kind, std::string detail, uint32_t count)
{
    PlayerbotBridge::AroundEntry entry;
    entry.Name = std::move(name);
    entry.Kind = kind;
    entry.Detail = std::move(detail);
    entry.Count = count;
    return entry;
}

PlayerbotBridge::BotStateFacts FullState()
{
    PlayerbotBridge::BotStateFacts state;
    state.healthPct = 73.4f;
    state.powerType = 0;
    state.powerCurrent = 4100;
    state.powerMax = 8200;
    state.level = 42;
    state.alive = true;
    state.inCombat = true;
    state.victim = true;
    state.victimVisible = true;
    state.victimName = "Defias Pillager";
    state.victimLevel = 41;
    state.victimType = "humanoid";
    state.victimHealthPct = 35.0f;
    state.travelling = true;
    state.questTitle = "The Defias Brotherhood";
    state.zone = "Westfall";
    state.area = "Moonbrook";
    state.mapId = 0;
    state.x = -11018.4f;
    state.y = 1513.6f;
    state.z = 43.2f;
    state.partnerOnline = true;
    state.partnerInGroup = true;
    state.partnerVisible = true;
    state.partnerSameMap = true;
    state.partnerYards = 18.6f;
    state.partnerZone = "Westfall";
    state.partnerX = -11001.2f;
    state.partnerY = 1520.4f;
    state.partnerZ = 44.0f;
    state.mounted = true;
    state.swimming = true;
    state.groupSize = 2;
    state.masterName = "Aldric";
    state.follow = true;
    state.freeBagSlots = 14;
    state.money = 1234567;
    return state;
}

// The encoded `input.state`, or null when the envelope has none.
nlohmann::json StateWire(std::optional<PlayerbotBridge::BotStateFacts> const& state)
{
    auto request = Request();
    request.input.state = state;
    std::string body;
    Check(OllamaBridge::Build(Settings(), request, {1000000, 10000}, body), "state: build");
    auto const wire = nlohmann::json::parse(body);
    return wire["input"].contains("state") ? wire["input"]["state"] : nlohmann::json();
}

std::string StateActivityKind(PlayerbotBridge::BotStateFacts const& state)
{
    return StateWire(state)["activity"]["kind"].get<std::string>();
}

void StateChecks()
{
    using namespace OllamaBridge;
    float const nan = std::numeric_limits<float>::quiet_NaN();
    float const inf = std::numeric_limits<float>::infinity();
    Check(StateWire(std::nullopt).is_null(), "state: absent unless read");

    Check(StateWire(FullState()) == nlohmann::json::parse(R"({"health":73,"power":{"kind":"mana","pct":50},"level":42,
        "alive":"alive","combat":{"target":{"name":"Defias Pillager","type":"humanoid","level":41,"health":35}},
        "activity":{"kind":"fighting"},"where":{"zone":"Westfall","area":"Moonbrook","mapId":0,"x":-11018,"y":1514,
        "z":43},"partner":{"zone":"Westfall","yards":19,"x":-11001,"y":1520,"z":44},"mounted":true,"swimming":true,
        "group":{"size":2,"leads":false,"follows":"Aldric"},"movement":"follow",
        "bags":14,"money":1234567})"),
          "state: every field encoded with its key");

    // Bounds.
    for (auto const& [pct, want] :
         std::initializer_list<std::pair<float, int>>{{-5.0f, 0}, {150.0f, 100}, {nan, 0}, {inf, 100}, {99.6f, 100}})
    {
        auto state = FullState();
        state.healthPct = pct;
        state.victimHealthPct = pct;
        auto const wire = StateWire(state);
        Check(wire["health"] == want && wire["combat"]["target"]["health"] == want, "state: percent clamped");
    }
    auto state = FullState();
    state.level = 300;
    state.victimLevel = 300;
    state.groupSize = 50;
    state.freeBagSlots = 300;
    state.powerCurrent = 9000;
    state.x = nan;
    state.y = 1e9f;
    state.z = -inf;
    state.partnerYards = -3.0f;
    state.partnerX = inf;
    auto wire = StateWire(state);
    Check(wire["level"] == 255 && wire["combat"]["target"]["level"] == 255, "state: levels capped at 255");
    Check(wire["group"]["size"] == 40 && wire["bags"] == 255, "state: group size and bags capped");
    Check(wire["power"]["pct"] == 100, "state: power above its maximum reads full");
    Check(wire["where"]["x"] == 0 && wire["where"]["y"] == 0 && wire["where"]["z"] == 0 &&
              wire["partner"]["yards"] == 0 && wire["partner"]["x"] == 0,
          "state: yards not finite, beyond any map or negative read 0");
    state = FullState();
    state.partnerYards = nan;
    Check(StateWire(state)["partner"]["yards"] == 0, "state: partner distance NaN reads 0");
    state.level = 0;
    state.victimLevel = 0;
    wire = StateWire(state);
    Check(!wire.contains("level") && !wire["combat"]["target"].contains("level"), "state: an unknown level is omitted");
    state = FullState();
    state.victimType = "";
    Check(!StateWire(state)["combat"]["target"].contains("type"), "state: a target without a type word omits the key");
    state = FullState();
    state.victimName = std::string(63, 'a') + "\xC3\xA9" + std::string(10, 'b');
    state.zone = std::string(70, 'z');
    state.area = std::string("Moon\x01") + "brook\x7F";
    state.questTitle = "\xFF\xFE";
    state.casting = true;
    state.inCombat = false;
    state.masterName = "\t";
    wire = StateWire(state);
    Check(wire["where"]["zone"] == std::string(64, 'z') && wire["where"]["area"] == "Moonbrook",
          "state: strings cut to 64 bytes, controls dropped");
    Check(!wire.contains("combat") && wire["activity"] == nlohmann::json{{"kind", "casting"}},
          "state: no target outside combat");
    state.inCombat = true;
    state.casting = false;
    wire = StateWire(state);
    Check(wire["combat"]["target"]["name"] == std::string(63, 'a'), "state: a cut never splits a UTF-8 sequence");
    Check(!wire["group"].contains("follows"), "state: a blank name is omitted");
    state.inCombat = false;
    wire = StateWire(state);
    Check(wire["activity"] == nlohmann::json{{"kind", "questing"}}, "state: an invalid UTF-8 quest title is omitted");

    // The partner filter.
    state = FullState();
    state.partnerInGroup = false;
    state.partnerVisible = false;
    Check(!StateWire(state).contains("partner"), "state: partner absent when neither grouped nor visible");
    state.partnerVisible = true;
    Check(StateWire(state).contains("partner"), "state: a visible partner outside the group is known");
    state = FullState();
    state.partnerOnline = false;
    Check(!StateWire(state).contains("partner"), "state: an offline partner is absent");
    state = FullState();
    state.partnerVisible = false;
    state.partnerSameMap = false;
    state.partnerZone = "Stormwind City";
    Check(StateWire(state)["partner"] == nlohmann::json{{"zone", "Stormwind City"}},
          "state: a grouped partner on another map gives the zone only");

    // The combat filter: own victim, visible, alive and fighting.
    for (int drop = 0; drop < 4; ++drop)
    {
        state = FullState();
        (drop == 0 ? state.victim : drop == 1 ? state.victimVisible : drop == 2 ? state.inCombat : state.alive) = false;
        Check(!StateWire(state).contains("combat"), "state: combat target only the bot's own visible victim");
    }

    // Activity priority: dead idle; flying > fighting > casting > looting > resting > questing > travelling > idle.
    state = FullState();
    state.flying = state.casting = state.looting = state.resting = state.working = true;
    Check(StateActivityKind(state) == "flying", "state: flying first");
    state.flying = false;
    Check(StateActivityKind(state) == "fighting", "state: fighting before casting");
    state.inCombat = false;
    Check(StateActivityKind(state) == "casting", "state: casting before looting");
    state.casting = false;
    Check(StateActivityKind(state) == "looting", "state: looting before resting");
    state.looting = false;
    Check(StateActivityKind(state) == "resting", "state: resting before questing");
    state.resting = false;
    Check(StateWire(state)["activity"] == nlohmann::json({{"kind", "questing"}, {"detail", "The Defias Brotherhood"}}),
          "state: questing names the quest");
    state.travelling = false;
    Check(StateActivityKind(state) == "questing", "state: working at a quest destination is questing");
    state.working = false;
    Check(StateActivityKind(state) == "idle", "state: nothing to do is idle");
    state.travelling = true;
    state.questTitle.clear();
    Check(StateWire(state)["activity"] == nlohmann::json{{"kind", "travelling"}}, "state: travelling without a quest");
    state.alive = false;
    state.flying = true;
    Check(StateActivityKind(state) == "idle" && StateWire(state)["alive"] == "dead", "state: the dead are idle");
    state.ghost = true;
    Check(StateWire(state)["alive"] == "ghost", "state: a ghost");

    // Movement priority, plans, power kinds, optional keys.
    state = FullState();
    state.stay = state.guard = state.grind = state.passive = true;
    Check(StateWire(state)["movement"] == "stay", "state: stay first");
    state.stay = false;
    Check(StateWire(state)["movement"] == "follow", "state: follow before guard");
    state.follow = false;
    Check(StateWire(state)["movement"] == "guard", "state: guard before grind");
    state.guard = false;
    Check(StateWire(state)["movement"] == "grind", "state: grind before passive");
    state.grind = false;
    Check(StateWire(state)["movement"] == "passive", "state: passive");
    state.passive = false;
    Check(StateWire(state)["movement"] == "none", "state: no movement strategy");
    // LCB7b (LC.36): each lasting setting in force is named while it holds, in the hub's plan order, and is gone once
    // the bot reverts it; follow and stay stay movement.
    Check(!StateWire(state).contains("plans"), "state: no plans key without a lasting setting in force");
    using Setting = bool PlayerbotBridge::BotStateFacts::*;
    for (auto const& [setting, word] : std::initializer_list<std::pair<Setting, char const*>>{
             {&PlayerbotBridge::BotStateFacts::formation, "formation"},
             {&PlayerbotBridge::BotStateFacts::saveMana, "save_mana"},
             {&PlayerbotBridge::BotStateFacts::focusHeal, "focus_healing"}})
    {
        state.*setting = true;
        Check(StateWire(state)["plans"] == nlohmann::json::array({word}),
              "state: a lasting setting named while it holds");
        state.*setting = false;
        Check(!StateWire(state).contains("plans"), "state: a reverted lasting setting is gone");
    }
    state.formation = state.saveMana = state.focusHeal = state.stay = true;
    Check(StateWire(state)["plans"] == nlohmann::json::array({"formation", "save_mana", "focus_healing"}) &&
              StateWire(state)["movement"] == "stay",
          "state: every lasting setting in the plan order; stay stays movement");
    state.saveMana = false;
    Check(StateWire(state)["plans"] == nlohmann::json::array({"formation", "focus_healing"}),
          "state: reverting one setting leaves the others named");
    state.formation = state.focusHeal = state.stay = false;
    for (auto const& [type, kind] : std::initializer_list<std::pair<uint8_t, char const*>>{
             {0, "mana"}, {1, "rage"}, {2, "focus"}, {3, "energy"}, {6, "runic"}})
    {
        state.powerType = type;
        Check(StateWire(state)["power"]["kind"] == kind, "state: power kinds");
    }
    state.powerType = 4;
    Check(!StateWire(state).contains("power"), "state: other power types omitted");
    state.powerType = 1;
    state.powerMax = 0;
    Check(!StateWire(state).contains("power"), "state: no maximum, no power");
    state = FullState();
    state.area = state.zone;
    state.mounted = state.swimming = false;
    state.groupSize = 0;
    wire = StateWire(state);
    Check(!wire["where"].contains("area") && !wire.contains("mounted") && !wire.contains("swimming") &&
              !wire.contains("group"),
          "state: area equal to zone, false flags and no group omitted");

    // Size over fixtures (recorded in the step's Log; the brief estimated 380-450 bytes).
    std::vector<std::size_t> sizes;
    state = FullState();
    sizes.push_back(StateWire(state).dump().size());  // Grouped, fighting, everything set.
    state.inCombat = false;
    state.mounted = state.swimming = false;
    sizes.push_back(StateWire(state).dump().size());  // Grouped, questing.
    state.partnerInGroup = state.partnerVisible = false;
    state.groupSize = 0;
    state.follow = false;
    sizes.push_back(StateWire(state).dump().size());  // Solo, partner unknown.
    state.travelling = false;
    state.questTitle.clear();
    state.area = state.zone;
    sizes.push_back(StateWire(state).dump().size());  // Solo, idle.
    state = FullState();
    state.inCombat = false;
    state.resting = true;
    state.zone = "Stormwind City";
    state.area = "Trade District";
    sizes.push_back(StateWire(state).dump().size());  // Grouped, resting in a city.
    state = FullState();
    state.victimName = state.questTitle = state.zone = state.area = state.partnerZone = state.masterName =
        std::string(80, 'w');
    state.money = 4294967295u;
    std::size_t const worst = StateWire(state).dump().size();
    std::sort(sizes.begin(), sizes.end());
    std::cout << "bridge-state-selftest: input.state bytes p50 " << sizes[sizes.size() / 2] << ", max " << sizes.back()
              << " over " << sizes.size() << " fixtures; worst case (all strings at the cap) " << worst << '\n';
    Check(worst <= 1024, "state: worst case stays within 1 KiB");
}

// J6e-c (DS.14 (c)): `input.wake` is exactly {condition, watchId} and only on a wake; the chain's depth and origin
// stay in the module. ActivityCode follows the state's activity kind (the watch compares codes).
void WakeChecks()
{
    using namespace OllamaBridge;
    auto request = Request();
    std::string body;
    Check(Build(Settings(), request, {1000000, 10000}, body) && !nlohmann::json::parse(body)["input"].contains("wake"),
          "wake: absent on a chat line");
    request.input.wakeCondition = "combat_end";
    request.input.wakeWatchId = "watch-7";
    request.input.wakeDepth = 2;
    request.input.originUnixMs = 987654321;
    Check(Build(Settings(), request, {1000000, 10000}, body), "wake: build");
    auto const input = nlohmann::json::parse(body)["input"];
    Check(input.contains("wake") &&
              input["wake"] == nlohmann::json({{"condition", "combat_end"}, {"watchId", "watch-7"}}),
          "wake: exactly condition and watch id");
    Check(body.find("987654321") == std::string::npos && !input.contains("wakeDepth") && !input.contains("depth"),
          "wake: depth and origin never on the wire");

    auto const code = [](PlayerbotBridge::BotStateFacts const& state) { return static_cast<int>(ActivityCode(state)); };
    auto state = FullState();
    Check(code(state) == 2, "activity code: fighting 2");
    state.flying = true;
    Check(code(state) == 1, "activity code: flying 1");
    state = FullState();
    state.alive = false;
    Check(code(state) == 0, "activity code: dead is idle 0");
    state = FullState();
    state.inCombat = false;
    Check(code(state) == 6, "activity code: questing 6");
    state.casting = true;
    Check(code(state) == 3, "activity code: casting 3");
    state.casting = false;
    state.looting = true;
    Check(code(state) == 4, "activity code: looting 4");
    state.looting = false;
    state.resting = true;
    Check(code(state) == 5, "activity code: resting 5");
    state.resting = false;
    state.questTitle.clear();
    Check(code(state) == 7, "activity code: travelling 7");
    state.travelling = false;
    Check(code(state) == 0, "activity code: idle 0");
}

// LC10: `input.facts`, `input.traits` and the state's around, servant, mount and deeds exactly as Build encodes them.
// Absent fields leave the request as before; a deed is a whole sentence of at most 200 bytes (LC7's cap), never the
// 64-byte fact rule; every list stops at its wire cap; an empty list or traits object is left out.
nlohmann::json InputWire(OllamaBridge::Request const& request)
{
    std::string body;
    Check(OllamaBridge::Build(Settings(), request, {1000000, 10000}, body), "lc10: build");
    return nlohmann::json::parse(body)["input"];
}

// Who the bot is (the adapter's getters) beside its lasting facts (playerbots' reader), as one request carries them.
struct Facts
{
    OllamaBridge::BotWho who;
    PlayerbotBridge::BotFacts lasting;
};

void PutFacts(OllamaBridge::Request& request, Facts const& facts)
{
    request.input.who = facts.who;
    request.input.facts = facts.lasting;
}

Facts FullFacts()
{
    Facts facts;
    facts.who = {"Thorgrim", "Dwarf", "Warrior", "male", 42, 0};
    facts.lasting.phrase = "fights with a great axe in both hands";
    facts.lasting.crafts = {{"Mining", "expert"}, {"Blacksmithing", "journeyman"}};
    facts.lasting.makes = {{"Blacksmithing", {"Rough Sharpening Stone"}}};
    facts.lasting.standing = {{"Ironforge", "honoured"}};
    facts.lasting.guild = "Stonehearth";
    return facts;
}

void FactsChecks()
{
    using namespace OllamaBridge;
    std::string const oldBody = []
    {
        std::string body;
        Check(Build(Settings(), Request(), {1000000, 10000}, body), "lc10: old build");
        return body;
    }();
    auto const oldInput = nlohmann::json::parse(oldBody)["input"];
    std::vector<std::string> keys;
    for (auto const& item : oldInput.items())
        keys.push_back(item.key());
    Check(keys == std::vector<std::string>{"channel", "directAddress", "groupScope", "memory", "observations", "party",
                                           "speakerControlsBot", "speakerGuid", "text"},
          "lc10: without the new fields the input has exactly the old keys");
    auto request = Request();
    request.input.facts = PlayerbotBridge::BotFacts();  // Present but without who or phrase: left out whole.
    request.input.traits.words = {"", " \t", "\x01"};
    request.input.traits.interests = {"\xFF"};
    std::string body;
    Check(Build(Settings(), request, {1000000, 10000}, body) && body == oldBody,
          "lc10: facts without who, blank and invalid traits leave the request byte-identical");
    request = Request();
    auto state = FullState();
    request.input.state = state;
    Check(Build(Settings(), request, {1000000, 10000}, body), "lc10: old state build");
    std::string const oldStateBody = body;
    state.around = {Around("", AroundKind::Hostile, "", 1), Around("Wolf", static_cast<AroundKind>(9), "", 1)};
    state.servantName = state.mountName = " ";
    state.deeds = {"", "\xC3"};
    request.input.state = state;
    Check(Build(Settings(), request, {1000000, 10000}, body) && body == oldStateBody,
          "lc10: state lists without a valid entry are left out (old bytes)");

    // Facts: who, phrase, the lists in the reader's order; empty lists and no guild left out.
    request = Request();
    PutFacts(request, FullFacts());
    Check(InputWire(request)["facts"] == nlohmann::json::parse(R"({"who":{"name":"Thorgrim","race":"Dwarf",
        "class":"Warrior","gender":"male","level":42,"side":"Alliance"},
        "phrase":"fights with a great axe in both hands",
        "crafts":[{"name":"Mining","rank":"expert"},{"name":"Blacksmithing","rank":"journeyman"}],
        "makes":[{"craft":"Blacksmithing","items":["Rough Sharpening Stone"]}],"standing":[{"faction":"Ironforge","word":"honoured"}],"guild":"Stonehearth"})"),
          "lc10: facts encoded with their keys, empty lists left out");
    auto const dropped = [&request](auto&& spoil)
    {
        auto facts = FullFacts();
        spoil(facts);
        PutFacts(request, facts);
        return !InputWire(request).contains("facts");
    };
    Check(dropped([](Facts& f) { f.lasting.phrase = "  "; }) && dropped([](Facts& f) { f.who.name.clear(); }) &&
              dropped([](Facts& f) { f.who.race = "\x01"; }) && dropped([](Facts& f) { f.who.className = "\xFF"; }) &&
              dropped([](Facts& f) { f.who.gender = "other"; }) && dropped([](Facts& f) { f.who.level = 0; }) &&
              dropped([](Facts& f) { f.who.team = 2; }),
          "lc10: facts left out whole when who or the phrase fails its rule");
    auto facts = FullFacts();
    facts.who.level = 300;
    facts.lasting.guild.clear();
    facts.lasting.phrase = std::string(96, 'p');
    facts.lasting.crafts = {{"Mining", ""}, {"", "expert"}, {"Herbalism", std::string(40, 'r')}};
    facts.lasting.standing.clear();
    facts.lasting.makes.assign(9, {"Cooking", {"a", "b", "c", "d", "e"}});
    facts.lasting.makes.push_back({"", {"x"}});
    facts.lasting.servants.assign(9, "Grimtooth");
    facts.lasting.relatives = {"Brann", " "};
    PutFacts(request, facts);
    auto wire = InputWire(request)["facts"];
    Check(wire["who"]["level"] == 255 && !wire.contains("guild") && !wire.contains("standing") &&
              wire["phrase"] == std::string(96, 'p') &&
              wire["crafts"] ==
                  nlohmann::json::parse(R"([{"name":"Herbalism","rank":"rrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr"}])") &&
              wire["makes"].size() == 8 && wire["makes"][0]["items"].size() == 4 && wire["servants"].size() == 8 &&
              wire["relatives"] == nlohmann::json::array({"Brann"}),
          "lc10: level capped, pairs need both texts (word cut at 32), lists at their caps, blanks dropped");
    facts = FullFacts();
    facts.lasting.makes.clear();
    PutFacts(request, facts);
    Check(InputWire(request)["facts"].contains("crafts") && !InputWire(request)["facts"].contains("makes") &&
              !InputWire(request)["facts"].contains("useful"),
          "lc18f: crafts known and no recipe useful: the craft without makes, and no useful key any more");
    facts.lasting.makes = {{"Cooking", {"Boiled Clams", " "}}, {"Alchemy", {}}, {"Tailoring", {" "}}};
    PutFacts(request, facts);
    Check(InputWire(request)["facts"]["makes"] ==
              nlohmann::json::parse(R"([{"craft":"Cooking","items":["Boiled Clams"]}])"),
          "lc18f: an item without text, a craft without items are dropped");
    facts.lasting.phrase = std::string(97, 'p');
    PutFacts(request, facts);
    Check(InputWire(request)["facts"]["phrase"] == std::string(96, 'p'), "lc10: phrase cut at 96 bytes");

    // LC18e: visits in the reader's order, land only when given, an entry without its name or kind dropped, at most 24.
    facts = FullFacts();
    facts.lasting.visits = {{"The Deadmines", "dungeon", "Eastern Kingdoms", 60},
                            {"Eastern Kingdoms", "continent", "", 3600},
                            {"", "zone", "Kalimdor", 1},
                            {"Elwynn Forest", " ", "Eastern Kingdoms", 1}};
    PutFacts(request, facts);
    Check(InputWire(request)["facts"]["visits"] == nlohmann::json::parse(R"([
        {"name":"The Deadmines","kind":"dungeon","land":"Eastern Kingdoms","ago":60},
        {"name":"Eastern Kingdoms","kind":"continent","ago":3600}])"),
          "lc18e: visits with name, kind, land when given and ago; an entry without name or kind dropped");
    facts.lasting.visits.assign(25, {"Westfall", "zone", std::string(65, 'l'), 4294967295u});
    PutFacts(request, facts);
    wire = InputWire(request)["facts"];
    Check(wire["visits"].size() == 24 && wire["visits"][0]["land"] == std::string(64, 'l') &&
              wire["visits"][0]["ago"] == 4294967295u,
          "lc18e: visits at most 24, land cut at 64 bytes, ago whole");
    facts.lasting.visits.assign(2, {" ", "zone", "", 5});
    PutFacts(request, facts);
    Check(!InputWire(request)["facts"].contains("visits"), "lc18e: no visits key without a valid entry");

    // Traits: each word whole within 1024 bytes (never cut at 64), 32 words, 3 interests; one list alone is fine.
    request = Request();
    std::string const longWord = std::string(1023, 'w') + "\xC3\xA9";  // 1025 bytes, byte 1024 inside a character.
    request.input.traits.words = {"very careful", std::string(100, 'c'), std::string(1024, 'k'), longWord};
    for (int i = 0; i < 30; ++i)
        request.input.traits.words.push_back("rather chatty");
    request.input.traits.interests = {"fishing", "inns and ale", "old maps", "dancing"};
    wire = InputWire(request)["traits"];
    Check(wire.size() == 2 && wire["words"].size() == 32 && wire["words"][0] == "very careful" &&
              wire["words"][1] == std::string(100, 'c') && wire["words"][2] == std::string(1024, 'k') &&
              wire["words"][3] == std::string(1023, 'w') &&
              wire["interests"] == nlohmann::json({"fishing", "inns and ale", "old maps"}),
          "lc10: trait words whole to 1024 bytes, cut on a character boundary past it, lists at their caps");
    request.input.traits.words.clear();
    wire = InputWire(request);
    Check(wire.contains("traits") && !wire["traits"].contains("words") && wire["traits"]["interests"].size() == 3,
          "lc10: traits with interests only");
    request.input.traits = {};
    Check(!InputWire(request).contains("traits"), "lc10: traits left out when both lists are empty");

    // LC15c: areas beside words, one per word; a dropped word takes its area along; no areas key unless every kept
    // word has one.
    request.input.traits.words = {"rather calm", " \t", "very kind"};
    request.input.traits.areas = {"work", "talk", "others"};
    wire = InputWire(request)["traits"];
    Check(wire["words"] == nlohmann::json({"rather calm", "very kind"}) &&
              wire["areas"] == nlohmann::json({"work", "others"}),
          "lc15c: areas beside words, a dropped word's area dropped with it");
    request.input.traits.areas = {"work", "talk"};
    wire = InputWire(request)["traits"];
    Check(wire["words"].size() == 2 && !wire.contains("areas"), "lc15c: no areas key when not one per word");
    request.input.traits.areas = {"work", "talk", " \t"};
    wire = InputWire(request)["traits"];
    Check(wire["words"].size() == 2 && !wire.contains("areas"), "lc15c: no areas key when a kept word's area is blank");
    request.input.traits = {};

    // State: around (nearest first, one per name, known kinds, count 1-255), servant and mount (64-byte rule),
    // deeds (whole sentences to 200 bytes, newest first, at most 3; a blank or invalid one dropped, not the state).
    request = Request();
    state = FullState();
    state.around = {Around("Defias Pillager", AroundKind::Hostile, "", 2),
                    Around("Wolf", AroundKind::Hostile, "", 0),
                    Around("Wolf\x01", AroundKind::Hostile, "", 1),
                    Around("Innkeeper Farley", AroundKind::Service, "innkeeper", 300),
                    Around("Strange Rock", static_cast<AroundKind>(9), "", 1),
                    Around("\xFF", AroundKind::Object, "", 1),
                    Around(std::string(70, 'n'), AroundKind::Person, std::string(70, 'd'), 1)};
    for (int i = 0; i < 12; ++i)
        state.around.push_back(Around("Kobold " + std::to_string(i), AroundKind::Hostile, "", 1));
    state.servantName = std::string(64, 's');
    state.mountName = std::string(65, 'm');
    std::string const deed200 = std::string(199, 'd') + ".";
    std::string const deed201 = std::string(199, 'e') + "\xC3\xA9";  // Byte 200 inside a character.
    state.deeds = {deed200, " ", deed201, "\xC3\x28", std::string(201, 'f'), "A fourth deed."};
    request.input.state = state;
    auto const stateWire = InputWire(request)["state"];
    auto const& around = stateWire["around"];
    Check(around.size() == 12 &&
              around[0] == nlohmann::json({{"name", "Defias Pillager"}, {"kind", "hostile"}, {"count", 2}}) &&
              around[1] == nlohmann::json({{"name", "Wolf"}, {"kind", "hostile"}, {"count", 1}}) &&
              around[2] ==
                  nlohmann::json(
                      {{"name", "Innkeeper Farley"}, {"kind", "service"}, {"detail", "innkeeper"}, {"count", 255}}) &&
              around[3] == nlohmann::json({{"name", std::string(64, 'n')},
                                           {"kind", "person"},
                                           {"detail", std::string(64, 'd')},
                                           {"count", 1}}) &&
              around[4]["name"] == "Kobold 0" && around[11]["name"] == "Kobold 7",
          "lc10: around in order, one per name after the bounds, known kinds only, count 1-255, at most 12");
    Check(stateWire["servant"] == std::string(64, 's') && stateWire["mount"] == std::string(64, 'm'),
          "lc10: servant and mount keep the 64-byte rule (64 unchanged, 65 cut)");
    Check(stateWire["deeds"] == nlohmann::json({deed200, std::string(199, 'e'), std::string(200, 'f')}),
          "lc10: deeds whole to 200 bytes, cut on a character boundary past it, blank and invalid dropped, at most 3");
    Check(stateWire["money"] == 1234567 && stateWire["where"]["zone"] == "Westfall",
          "lc10: the old state keys are unchanged beside the new ones");

    // Size: every new field at its cap (traits at 1024 bytes each) on the fixture request stays under the bound.
    request = Request();
    Facts cap;
    cap.who = {std::string(64, 'x'), std::string(64, 'x'), std::string(64, 'x'), "female", 80, 1};
    cap.lasting.guild = std::string(64, 'x');
    cap.lasting.phrase = std::string(96, 'p');
    cap.lasting.crafts.assign(8, {std::string(64, 'c'), std::string(32, 'r')});
    cap.lasting.makes.assign(8, {std::string(64, 'm'), std::vector<std::string>(4, std::string(64, 'u'))});
    cap.lasting.servants.assign(8, std::string(64, 's'));
    cap.lasting.standing.assign(6, {std::string(64, 'f'), std::string(32, 'w')});
    cap.lasting.relatives.assign(8, std::string(64, 'k'));
    cap.lasting.visits.assign(24, {std::string(64, 'n'), "continent", std::string(64, 'l'), 4294967295u});
    PutFacts(request, cap);
    request.input.traits.words.assign(32, "extremely careful");
    request.input.traits.interests.assign(3, "inns and ale");
    state = FullState();
    state.around.clear();
    // LCB4: every object with its eight open rows at the bounds (24-byte keys, 64-byte texts).
    std::vector<PlayerbotBridge::StateKey> openCap;
    for (char key = 'a'; key < 'i'; ++key)
        openCap.push_back({std::string(24, key), std::string(64, 'o')});
    for (int i = 0; i < 12; ++i)
    {
        state.around.push_back(
            Around(std::string(60, 'a') + std::to_string(1000 + i), AroundKind::Hostile, std::string(64, 'd'), 255));
        state.around.back().open = openCap;
    }
    state.open = openCap;
    state.servantName = state.mountName = std::string(64, 'v');
    state.deeds.assign(3, std::string(200, 'z'));
    request.input.state = state;
    Check(Build(Settings(), request, {1000000, 10000}, body), "lc10: realistic traits build");
    std::size_t const realistic = body.size();
    auto const capped = nlohmann::json::parse(body)["input"];
    Check(capped["state"]["around"].size() == 12 && capped["facts"]["crafts"].size() == 8 &&
              capped["facts"]["visits"].size() == 24 &&
              capped["state"]["deeds"].size() == 3 && capped["state"].contains(std::string(24, 'h')) &&
              capped["state"]["around"][11].contains(std::string(24, 'h')),
          "lc10: every list at its cap is sent whole");
    request.input.traits.words.assign(32, std::string(1024, 't'));
    request.input.traits.interests.assign(3, std::string(1024, 'i'));
    Check(Build(Settings(), request, {1000000, 10000}, body) && body.size() < MaxRequestBytes,
          "lc10: every new field at its cap stays under MaxRequestBytes");
    std::cout << "bridge-state-selftest: lc10 request bytes: old " << oldBody.size() << ", every new field at cap with "
              << "realistic traits " << realistic << ", with every trait at 1024 bytes " << body.size() << '\n';
}

// LCB4: a key playerbots adds as an open row reaches the wire with no module edit, on the state and on each around
// entry, beside unchanged shaped keys. A row is dropped alone when its key is not [a-z][a-z_]{0,23}, is a key the
// envelope writes itself or repeats a kept one, when its text does not survive the fact rule, or past the eighth
// kept row of its object; numbers are clamped to +-1e9 and text follows the fact rule.
void OpenKeyChecks()
{
    using PlayerbotBridge::StateKey;
    auto state = FullState();
    state.around = {Around("Wolf", AroundKind::Hostile, "", 2)};
    auto const shaped = StateWire(state);
    state.open.push_back({"probe", true});
    state.around[0].open.push_back({"type", "beast"});
    auto wire = StateWire(state);
    Check(wire["probe"] == true && wire["around"][0]["type"] == "beast",
          "open: a row added in playerbots reaches the state and an around entry");
    wire.erase("probe");
    wire["around"][0].erase("type");
    Check(wire == shaped, "open: the shaped keys are unchanged beside the open rows");

    state = FullState();
    state.open = {{"Bad", true},
                  {"9lives", true},
                  {"has-dash", true},
                  {std::string(25, 'k'), true},
                  {"", true},
                  {"health", 5},
                  {"around", true},
                  {"deeds", "x"},
                  {"blank", " \t"},
                  {"broken", "\xC3\x28"},
                  {"first", 1},
                  {"first", 2},
                  {"big", int64_t{5000000000}},
                  {"small", int64_t{-5000000000}},
                  {"text", "a\x01" + std::string(70, 't')},
                  {"flag", false},
                  {std::string(24, 'k'), "keep"},
                  {"under_score", 7},
                  {"eighth", "last"},
                  {"ninth", true}};
    state.around = {Around("Wolf", AroundKind::Hostile, "", 2)};
    state.around[0].open = {{"name", "x"}, {"count", 3}, {"kind", "y"}, {"detail", "z"}, {"type", "beast"}};
    for (int i = 0; i < 8; ++i)
        state.around[0].open.push_back({"extra_" + std::string(1, static_cast<char>('a' + i)), i});
    wire = StateWire(state);
    nlohmann::json open = nlohmann::json::object();
    for (auto const& [key, value] : wire.items())
        if (!shaped.contains(key))
            open[key] = value;
    Check(open == nlohmann::json({{"first", 1},
                                  {"big", 1000000000},
                                  {"small", -1000000000},
                                  {"text", "a" + std::string(63, 't')},
                                  {"flag", false},
                                  {std::string(24, 'k'), "keep"},
                                  {"under_score", 7},
                                  {"eighth", "last"}}),
          "open: bad keys, shaped keys, repeats, blank and invalid text and the ninth row dropped; bounds kept");
    Check(wire["health"] == 73 && !wire.contains("deeds") && wire["around"][0]["name"] == "Wolf" &&
              wire["around"][0]["count"] == 2 && wire["around"][0]["kind"] == "hostile" &&
              !wire["around"][0].contains("detail"),
          "open: a row never replaces a shaped key");
    auto const& entry = wire["around"][0];
    Check(entry.size() == 3 + 8 && entry["type"] == "beast" && entry.contains("extra_g") && !entry.contains("extra_h"),
          "open: an around entry keeps its own eight rows");
}
}  // namespace

int main()
{
    try
    {
        StateChecks();
        WakeChecks();
        FactsChecks();
        OpenKeyChecks();
        std::cout << "bridge-state-selftest: PASS " << checks << " state key checks\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "bridge-state-selftest: FAIL " << error.what() << '\n';
        return 1;
    }
}
