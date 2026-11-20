// CS27c: checks a deleted character's chat rows (src/mod-ollama-chat_delete.h): the list holds the eight pairs that
// mod-playerbots' BotStores.h held before (at 37bc88f2, in that order), the probe's IN list, the skip of a pair the
// characters DB lacks (a statement on a missing table aborts the worldserver) and the statement text.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "mod-ollama-chat_delete.h"

namespace
{
int g_checks = 0;

void Check(bool ok, char const* what)
{
    ++g_checks;
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        std::exit(1);
    }
}

using Columns = std::vector<std::pair<std::string, std::string>>;

Columns AllColumns()
{
    Columns out;
    for (ChatRow const& row : ChatRows)
        out.emplace_back(row.table, row.column);
    // other columns of the same tables, and other tables, change nothing
    out.emplace_back("mod_ollama_chat_history", "player_message");
    out.emplace_back("mod_ollama_chat_voice", "guid");
    return out;
}
}  // namespace

int main()
{
    // The list: the eight pairs, in the delete order BotStores.h had
    char const* const expected[][2] = {
        {"mod_ollama_chat_personality", "guid"},
        {"mod_ollama_chat_bot_player_sentiments", "bot_guid"},
        {"mod_ollama_chat_bot_player_sentiments", "player_guid"},
        {"mod_ollama_chat_history", "bot_guid"},
        {"mod_ollama_chat_history", "player_guid"},
        {"mod_ollama_chat_memories", "bot_guid"},
        {"mod_ollama_chat_relationships", "bot_guid"},
        {"mod_ollama_chat_relationships", "other_guid"},
    };
    Check(ChatRowCount == 8, "eight pairs");
    for (std::size_t index = 0; index < ChatRowCount; ++index)
        Check(std::string(ChatRows[index].table) == expected[index][0] &&
                  std::string(ChatRows[index].column) == expected[index][1],
              "a pair in its place");

    // The probe's IN list: five tables, each once
    Check(ChatRowTables() ==
              "'mod_ollama_chat_personality', 'mod_ollama_chat_bot_player_sentiments', 'mod_ollama_chat_history', "
              "'mod_ollama_chat_memories', 'mod_ollama_chat_relationships'",
          "the probe names each table once");

    // Before the probe (startup's old-character delete runs earlier on another path) nothing runs
    ChatRowsPresent const none{};
    Check(ChatDeleteStatements(none, 42).empty(), "no statement before the probe");
    Check(ChatRowsMissing(none).size() > 0, "before the probe every pair is missing");

    // Every pair present: eight statements in list order
    ChatRowsPresent const all = ChatRowsFound(AllColumns());
    std::vector<std::string> const statements = ChatDeleteStatements(all, 4242);
    Check(statements.size() == 8, "eight statements when every pair is present");
    Check(statements.front() == "DELETE FROM mod_ollama_chat_personality WHERE guid = 4242", "the first statement");
    Check(statements[2] == "DELETE FROM mod_ollama_chat_bot_player_sentiments WHERE player_guid = 4242",
          "the other column of a pair table");
    Check(statements.back() == "DELETE FROM mod_ollama_chat_relationships WHERE other_guid = 4242",
          "the last statement");
    Check(ChatRowsMissing(all).empty(), "nothing missing when every pair is present");

    // A missing table: its pair is skipped, the others run
    Columns noMemories;
    for (auto const& column : AllColumns())
        if (column.first != "mod_ollama_chat_memories")
            noMemories.push_back(column);
    ChatRowsPresent const partial = ChatRowsFound(noMemories);
    std::vector<std::string> const seven = ChatDeleteStatements(partial, 7);
    Check(seven.size() == 7, "a missing table skips its pair");
    bool memories = false;
    for (std::string const& sql : seven)
        memories = memories || sql.find("mod_ollama_chat_memories") != std::string::npos;
    Check(!memories, "no statement on the missing table");
    Check(ChatRowsMissing(partial) == " mod_ollama_chat_memories.bot_guid", "the startup line names the missing pair");

    // A missing column of a present table: only that pair is skipped
    Columns noOther;
    for (auto const& column : AllColumns())
        if (!(column.first == "mod_ollama_chat_relationships" && column.second == "other_guid"))
            noOther.push_back(column);
    ChatRowsPresent const column = ChatRowsFound(noOther);
    Check(ChatDeleteStatements(column, 7).size() == 7 && !column[7] && column[6], "a missing column skips its pair");

    // A table of the same name prefix is not taken for a listed one
    Check(!ChatRowsFound({{"mod_ollama_chat_personality_templates", "guid"}})[0], "names match whole");

    std::printf("PASS %d chat delete checks\n", g_checks);
    return 0;
}
