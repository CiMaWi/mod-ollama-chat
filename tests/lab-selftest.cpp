// J5e: checks the lab logout decision and its console words (mod-ollama-chat_lab.h, header only).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "mod-ollama-chat_lab.h"

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
}  // namespace

int main()
{
    using OllamaLab::DecideLogout;
    using OllamaLab::LogoutMessage;
    using OllamaLab::LogoutStep;
    // The checks run in order: name, online, bot, holder. Each refusal names the first missing condition.
    Check(DecideLogout(false, true, true, true) == LogoutStep::NoName, "no name is a usage refusal");
    Check(DecideLogout(true, false, true, true) == LogoutStep::NotOnline, "an offline name is refused");
    Check(DecideLogout(true, true, false, true) == LogoutStep::NotBot, "a real player is never logged out");
    Check(DecideLogout(true, true, false, false) == LogoutStep::NotBot, "a real player is refused before the holder");
    Check(DecideLogout(true, true, true, false) == LogoutStep::NoHolder, "an unheld bot is left alone");
    Check(DecideLogout(true, true, true, true) == LogoutStep::LoggedOut, "a held bot is logged out");
    Check(LogoutMessage(LogoutStep::NoName, "") == "Usage: .ollama lab logout <botName>", "usage words");
    Check(LogoutMessage(LogoutStep::NotOnline, "Ann") == "Lab logout: no player named Ann is online.", "offline words");
    Check(LogoutMessage(LogoutStep::NotBot, "Ann") == "Lab logout: Ann is not a bot; refused.", "person words");
    Check(LogoutMessage(LogoutStep::NoHolder, "Ann") == "Lab logout: no bot holder owns Ann; nothing done.",
          "holder words");
    Check(LogoutMessage(LogoutStep::LoggedOut, "Ann") == "Lab logout: Ann is logging out.", "done words");
    std::printf("lab selftest: %d checks passed\n", g_checks);
    return 0;
}
