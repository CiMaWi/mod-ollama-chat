// Selftest stand-in for the core's ScriptMgr.h: the script bases the module's headers derive from, with the hooks
// they override. No selftest registers or runs a script.
#ifndef OLLAMA_SELFTEST_SCRIPT_MGR_H
#define OLLAMA_SELFTEST_SCRIPT_MGR_H

#include <cstdint>
#include <string>
#include <vector>

using uint32 = uint32_t;

class Channel;
class Group;
class Guild;
class ObjectGuid;
class Player;

enum PlayerHook : uint16_t
{
    PLAYERHOOK_CAN_PLAYER_USE_CHAT,
    PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
    PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
    PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
    PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT,
    PLAYERHOOK_ON_LOGOUT
};

class WorldScript
{
public:
    virtual ~WorldScript() = default;
    virtual void OnStartup() = 0;
    virtual void OnShutdown() = 0;
};

class PlayerScript
{
public:
    PlayerScript(char const*, std::vector<uint16_t> const& = {}) {}
    virtual ~PlayerScript() = default;
    virtual bool OnPlayerCanUseChat(Player*, uint32_t, uint32_t, std::string&, Player*) = 0;
    virtual bool OnPlayerCanUseChat(Player*, uint32_t, uint32_t, std::string&) = 0;
    virtual bool OnPlayerCanUseChat(Player*, uint32_t, uint32_t, std::string&, Group*) = 0;
    virtual bool OnPlayerCanUseChat(Player*, uint32_t, uint32_t, std::string&, Guild*) = 0;
    virtual bool OnPlayerCanUseChat(Player*, uint32_t, uint32_t, std::string&, Channel*) = 0;
    virtual void OnPlayerLogout(Player*) = 0;
    virtual void OnPlayerTextEmote(Player*, uint32, uint32, ObjectGuid) = 0;
};

#endif  // OLLAMA_SELFTEST_SCRIPT_MGR_H
