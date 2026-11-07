#ifndef MOD_OLLAMA_CHAT_LAB_H
#define MOD_OLLAMA_CHAT_LAB_H

#include <cstdint>
#include <string>

// J5e: pure decisions of the lab console commands (tests only, no config option). The game reads and the logout call
// stay in mod-ollama-chat_command.cpp on the world thread; this header only orders the checks and words the result.
namespace OllamaLab
{
enum class LogoutStep : uint8_t
{
    NoName,
    NotOnline,
    NotBot,
    NoHolder,
    LoggedOut
};

// named: a name was given; online: a player of that name is in the world; bot: it is a playerbot, not a person;
// held: its master's or the random-bot holder owns it, so that holder logs it out.
inline LogoutStep DecideLogout(bool named, bool online, bool bot, bool held)
{
    if (!named)
        return LogoutStep::NoName;
    if (!online)
        return LogoutStep::NotOnline;
    if (!bot)
        return LogoutStep::NotBot;
    if (!held)
        return LogoutStep::NoHolder;
    return LogoutStep::LoggedOut;
}

inline std::string LogoutMessage(LogoutStep step, std::string const& name)
{
    switch (step)
    {
        case LogoutStep::NoName:
            return "Usage: .ollama lab logout <botName>";
        case LogoutStep::NotOnline:
            return "Lab logout: no player named " + name + " is online.";
        case LogoutStep::NotBot:
            return "Lab logout: " + name + " is not a bot; refused.";
        case LogoutStep::NoHolder:
            return "Lab logout: no bot holder owns " + name + "; nothing done.";
        case LogoutStep::LoggedOut:
            return "Lab logout: " + name + " is logging out.";
    }
    return "Lab logout: unknown result.";
}
}  // namespace OllamaLab

#endif
