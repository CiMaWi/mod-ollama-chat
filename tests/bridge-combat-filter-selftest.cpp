// J5g: OllamaChat.DisableRepliesInCombat in ProcessChat's reply selection (hub J5g-check runner). Real: the
// bridgeHumanLine line and the whole whisper/say selection block of handler.cpp (extracted verbatim into .inc files)
// and OllamaBridge::CombatSkipsReply. Models: a whisper's candidates (the receiver only, as
// IsBotEligibleForChatChannelLocal: no map, distance or team term; the runner checks that source order) and the bridge
// gate (only a human's line opens a request). Game objects, chance roll (always passes), name mention and language
// barrier are stand-ins. No live-game proof.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mod-ollama-chat_bridge_query.h"

#define LOG_INFO(...) ((void)0)

enum ChatChannelSourceLocal
{
    SRC_UNDEFINED_LOCAL = 0,
    SRC_SAY_LOCAL = 1,
    SRC_PARTY_LOCAL = 2,
    SRC_RAID_LOCAL = 3,
    SRC_GUILD_LOCAL = 4,
    SRC_OFFICER_LOCAL = 5,
    SRC_YELL_LOCAL = 6,
    SRC_WHISPER_LOCAL = 7,
    SRC_GENERAL_LOCAL = 17
};

struct Player
{
    std::string name;
    uint32_t mapId = 0;
    bool combat = false;
    bool human = false;
    bool IsInCombat() const { return combat; }
    std::string const& GetName() const { return name; }
};

bool g_DebugEnabled = true;  // The debug branches run; LOG_INFO is a no-op.
bool g_DisableRepliesInCombat = true;
uint32_t g_DirectAddressReplyChance = 0;
uint32_t urand(uint32_t min, uint32_t) { return min; }
size_t OllamaFindBotNameMention(std::string const& msg, std::string const& botName) { return msg.find(botName); }
bool Roleplay_IsLanguageBarrier(Player*, Player*) { return false; }
bool OllamaIsDirectAddress(Player*, Player*, ChatChannelSourceLocal, std::string const&, bool, std::string const&)
{
    return false;
}

unsigned checks = 0;
void Check(bool condition, std::string const& message)
{
    ++checks;
    if (!condition)
        throw std::runtime_error("FAIL " + message);
}

// The selection step between "Candidates" and "Final" of ProcessChat, with its own locals.
std::vector<Player*> Select(Player* player, std::vector<Player*> const& candidateBots,
                            ChatChannelSourceLocal sourceLocal, bool bridgeProvider, bool senderIsBot,
                            std::string const& trimmedMsg)
{
    uint32_t const chance = 100;
    std::string const scopeKey = "scope";
#include "combat-line.inc"
    std::vector<Player*> finalCandidates;
#include "combat-select.inc"
    return finalCandidates;
}

struct Outcome
{
    unsigned bridgeRequests = 0;  // BridgeSubmit calls (the bridge request path).
    unsigned localReplies = 0;    // Local model replies (non-bridge provider).
};

// One human or bot line through the models and the extracted selection. A whisper's candidates are its receiver
// wherever it stands; the bridge gate admits only a human's whisper/party/say/yell.
Outcome Line(Player* sender, Player* receiver, std::vector<Player*> const& online, ChatChannelSourceLocal source,
             std::string const& msg, bool bridge)
{
    Outcome outcome;
    bool const senderIsBot = !sender->human;
    if (bridge && (senderIsBot || (source != SRC_WHISPER_LOCAL && source != SRC_PARTY_LOCAL &&
                                   source != SRC_SAY_LOCAL && source != SRC_YELL_LOCAL)))
        return outcome;
    std::vector<Player*> candidates;
    for (Player* bot : online)
        if (bot != sender && (source != SRC_WHISPER_LOCAL || (receiver && bot == receiver)))
            candidates.push_back(bot);
    for (Player* bot : Select(sender, candidates, source, bridge, senderIsBot, msg))
    {
        (void)bot;
        ++(bridge ? outcome.bridgeRequests : outcome.localReplies);
    }
    return outcome;
}

void Whispers()
{
    Player human{"Chris", 0, false, true};
    Player far{"Halnir", 571, false, false};
    std::vector<Player*> const online{&human, &far};

    g_DisableRepliesInCombat = true;
    Outcome result = Line(&human, &far, online, SRC_WHISPER_LOCAL, "where are you", true);
    Check(result.bridgeRequests == 1,
          "(a) a whisper to a bot on another map, not fighting, reaches the bridge request");

    far.combat = true;
    result = Line(&human, &far, online, SRC_WHISPER_LOCAL, "where are you", true);
    Check(result.bridgeRequests == 1, "(b) a whisper to a fighting bot under the bridge reaches the request (flag 1)");

    result = Line(&human, &far, online, SRC_WHISPER_LOCAL, "where are you", false);
    Check(result.localReplies == 0, "(c) a local provider still skips a fighting bot (flag 1)");

    g_DisableRepliesInCombat = false;
    result = Line(&human, &far, online, SRC_WHISPER_LOCAL, "where are you", false);
    Check(result.localReplies == 1, "(e) flag 0: a fighting bot answers a local whisper");
    result = Line(&human, &far, online, SRC_WHISPER_LOCAL, "where are you", true);
    Check(result.bridgeRequests == 1, "(e) flag 0: a fighting bot answers a bridge whisper");

    g_DisableRepliesInCombat = true;
    far.combat = false;
    result = Line(&human, &far, online, SRC_WHISPER_LOCAL, "where are you", false);
    Check(result.localReplies == 1, "a local whisper to a far bot, not fighting, answers (no map term)");
}

void Speech()
{
    Player human{"Chris", 0, false, true};
    Player named{"Talithea", 0, true, false};
    Player calm{"Jarem", 0, false, false};
    Player fighter{"Aaltraa", 0, true, false};
    g_DisableRepliesInCombat = true;

    Outcome result = Line(&human, nullptr, {&human, &named, &calm}, SRC_SAY_LOCAL, "Talithea, behind you", true);
    Check(result.bridgeRequests == 1, "a bridge say naming a fighting bot opens one request");
    std::vector<Player*> chosen = Select(&human, {&named, &calm}, SRC_SAY_LOCAL, true, false, "Talithea, behind you");
    Check(chosen.size() == 1 && chosen[0] == &named, "the named fighting bot is the one chosen (mention scan)");

    chosen = Select(&human, {&fighter, &named}, SRC_SAY_LOCAL, true, false, "anyone here");
    Check(chosen.size() == 2, "a bridge say without a name reaches fighting bots too");

    chosen = Select(&human, {&fighter, &calm}, SRC_SAY_LOCAL, false, false, "anyone here");
    Check(chosen.size() == 1 && chosen[0] == &calm, "a local say still skips the fighting bot");
    chosen = Select(&human, {&named, &calm}, SRC_SAY_LOCAL, false, false, "Talithea, behind you");
    Check(chosen.size() == 1 && chosen[0] == &calm, "a local say naming a fighting bot: another bot may answer");

    // (d) A bot's line under the bridge: the gate stops it, and the selection still applies the combat check.
    result = Line(&calm, nullptr, {&calm, &named}, SRC_SAY_LOCAL, "Talithea, behind you", true);
    Check(result.bridgeRequests == 0, "(d) a bot's line opens no bridge request");
    chosen = Select(&calm, {&named, &fighter}, SRC_SAY_LOCAL, true, true, "anyone here");
    Check(chosen.empty(), "(d) a bot's line keeps the combat check under the bridge provider");
}

void Predicate()
{
    Check(!OllamaBridge::CombatSkipsReply(true, true, true), "bridge human line in combat is not skipped");
    Check(OllamaBridge::CombatSkipsReply(true, false, true), "other chatter in combat is skipped");
    Check(!OllamaBridge::CombatSkipsReply(true, false, false), "nobody out of combat is skipped");
    Check(!OllamaBridge::CombatSkipsReply(false, false, true), "flag 0 never skips");
}

int main()
{
    try
    {
        Predicate();
        Whispers();
        Speech();
        std::cout << "PASS " << checks << " combat filter checks\n";
    }
    catch (std::exception const& error)
    {
        std::cout << error.what() << "\n";
        return 1;
    }
    return 0;
}
