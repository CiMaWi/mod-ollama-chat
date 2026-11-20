#ifndef MOD_OLLAMA_CHAT_DELETE_H
#define MOD_OLLAMA_CHAT_DELETE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

// The module's rows of a character, deleted when the core deletes the character (OnPlayerDeleteFromDB, also for a
// real player's character and for a retired bot: mod-playerbots' BotStores::Delete ends in the core's delete). A new
// chat table that holds a character's rows adds its pairs here. Game-free, one pair per line: the hub's
// tools/lib/bot-tables.js parses this list (consolidation CS27c, CS.31).
struct ChatRow
{
    char const* table;
    char const* column;
};

// clang-format off
inline constexpr ChatRow ChatRows[] = {
    {"mod_ollama_chat_personality", "guid"},
    {"mod_ollama_chat_bot_player_sentiments", "bot_guid"},
    {"mod_ollama_chat_bot_player_sentiments", "player_guid"},
    {"mod_ollama_chat_history", "bot_guid"},
    {"mod_ollama_chat_history", "player_guid"},
    {"mod_ollama_chat_memories", "bot_guid"},
    {"mod_ollama_chat_relationships", "bot_guid"},
    {"mod_ollama_chat_relationships", "other_guid"},
};
// clang-format on

inline constexpr std::size_t ChatRowCount = std::size(ChatRows);

using ChatRowsPresent = std::array<bool, ChatRowCount>;

// The startup probe's IN list: each table of the list once, quoted, in list order
inline std::string ChatRowTables()
{
    std::vector<std::string> seen;
    std::string out;
    for (ChatRow const& row : ChatRows)
    {
        bool known = false;
        for (std::string const& table : seen)
            known = known || table == row.table;
        if (known)
            continue;
        seen.emplace_back(row.table);
        out += std::string(out.empty() ? "" : ", ") + "'" + row.table + "'";
    }
    return out;
}

// Which pairs the characters DB has, from the probe's (table, column) answers. A statement on a missing table or
// column aborts the worldserver (MySQLConnection: ER_NO_SUCH_TABLE, ER_BAD_FIELD_ERROR), so an absent pair is skipped
inline ChatRowsPresent ChatRowsFound(std::vector<std::pair<std::string, std::string>> const& columns)
{
    ChatRowsPresent present{};
    for (std::size_t index = 0; index < ChatRowCount; ++index)
        for (auto const& [table, column] : columns)
            if (table == ChatRows[index].table && column == ChatRows[index].column)
                present[index] = true;
    return present;
}

// The pairs the probe did not find, " table.column" each (the startup line)
inline std::string ChatRowsMissing(ChatRowsPresent const& present)
{
    std::string out;
    for (std::size_t index = 0; index < ChatRowCount; ++index)
        if (!present[index])
            out += std::string(" ") + ChatRows[index].table + "." + ChatRows[index].column;
    return out;
}

// A character's delete: one statement per present pair, in list order
inline std::vector<std::string> ChatDeleteStatements(ChatRowsPresent const& present, std::uint32_t guid)
{
    std::vector<std::string> out;
    for (std::size_t index = 0; index < ChatRowCount; ++index)
        if (present[index])
            out.push_back(std::string("DELETE FROM ") + ChatRows[index].table + " WHERE " + ChatRows[index].column +
                          " = " + std::to_string(guid));
    return out;
}

// mod-ollama-chat_delete.cpp: the startup probe's script and the delete hook's
void AddOllamaChatDeleteScripts();

#endif
