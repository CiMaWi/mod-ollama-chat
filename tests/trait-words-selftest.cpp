// WT11b: checks the trait word formatters (src/mod-ollama-chat_trait_words.h) and prints the sample of 20 personalities
// as the LLM sees them (mod-playerbots TraitRules.h, guids 1001-1020, same bots as the trait selftest sample).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "TraitRules.h"
#include "mod-ollama-chat_trait_words.h"

using OllamaTraitWords::FormatCardTraits;
using OllamaTraitWords::FormatPersonalityPrompt;
using Words = std::vector<std::string>;

namespace
{
int g_checks = 0;

void Check(bool ok, char const* what)
{
    ++g_checks;
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        std::exit(1);
    }
}

bool HasDigit(std::string const& text)
{
    for (char c : text)
        if (c >= '0' && c <= '9')
            return true;
    return false;
}

std::string_view RaceName(uint8_t race)
{
    constexpr std::string_view names[] = {"",       "Human", "Orc",   "Dwarf", "Night Elf", "Forsaken",
                                          "Tauren", "Gnome", "Troll", "",      "Blood Elf", "Draenei"};
    return race < std::size(names) ? names[race] : "";
}

std::string_view ClassName(uint8_t cls)
{
    constexpr std::string_view names[] = {"",       "Warrior", "Paladin", "Hunter", "Rogue", "Priest", "Death Knight",
                                          "Shaman", "Mage",    "Warlock", "",       "Druid"};
    return cls < std::size(names) ? names[cls] : "";
}

void CheckFormatters()
{
    Words const words = {"very careful", "deeply devout", "rather bookish", "rather suspicious"};
    Words const interests = {"fishing", "inns and ale", "lore"};
    Check(FormatPersonalityPrompt(words, interests) ==
              "very careful, deeply devout, rather bookish, rather suspicious; fond of fishing, inns and ale and lore",
          "words and interests");
    Check(FormatPersonalityPrompt(words, {"fishing", "lore"}) ==
              "very careful, deeply devout, rather bookish, rather suspicious; fond of fishing and lore",
          "two interests");
    Check(FormatPersonalityPrompt(words, {}) == "very careful, deeply devout, rather bookish, rather suspicious",
          "only words");
    Check(FormatPersonalityPrompt({}, {"fishing"}) == "fond of fishing", "only interests");
    Check(FormatPersonalityPrompt({}, {}).empty(), "empty input");
    Check(FormatPersonalityPrompt({"very  careful\n", " "}, {}) == "very careful", "controls and double spaces");

    Check(FormatCardTraits(words) == "careful, devout, bookish", "three words without adverbs");
    Check(FormatCardTraits({"a loner", "fiercely loyal"}) == "loner, loyal", "article and adverb stripped");
    Check(FormatCardTraits({"very careful"}) == "careful", "one word");
    Check(FormatCardTraits({}).empty(), "empty card words");
    std::string const longWord(30, 'x');
    std::string const cut = FormatCardTraits({"very " + longWord, "very " + longWord, "very " + longWord});
    Check(cut == longWord + ", " + longWord, "cut at a word boundary under 64 bytes");
    Check(cut.size() <= 64, "64 bytes");
}

struct SampleBot
{
    uint8_t Race;
    uint8_t Class;
};

constexpr SampleBot SAMPLE[] = {{1, 2}, {2, 1}, {3, 3}, {4, 11}, {5, 5}, {6, 7},  {7, 8}, {8, 4}, {10, 9}, {11, 2},
                                {1, 5}, {2, 7}, {3, 2}, {4, 4},  {5, 9}, {6, 11}, {7, 4}, {8, 5}, {10, 6}, {11, 8}};

std::string PromptOf(uint32_t guid, SampleBot const& bot)
{
    TraitRules::BotTraits const traits = TraitRules::Roll(guid, 1, bot.Race, bot.Class);
    return FormatPersonalityPrompt(TraitRules::Describe(traits), TraitRules::InterestNames(traits));
}

std::string Sample()
{
    std::ostringstream out;
    out << "WT11b sample: 20 personalities as the LLM sees them (guids 1001-1020, trait version 1)\n"
           "format: #guid race class | {bot_personality} | card traits\n\n";
    uint32_t guid = 1001;
    for (SampleBot const& bot : SAMPLE)
    {
        TraitRules::BotTraits const traits = TraitRules::Roll(guid, 1, bot.Race, bot.Class);
        std::string const prompt = PromptOf(guid, bot);
        std::string const card = FormatCardTraits(TraitRules::Describe(traits));
        Check(!HasDigit(prompt) && !HasDigit(card), "no digits in a sample bot");
        Check(prompt.find("  ") == std::string::npos && card.size() <= 64, "clean sample bot");
        out << "#" << guid << " " << RaceName(bot.Race) << " " << ClassName(bot.Class) << " | " << prompt << " | "
            << card << "\n";
        ++guid;
    }

    // Two of them in the hub random chatter template (OllamaChat.RandomChatterPromptTemplate), filled by hand
    out << "\nRandom chatter prompt (hub template, first two bots; level, area and name are made up):\n";
    for (int i = 0; i < 2; ++i)
    {
        out << "You are Bot" << i + 1 << ", a 60 " << RaceName(SAMPLE[i].Race) << " " << ClassName(SAMPLE[i].Class)
            << " of the Alliance, now in Elwynn Forest, Goldshire. Your manner: " << PromptOf(1001 + i, SAMPLE[i])
            << ". Say one thing aloud to the people around you, in under 15 words.\n";
    }
    return out.str();
}
}  // namespace

int main(int argc, char** argv)
{
    CheckFormatters();
    std::string const sample = Sample();
    std::cout << sample;
    if (argc > 1)
        std::ofstream(argv[1], std::ios::binary) << sample;
    std::printf("\nPASS: %d checks passed\n", g_checks);
    return 0;
}
