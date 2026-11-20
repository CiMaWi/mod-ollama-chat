#ifndef MOD_OLLAMA_CHAT_TRAIT_WORDS_H
#define MOD_OLLAMA_CHAT_TRAIT_WORDS_H

// Travel plan WT11b: formats a bot's personality words (from mod-playerbots, already in-world words, strongest first)
// for `{bot_personality}`. Pure string code: no game headers, no numbers.

#include <cstddef>
#include <string>
#include <vector>

namespace OllamaTraitWords
{
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

// "wary, devout; fond of fishing and inns and ale"
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
}  // namespace OllamaTraitWords

#endif
