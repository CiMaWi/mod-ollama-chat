#ifndef MOD_OLLAMA_CHAT_REPLY_SPLIT_H
#define MOD_OLLAMA_CHAT_REPLY_SPLIT_H

#include <cstddef>
#include <string>
#include <string_view>

// LC10 (PLAN §3.6, LC.11): a bridge reply is never cut; the model keeps short by rule. One chat line holds at most
// ChatLineBytes. A longer reply goes out as two messages split at a sentence end; a reply that would need three, or has
// no sentence end that fits, is refused. Pure: no game headers, no config.
namespace OllamaBridge
{
constexpr std::size_t ChatLineBytes = 255;

struct ReplySplit
{
    bool refused = false;
    std::string first;
    std::string second;  // Empty: the reply is one message.
};

// The bytes of the closing quote that starts at `at` (ASCII " or ', UTF-8 right double or single quote), else 0.
inline std::size_t ClosingQuoteBytes(std::string_view text, std::size_t at)
{
    if (at < text.size() && (text[at] == '"' || text[at] == '\''))
        return 1;
    std::string_view const rest = text.substr(at < text.size() ? at : text.size());
    if (rest.starts_with("\xE2\x80\x9D") || rest.starts_with("\xE2\x80\x99"))
        return 3;
    return 0;
}

// The split is at the last sentence end ('.', '!' or '?', optionally followed by one closing quote, then a space or the
// end of the text) that keeps the first part within `limit` bytes; the spaces after it belong to neither part. The
// second part must fit `limit` too. A split point follows a whole ASCII byte or a whole quote, so neither part ever
// ends or starts inside a UTF-8 sequence, and the parts joined by the spaces between them give the text back.
inline ReplySplit SplitReply(std::string const& text, std::size_t limit = ChatLineBytes)
{
    ReplySplit split;
    if (text.size() <= limit)
    {
        split.first = text;
        return split;
    }
    std::string_view const view(text);
    std::size_t end = 0;
    for (std::size_t i = 0; i < view.size(); ++i)
    {
        if (view[i] != '.' && view[i] != '!' && view[i] != '?')
            continue;
        std::size_t const after = i + 1 + ClosingQuoteBytes(view, i + 1);
        if (after <= limit && (after == view.size() || view[after] == ' '))
            end = after;
    }
    std::size_t start = end;
    while (start < view.size() && view[start] == ' ')
        ++start;
    if (!end || start == view.size() || view.size() - start > limit)
    {
        split.refused = true;
        return split;
    }
    split.first = text.substr(0, end);
    split.second = text.substr(start);
    return split;
}
}  // namespace OllamaBridge

#endif  // MOD_OLLAMA_CHAT_REPLY_SPLIT_H
