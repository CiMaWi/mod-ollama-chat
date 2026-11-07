// J5e-crash: checks the production BridgeCardFacts (handler.cpp, `// BEGIN/END BRIDGE CARD FACTS`, extracted verbatim
// into card-facts.inc by the runner) against fake world types. The J17Cc live round crashed the world thread on a
// guildless bot's first card offer: Player::GetGuildName() returns sGuildMgr->GetGuildById(GetGuildId())->GetName()
// without a null check. The fake GetGuildName reproduces that hazard as an exception instead of undefined behaviour.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "mod-ollama-chat_bridge.h"

using uint8 = std::uint8_t;

enum Gender : uint8
{
    GENDER_MALE = 0,
    GENDER_FEMALE = 1
};

enum TeamId : uint8
{
    TEAM_ALLIANCE = 0,
    TEAM_HORDE = 1
};

struct AreaTableEntry
{
    char const* name;
};

struct ObjectGuid
{
    std::uint64_t raw = 0;
    std::uint64_t GetRawValue() const { return raw; }
};

struct Guild
{
    std::string name;
    std::string const& GetName() const { return name; }
};

struct Player
{
    std::string name;
    uint8 race = 4;
    uint8 cls = 11;
    Gender gender = GENDER_MALE;
    std::uint32_t level = 1;
    TeamId team = TEAM_ALLIANCE;
    std::uint32_t guildId = 0;
    Guild* guild = nullptr;  // What sGuildMgr->GetGuildById(guildId) finds: null without an id or for a gone guild.
    ObjectGuid guid;
    int guildNameCalls = 0;

    std::string const& GetName() const { return name; }
    uint8 getRace() const { return race; }
    uint8 getClass() const { return cls; }
    Gender getGender() const { return gender; }
    std::uint32_t GetLevel() const { return level; }
    TeamId GetTeamId() const { return team; }
    ObjectGuid GetGUID() const { return guid; }
    // Core Player::GetGuild: guildId ? sGuildMgr->GetGuildById(guildId) : nullptr.
    Guild* GetGuild() const { return guildId ? guild : nullptr; }
    // Core Player::GetGuildName: sGuildMgr->GetGuildById(GetGuildId())->GetName(), unchecked.
    std::string const& GetGuildName()
    {
        ++guildNameCalls;
        if (!guild)
            throw std::logic_error("Player::GetGuildName dereferences a null Guild");
        return guild->GetName();
    }
};

struct PlayerbotAI
{
    AreaTableEntry const* zone = nullptr;
    AreaTableEntry const* GetCurrentZone() { return zone; }
    static std::string GetLocalizedAreaName(AreaTableEntry const* entry) { return entry ? entry->name : ""; }
};

struct PlayerbotsMgr
{
    std::unordered_map<Player*, PlayerbotAI*> ais;
    static PlayerbotsMgr& instance()
    {
        static PlayerbotsMgr mgr;
        return mgr;
    }
    PlayerbotAI* GetPlayerbotAI(Player* player)
    {
        auto const it = ais.find(player);
        return it == ais.end() ? nullptr : it->second;
    }
};

std::string FormatPlayerRace(uint8 race) { return race == 4 ? "Night Elf" : race == 11 ? "Draenei" : "Unknown"; }
std::string FormatPlayerClass(uint8 cls) { return cls == 11 ? "Druid" : cls == 2 ? "Paladin" : "Unknown"; }

bool g_EnableRPPersonalities = true;
std::unordered_map<std::uint64_t, std::string> g_BotPersonalityList;

#include "card-facts.inc"

namespace
{
int g_checks = 0;

void Check(bool ok, char const* label)
{
    ++g_checks;
    if (ok)
        return;
    std::printf("FAIL: %s\n", label);
    std::exit(1);
}

// Runs the production function; a throw is the reproduced world-thread crash.
bool Facts(Player& bot, OllamaBridge::CardFacts& facts, char const* label)
{
    try
    {
        BridgeCardFacts(&bot, facts);
        return true;
    }
    catch (std::exception const& e)
    {
        std::printf("FAIL: %s: %s\n", label, e.what());
        std::exit(1);
    }
}
}  // namespace

int main()
{
    AreaTableEntry const ashenvale{"Ashenvale"};
    PlayerbotAI ai;
    ai.zone = &ashenvale;

    // 1. The J17Cc crash fixture: Branlorne (guid 922), night elf druid 46, Alliance, no guild row.
    Player branlorne;
    branlorne.name = "Branlorne";
    branlorne.level = 46;
    branlorne.guid.raw = 922;
    PlayerbotsMgr::instance().ais[&branlorne] = &ai;
    OllamaBridge::CardFacts facts;
    Facts(branlorne, facts, "a guildless bot's card facts");
    Check(branlorne.guildNameCalls == 0, "the unchecked Player::GetGuildName is never called");
    Check(facts.guild.empty(), "a guildless bot has no guild");
    Check(facts.name == "Branlorne" && facts.race == "Night Elf" && facts.className == "Druid", "name, race, class");
    Check(facts.gender == "male" && facts.level == 46 && facts.faction == "Alliance", "gender, level, faction");
    Check(facts.zone == "Ashenvale", "zone from the bot's AI");

    // 2. A guild id whose guild is gone (disbanded): GetGuildById finds nothing, the core accessor would crash too.
    Player orphan = branlorne;
    orphan.guildId = 77;
    orphan.guildNameCalls = 0;
    OllamaBridge::CardFacts orphanFacts;
    Facts(orphan, orphanFacts, "a gone guild's card facts");
    Check(orphan.guildNameCalls == 0 && orphanFacts.guild.empty(), "a gone guild leaves the guild empty");

    // 3. A bot in a guild gets the guild's name; Horde and female map as before; no AI leaves the zone empty.
    Guild guild{"Wardens of the Grove"};
    Player avenomah;
    avenomah.name = "Avenomah";
    avenomah.race = 11;
    avenomah.cls = 2;
    avenomah.gender = GENDER_FEMALE;
    avenomah.level = 80;
    avenomah.team = TEAM_HORDE;
    avenomah.guildId = 21;
    avenomah.guild = &guild;
    avenomah.guid.raw = 2144;
    OllamaBridge::CardFacts guildFacts;
    Facts(avenomah, guildFacts, "a guild bot's card facts");
    Check(guildFacts.guild == "Wardens of the Grove", "a guild bot carries its guild name");
    Check(guildFacts.gender == "female" && guildFacts.faction == "Horde" && guildFacts.level == 80,
          "female, Horde, level 80");
    Check(guildFacts.race == "Draenei" && guildFacts.className == "Paladin", "race and class of the second bot");
    Check(guildFacts.zone.empty(), "no bot AI leaves the zone empty");

    // 4. Traits only from an existing, non-default assignment while RP personalities are on.
    g_BotPersonalityList[922] = "default";
    OllamaBridge::CardFacts defaultFacts;
    Facts(branlorne, defaultFacts, "a default personality's card facts");
    Check(defaultFacts.traits.empty(), "the default personality gives no trait word");
    g_EnableRPPersonalities = false;
    g_BotPersonalityList[922] = "GRUMPY_VETERAN";
    OllamaBridge::CardFacts offFacts;
    Facts(branlorne, offFacts, "personalities off");
    Check(offFacts.traits.empty(), "RP personalities off give no trait word");

    std::printf("PASS: %d checks passed\n", g_checks);
    return 0;
}
