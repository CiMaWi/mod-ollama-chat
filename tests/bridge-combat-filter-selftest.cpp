// J5g: OllamaChat.DisableRepliesInCombat in ProcessChat's reply selection. The rule is OllamaBridge::CombatSkipsReply
// (mod-ollama-chat_bridge_query.h): ProcessChat asks it at its four selection sites (a whisper's receiver, the mention
// scan, the first mentioned bot, the unnamed candidates) with the config flag, whether the line is a human's under the
// bridge, and the bot's combat state. This selftest proves the rule's whole truth table. The call sites are ProcessChat
// code, which only the game can run: the lab's functional runs cover them.
#include <iostream>
#include <stdexcept>
#include <string>

#include "mod-ollama-chat_bridge_query.h"

namespace
{
unsigned checks = 0;
void Check(bool condition, std::string const& message)
{
    ++checks;
    if (!condition)
        throw std::runtime_error("FAIL " + message);
}

void Predicate()
{
    using OllamaBridge::CombatSkipsReply;
    // Flag on: only a fighting bot is skipped, and never for a human's line under the bridge (DS.14).
    Check(CombatSkipsReply(true, false, true), "flag 1: other chatter to a fighting bot is skipped");
    Check(!CombatSkipsReply(true, true, true), "flag 1: a human's bridge line reaches a fighting bot");
    Check(!CombatSkipsReply(true, false, false), "flag 1: other chatter to a calm bot is answered");
    Check(!CombatSkipsReply(true, true, false), "flag 1: a human's bridge line to a calm bot is answered");
    // Flag off: nobody is skipped.
    Check(!CombatSkipsReply(false, false, true), "flag 0: other chatter reaches a fighting bot");
    Check(!CombatSkipsReply(false, true, true), "flag 0: a human's bridge line reaches a fighting bot");
    Check(!CombatSkipsReply(false, false, false), "flag 0: other chatter to a calm bot is answered");
    Check(!CombatSkipsReply(false, true, false), "flag 0: a human's bridge line to a calm bot is answered");
}
}  // namespace

int main()
{
    try
    {
        Predicate();
        std::cout << "PASS " << checks << " combat filter checks\n";
    }
    catch (std::exception const& error)
    {
        std::cout << error.what() << "\n";
        return 1;
    }
    return 0;
}
