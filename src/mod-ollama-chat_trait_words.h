#ifndef MOD_OLLAMA_CHAT_TRAIT_WORDS_H
#define MOD_OLLAMA_CHAT_TRAIT_WORDS_H

// Travel plan WT11b: formats a bot's personality words (from mod-playerbots, already in-world words, strongest first)
// for `{bot_personality}` and the bridge card's `traits` fact. Pure string code: no game headers, no numbers.

#include <cstddef>
#include <string>
#include <vector>

namespace OllamaTraitWords
{
constexpr std::size_t CardTraitBytes = 64;  // OllamaBridge::MaxCardFactBytes
constexpr std::size_t CardTraitWords = 3;

// Control characters dropped, runs of spaces made one, ends trimmed.
inline std::string Clean(std::string const& text)
{
    std::string out;
    for (unsigned char byte : text)
    {
        if (byte < 0x20 || byte == 0x7F)
            continue;
        if (byte == ' ' && (out.empty() || out.back() == ' '))
            continue;
        out.push_back(static_cast<char>(byte));
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// "very careful" -> "careful", "a loner" -> "loner": the word without its intensity or article (the four grades of
// playerbots' TraitRules::Word: somewhat, rather, very, extremely)
inline std::string StripAdverb(std::string const& word)
{
    static char const* const prefixes[] = {"extremely ", "very ",   "rather ", "somewhat ",
                                           "fiercely ",  "deeply ", "a ",      "an "};
    for (char const* prefix : prefixes)
    {
        std::string const p = prefix;
        if (word.compare(0, p.size(), p) == 0)
            return word.substr(p.size());
    }
    return word;
}

// "very careful, very devout; fond of fishing and inns and ale"
inline std::string FormatPersonalityPrompt(std::vector<std::string> const& words,
                                           std::vector<std::string> const& interests)
{
    std::string text;
    for (std::string const& word : words)
    {
        std::string const clean = Clean(word);
        if (!clean.empty())
            text += (text.empty() ? "" : ", ") + clean;
    }

    std::vector<std::string> cleanInterests;
    for (std::string const& interest : interests)
    {
        std::string const clean = Clean(interest);
        if (!clean.empty())
            cleanInterests.push_back(clean);
    }

    if (!cleanInterests.empty())
    {
        std::string joined;
        for (std::size_t i = 0; i < cleanInterests.size(); ++i)
        {
            if (i)
                joined += i + 1 == cleanInterests.size() ? " and " : ", ";
            joined += cleanInterests[i];
        }
        text += (text.empty() ? "fond of " : "; fond of ") + joined;
    }

    return text;
}

// "careful, devout, bookish": the three strongest words without intensity, cut at a word to the card fact bound
inline std::string FormatCardTraits(std::vector<std::string> const& words)
{
    std::string text;
    std::size_t taken = 0;
    for (std::string const& word : words)
    {
        if (taken == CardTraitWords)
            break;
        std::string const clean = Clean(StripAdverb(Clean(word)));
        if (clean.empty())
            continue;
        std::string const next = text.empty() ? clean : text + ", " + clean;
        if (next.size() > CardTraitBytes)
            break;
        text = next;
        ++taken;
    }
    return text;
}
}  // namespace OllamaTraitWords

#endif
