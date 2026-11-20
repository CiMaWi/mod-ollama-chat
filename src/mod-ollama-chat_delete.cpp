#include "mod-ollama-chat_delete.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "ScriptMgr.h"

#include <atomic>

namespace
{
// Which pairs the characters DB has; all false until the startup probe, so a delete before it touches no table.
// Written once on the main thread at startup, read by the delete hook on the world thread.
std::array<std::atomic<bool>, ChatRowCount> g_present{};

// Startup, before Player::DeleteOldCharacters (World::SetInitialWorldSettings): one query of the characters DB's
// columns of the list's tables, so a pair whose table or column is absent never runs
class OllamaChatDeleteRowsScript : public WorldScript
{
public:
    OllamaChatDeleteRowsScript() : WorldScript("OllamaChatDeleteRowsScript", { WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE })
    {
    }

    void OnLoadCustomDatabaseTable() override
    {
        std::vector<std::pair<std::string, std::string>> columns;
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT TABLE_NAME, COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND "
                "TABLE_NAME IN (" + ChatRowTables() + ")"))
        {
            do
            {
                Field* fields = result->Fetch();
                columns.emplace_back(fields[0].Get<std::string>(), fields[1].Get<std::string>());
            } while (result->NextRow());
        }

        ChatRowsPresent const present = ChatRowsFound(columns);
        for (std::size_t index = 0; index < ChatRowCount; ++index)
            g_present[index].store(present[index], std::memory_order_relaxed);

        std::string const missing = ChatRowsMissing(present);
        if (!missing.empty())
            LOG_INFO("server.loading", "[Ollama Chat] a deleted character's rows skip what the characters DB lacks:{}",
                     missing);
    }
};

// Every caller of Player::DeleteFromDB that removes the character (CHAR_DELETE_REMOVE): the startup's old-character
// delete, `character erase` and `character deleted delete|old`, a real player's delete below CharDelete.MinLevel, an
// account's delete, a retired bot (BotStores::Delete). All on the world thread, or before the first world update.
class OllamaChatDeleteScript : public PlayerScript
{
public:
    OllamaChatDeleteScript() : PlayerScript("OllamaChatDeleteScript", { PLAYERHOOK_ON_DELETE_FROM_DB }) {}

    // The core's transaction is not used: the module's statements run on their own, so a chat table can never roll
    // the character's delete back
    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction /*trans*/, uint32 guid) override
    {
        // Memory first, as a logout forgets it (each under its own lock; empty before the module has loaded)
        ObjectGuid const player = ObjectGuid::Create<HighGuid::Player>(guid);
        Governor_OnPlayerLogout(player);
        OllamaDispatch_BridgeWatchLogout(player.GetRawValue());

        ChatRowsPresent present{};
        for (std::size_t index = 0; index < ChatRowCount; ++index)
            present[index] = g_present[index].load(std::memory_order_relaxed);
        for (std::string const& sql : ChatDeleteStatements(present, guid))
            CharacterDatabase.Execute(sql);
    }
};
}  // namespace

void AddOllamaChatDeleteScripts()
{
    new OllamaChatDeleteRowsScript();
    new OllamaChatDeleteScript();
}
