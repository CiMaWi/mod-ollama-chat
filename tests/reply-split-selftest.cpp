// LC10: checks the two-message reply rule (src/mod-ollama-chat_reply_split.h): a reply within one chat line is one
// part; a longer one splits at the last sentence end that keeps the first part within the line, the second part fits a
// line too, or the reply is refused. Hand cases plus a seeded sweep checked against the rule's properties.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "mod-ollama-chat_reply_split.h"

using OllamaBridge::ChatLineBytes;
using OllamaBridge::ReplySplit;
using OllamaBridge::SplitReply;

namespace
{
int g_checks = 0;

void Check(bool ok, std::string const& what)
{
    ++g_checks;
    if (!ok)
    {
        std::printf("FAIL: %s\n", what.c_str());
        std::exit(1);
    }
}

// True when `text` is complete UTF-8 (no sequence cut at either end or inside).
bool ValidUtf8(std::string_view text)
{
    for (std::size_t i = 0; i < text.size();)
    {
        unsigned char const lead = static_cast<unsigned char>(text[i]);
        std::size_t const length = lead < 0x80           ? 1
                                   : (lead >> 5) == 0x6  ? 2
                                   : (lead >> 4) == 0xE  ? 3
                                   : (lead >> 3) == 0x1E ? 4
                                                         : 0;
        if (!length || i + length > text.size())
            return false;
        for (std::size_t k = 1; k < length; ++k)
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80)
                return false;
        i += length;
    }
    return true;
}

bool Punctuation(char c) { return c == '.' || c == '!' || c == '?'; }

// The rule restated as a test of one position: `at` bytes of `text` end a sentence when the byte before is '.', '!' or
// '?', or a closing quote follows one, and a space or the end of the text comes next.
bool SentenceEndAt(std::string_view text, std::size_t at)
{
    if (!at || at > text.size() || (at < text.size() && text[at] != ' '))
        return false;
    if (Punctuation(text[at - 1]))
        return true;
    if (at >= 2 && (text[at - 1] == '"' || text[at - 1] == '\'') && Punctuation(text[at - 2]))
        return true;
    std::string_view const head = text.substr(0, at);
    return at >= 4 && (head.ends_with("\xE2\x80\x9D") || head.ends_with("\xE2\x80\x99")) && Punctuation(text[at - 4]);
}

// Every property of the rule for one input: the split found, or the refusal, is the only right answer.
void CheckSplit(std::string const& text, std::string const& label, std::size_t limit = ChatLineBytes)
{
    ReplySplit const split = SplitReply(text, limit);
    if (text.size() <= limit)
    {
        Check(!split.refused && split.first == text && split.second.empty(), label + ": one line stays one message");
        return;
    }
    // The last position that could end the first part, and whether the rest after it would fit one line.
    std::size_t best = 0;
    for (std::size_t at = 1; at <= limit && at <= text.size(); ++at)
        if (SentenceEndAt(text, at))
            best = at;
    std::size_t rest = best;
    while (rest < text.size() && text[rest] == ' ')
        ++rest;
    bool const possible = best && rest < text.size() && text.size() - rest <= limit;
    if (!possible)
    {
        Check(split.refused && split.first.empty() && split.second.empty(),
              label + ": no sentence end leaves two lines, so refused");
        return;
    }
    Check(!split.refused, label + ": a fitting sentence end splits");
    Check(!split.first.empty() && !split.second.empty(), label + ": both parts non-empty");
    Check(split.first.size() <= limit && split.second.size() <= limit, label + ": every part within one line");
    Check(split.first.size() == best, label + ": the split is at the last fitting sentence end");
    Check(SentenceEndAt(text, split.first.size()), label + ": the first part ends a sentence");
    Check(text.compare(0, split.first.size(), split.first) == 0 &&
              text.compare(text.size() - split.second.size(), split.second.size(), split.second) == 0 &&
              text.find_first_not_of(' ', split.first.size()) == text.size() - split.second.size(),
          label + ": the parts and the spaces between them are the text");
    Check(ValidUtf8(split.first) && ValidUtf8(split.second), label + ": no part cuts a UTF-8 sequence");
}

std::string Repeat(std::string const& unit, std::size_t count)
{
    std::string text;
    for (std::size_t i = 0; i < count; ++i)
        text += unit;
    return text;
}

// A sentence of exactly `bytes` bytes (>= 2): "aaaa...a." with the given end mark.
std::string Sentence(std::size_t bytes, char end = '.') { return std::string(bytes - 1, 'a') + end; }

void HandCases()
{
    Check(!SplitReply("").refused && SplitReply("").first.empty(), "empty text is one empty part");
    std::string const line255 = Sentence(120) + " " + std::string(132, 'b') + " c";
    Check(line255.size() == 255, "fixture: 255 bytes");
    ReplySplit const one = SplitReply(line255);
    Check(!one.refused && one.first == line255 && one.second.empty(), "exactly 255 bytes is one message, uncut");
    CheckSplit(line255, "255 bytes");

    std::string const text256 = Sentence(200) + " " + Sentence(55);
    Check(text256.size() == 256, "fixture: 256 bytes");
    ReplySplit const two = SplitReply(text256);
    Check(!two.refused && two.first == Sentence(200) && two.second == Sentence(55), "256 bytes split at its sentence");
    CheckSplit(text256, "256 bytes");

    // Sentence ends at 100, 200 and 250 bytes, the next only after the line: the split is at 250.
    std::string const last = Sentence(100) + " " + Sentence(99) + " " + Sentence(49) + " " + Sentence(100);
    ReplySplit const atLast = SplitReply(last);
    Check(!atLast.refused && atLast.first.size() == 250 && atLast.second == Sentence(100),
          "the split is the last sentence end that fits, not the first");
    CheckSplit(last, "last fitting sentence end");

    // A first part of exactly 255 bytes, and a sentence end one byte past the line.
    std::string const full = Sentence(255) + " " + Sentence(40);
    ReplySplit const fullSplit = SplitReply(full);
    Check(!fullSplit.refused && fullSplit.first.size() == 255, "a first part may fill the whole line");
    std::string const over = Sentence(256) + " " + Sentence(40);
    Check(SplitReply(over).refused, "a sentence end at byte 256 is past the line: refused");
    CheckSplit(over, "sentence end at 256");

    for (char const end : {'!', '?'})
    {
        std::string const text = Sentence(150, end) + " " + Sentence(150);
        ReplySplit const split = SplitReply(text);
        Check(!split.refused && split.first == Sentence(150, end), std::string("'") + end + "' ends a sentence");
        CheckSplit(text, std::string("end mark ") + end);
    }
    for (std::string const quote : {"\"", "'", "\xE2\x80\x9D", "\xE2\x80\x99"})
    {
        std::string const first = "He said \"" + std::string(140, 'a') + "." + quote;
        std::string const text = first + " " + Sentence(150);
        ReplySplit const split = SplitReply(text);
        Check(!split.refused && split.first == first, "a closing quote after the end mark stays with the sentence");
        CheckSplit(text, "closing quote");
    }
    // Not sentence ends: a decimal point, the first dots of an ellipsis, a mark before a letter.
    std::string const decimal = std::string(120, 'a') + " costs 3.5 gold and " + std::string(140, 'b') + ".";
    Check(SplitReply(decimal).refused, "a decimal point is no sentence end; one long sentence is refused");
    std::string const dots = Sentence(100) + " Wait... " + Sentence(200);
    ReplySplit const ellipsis = SplitReply(dots);
    Check(!ellipsis.refused && ellipsis.first == Sentence(100) + " Wait...", "an ellipsis ends at its last dot");
    CheckSplit(dots, "ellipsis");

    // Multi-byte text: byte 255 falls inside a two-byte character; the split stays at the sentence end.
    std::string const accented = Repeat("\xC3\xA9", 100) + ". " + Repeat("\xC3\xA9", 100) + ".";
    Check(accented.size() == 403 && (static_cast<unsigned char>(accented[255]) & 0xC0) == 0x80,
          "fixture: byte 255 is a continuation byte");
    ReplySplit const utf8 = SplitReply(accented);
    Check(
        !utf8.refused && utf8.first == Repeat("\xC3\xA9", 100) + "." && ValidUtf8(utf8.first) && ValidUtf8(utf8.second),
        "a multi-byte reply splits at the sentence end, whole characters on "
        "both sides");
    CheckSplit(accented, "multi-byte");
    std::string const noEnd = Repeat("\xE2\x82\xAC ", 80);
    Check(SplitReply(noEnd).refused,
          "multi-byte text without a sentence end is "
          "refused, never cut in a character");

    // Refusals: three messages' worth, no sentence end at all, a second part over the line.
    std::string const three = Sentence(200) + " " + Sentence(200) + " " + Sentence(199);
    Check(three.size() == 601 && SplitReply(three).refused, "a reply needing three messages is refused");
    CheckSplit(three, "three messages");
    std::string const words = Repeat("word ", 60) + "end";
    Check(SplitReply(words).refused, "a reply without a sentence end is refused, never cut at a word");
    std::string const longTail = Sentence(100) + " " + Sentence(256);
    Check(SplitReply(longTail).refused, "a second part over one line is refused");
    std::string const twoAtCap = Sentence(255) + " " + Sentence(255);
    ReplySplit const cap = SplitReply(twoAtCap);
    Check(!cap.refused && cap.first.size() == 255 && cap.second.size() == 255, "two full lines are two messages");
    CheckSplit(twoAtCap, "two full lines");

    // Reassembly for a typical case: joined by one space they are the reply.
    std::string const reply =
        "I saw the wolves near the mill. " + std::string(200, 'x') + "! Then " + std::string(80, 'y') + "?";
    ReplySplit const typical = SplitReply(reply);
    Check(!typical.refused && typical.first + " " + typical.second == reply,
          "the parts joined by a space are the reply");
}

uint64_t g_seed = 0x9E3779B97F4A7C15ull;
uint32_t Next(uint32_t bound)
{
    g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(g_seed >> 33) % bound;
}

// Seeded sweep: sentences of mixed ASCII and multi-byte words, every end mark and quote, decimals and ellipses, single
// and double spaces, total lengths up to about 700 bytes; each result is held against CheckSplit.
void Sweep()
{
    char const* const letters[] = {"a", "b", "e", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80"};
    char const* const quotes[] = {"", "", "", "\"", "'", "\xE2\x80\x9D", "\xE2\x80\x99"};
    char const* const ends[] = {".", "!", "?", "...", "?!"};
    unsigned split = 0, refused = 0;
    for (int round = 0; round < 20000; ++round)
    {
        std::string text;
        std::size_t const target = 1 + Next(700);
        while (text.size() < target)
        {
            if (!text.empty())
                text += Next(8) ? " " : "  ";
            std::size_t const words = 1 + Next(12);
            for (std::size_t w = 0; w < words; ++w)
            {
                if (w)
                    text += " ";
                std::size_t const letterCount = 1 + Next(9);
                for (std::size_t k = 0; k < letterCount; ++k)
                    text += letters[Next(6)];
                if (!Next(25))
                    text += " 3.5";
            }
            text += ends[Next(5)];
            text += quotes[Next(7)];
            if (!Next(10))
                text += " and" + std::string(Next(40),
                                             'z');  // Sometimes the text ends without a sentence end.
        }
        CheckSplit(text, "sweep " + std::to_string(round));
        ReplySplit const result = SplitReply(text);
        if (result.refused)
            ++refused;
        else if (!result.second.empty())
            ++split;
    }
    Check(split > 1000 && refused > 1000, "the sweep exercised both splits and refusals");
    std::printf("sweep: %u splits, %u refused of 20000\n", split, refused);
}
}  // namespace

int main()
{
    HandCases();
    Sweep();
    // A smaller line keeps the same rule (the bound is a parameter, not a constant in the loop).
    CheckSplit(Sentence(10) + " " + Sentence(10), "limit 12", 12);
    CheckSplit(Sentence(10) + " " + Sentence(13), "limit 12, long tail", 12);
    std::printf("%d reply split checks passed\n", g_checks);
    return 0;
}
