#include "mod-ollama-chat_persona_db.h"

#include <list>
#include <memory>
#include <variant>

#include "DatabaseEnv.h"
#include "DatabaseScript.h"
#include "Field.h"
#include "Log.h"
#include "ModuleDatabasePool.h"
#include "MySQLPreparedStatement.h"
#include "QueryResult.h"
#include "StringConvert.h"

namespace OllamaPersona
{
namespace
{
enum PersonaStatement : uint32
{
    SelectPersona,
    InsertPersona,
    UpdatePersona,
    PersonaStatementCount
};

class PersonaConnection final : public MySQLConnection
{
public:
    using MySQLConnection::MySQLConnection;

private:
    void DoPrepareStatements() override
    {
        m_stmts.resize(PersonaStatementCount);
        PrepareStatement(SelectPersona,
                         "SELECT seed.requested_guid, p.bot_guid, p.revision, p.accepted_source, "
                         "p.accepted_body, p.claude_alternative, p.codex_alternative "
                         "FROM (SELECT CAST(? AS UNSIGNED) AS requested_guid) AS seed "
                         "LEFT JOIN mod_ollama_chat_persona_cards AS p ON p.bot_guid = seed.requested_guid",
                         CONNECTION_ASYNC);
        PrepareStatement(InsertPersona,
                         "INSERT INTO mod_ollama_chat_persona_cards "
                         "(bot_guid, revision, accepted_source, accepted_body, claude_alternative, codex_alternative) "
                         "VALUES (?, ?, ?, ?, ?, ?)",
                         CONNECTION_ASYNC);
        PrepareStatement(UpdatePersona,
                         "UPDATE mod_ollama_chat_persona_cards SET revision = ?, accepted_source = ?, "
                         "accepted_body = ?, claude_alternative = ?, codex_alternative = ? "
                         "WHERE bot_guid = ? AND revision = ?",
                         CONNECTION_ASYNC);
    }

    // Lost connections go to the core handler. It reconnects, re-prepares and reruns a transaction cut off before
    // COMMIT (up to 3 times, the compare-and-swap makes that safe) and fails one cut off during COMMIT. Other errors
    // stay local: the core aborts on schema or parse errors, and this storage is optional. Codes from errmsg.h,
    // which the core's database target keeps private.
    bool _HandleMySQLErrno(uint32 errNo, char const* err, uint8 attempts) override
    {
        switch (errNo)
        {
            case 2003:  // CR_CONN_HOST_ERROR
            case 2006:  // CR_SERVER_GONE_ERROR
            case 2013:  // CR_SERVER_LOST
            case 2055:  // CR_SERVER_LOST_EXTENDED
                return MySQLConnection::_HandleMySQLErrno(errNo, err, attempts);
            default:
                return false;
        }
    }
};

using PersonaPreparedStatement = PreparedStatement<PersonaConnection>;

class PersonaPool final : public ModuleDatabasePool
{
public:
    PersonaPreparedStatement* Statement(PersonaStatement index)
    {
        return new PersonaPreparedStatement(index, GetPreparedStatementParamCount(index));
    }

private:
    MySQLConnection* CreateConnection(MySQLConnectionInfo& info) override { return new PersonaConnection(info); }

    MySQLConnection* CreateAsyncConnection(ProducerConsumerQueue<SQLOperation*>* queue,
                                           MySQLConnectionInfo& info) override
    {
        return new PersonaConnection(queue, info);
    }
};

template <typename T>
std::optional<T> Nullable(Field const& field)
{
    if (field.IsNull())
        return std::nullopt;
    return field.Get<T>();
}

std::optional<PersonaDbRow> ReadPersonaRow(PreparedQueryResult const& result)
{
    if (!result || result->GetRowCount() != 1 || result->GetFieldCount() != 7)
        return std::nullopt;
    Field const* fields = result->Fetch();
    if (fields[0].IsNull())
        return std::nullopt;
    return PersonaDbRow{fields[0].Get<uint64>(),
                        Nullable<uint64>(fields[1]),
                        Nullable<uint64>(fields[2]),
                        Nullable<uint8>(fields[3]),
                        Nullable<std::string_view>(fields[4]),
                        Nullable<std::string_view>(fields[5]),
                        Nullable<std::string_view>(fields[6])};
}

void BindBody(PersonaPreparedStatement& statement, uint8 index, std::string_view body)
{
    if (body.empty())
        statement.SetData(index, nullptr);
    else
        statement.SetData(index, body);
}

class PreparedPersonaDb final : public PersonaDbIo
{
public:
    bool Open()
    {
        auto const& info = *CharacterDatabase.GetConnectionInfo();
        std::string host = info.host;
#ifndef _WIN32
        // The core rewrites the host to localhost after opening a Unix socket.
        if (host == "localhost" && !Acore::StringTo<uint32>(info.port_or_socket))
            host = ".";
#endif
        if (host.empty() || info.database.empty())
            return false;
        auto const connection =
            host + ";" + info.port_or_socket + ";" + info.user + ";" + info.password + ";" + info.database;
        _pool.SetConnectionInfo(info.ssl.empty() ? connection : connection + ";" + info.ssl, 1, 1);
        return _pool.Open() == 0 && _pool.PrepareStatements();
    }

    void Lookup(uint64_t botGuid, LookupCallback callback) override
    {
        auto* statement = _pool.Statement(SelectPersona);
        statement->SetData(0, static_cast<uint64>(botGuid));
        _callbacks.emplace_back(_pool.AsyncQuery(statement).WithPreparedCallback(
            [this, callback = std::move(callback)](PreparedQueryResult result)
            {
                auto const row = ReadPersonaRow(result);
                Report(row.has_value());
                callback(row);
            }));
    }

    void Commit(std::vector<StoreOperation> const& writes, CommitCallback callback) override
    {
        auto transaction = std::make_shared<Transaction<PersonaConnection>>();
        for (auto const& operation : writes)
        {
            auto const& write = *operation.write;
            State const& next = write.next;
            bool const insert = write.priorRevision == 0;
            auto* statement = _pool.Statement(insert ? InsertPersona : UpdatePersona);
            uint8 index = 0;
            if (insert)
                statement->SetData(index++, static_cast<uint64>(next.botGuid));
            statement->SetData(index++, static_cast<uint64>(next.revision));
            statement->SetData(index++, next.accepted ? static_cast<uint8>(next.accepted->source) : uint8{0});
            BindBody(*statement, index++, next.accepted ? std::string_view(next.accepted->body) : std::string_view{});
            BindBody(*statement, index++, next.claudeAlternative);
            BindBody(*statement, index++, next.codexAlternative);
            if (!insert)
            {
                statement->SetData(index++, static_cast<uint64>(next.botGuid));
                statement->SetData(index, static_cast<uint64>(write.priorRevision));
            }
            transaction->Append(statement);
        }
        auto result = _pool.AsyncCommitTransaction(std::move(transaction));
        result.AfterComplete(
            [this, callback = std::move(callback)](bool committed)
            {
                Report(committed);
                callback(committed);
            });
        _callbacks.emplace_back(std::move(result));
    }

    void Poll() override
    {
        std::size_t remaining = _callbacks.size();
        std::size_t completed = 0;
        for (auto it = _callbacks.begin(); remaining && completed < MaxStoreCompletionsPerPump; --remaining)
        {
            // A commit callback may append new queries. Poll those on the next world update.
            if (std::visit([](auto& callback) { return callback.InvokeIfReady(); }, *it))
            {
                it = _callbacks.erase(it);
                ++completed;
            }
            else
                ++it;
        }
    }

    // World thread, on the core's ping tick, so MySQL's wait_timeout never closes an idle pool.
    void KeepAlive() { _pool.KeepAlive(); }

    void Close() override
    {
        // ModuleDatabasePool joins workers after draining the issued queue. Suppress callbacks that
        // could start readback after shutdown. The port then disposes each remaining operation once.
        _pool.Close();
        _callbacks.clear();
    }

private:
    // Callbacks run only in Poll, on the world thread. One line per change, so an outage logs once.
    void Report(bool ok)
    {
        if (ok != _failing)
            return;
        _failing = !ok;
        if (_failing)
            LOG_WARN("module.ollamachat", "Persona card storage failed a lookup or commit and retries. See sql.sql.");
        else
            LOG_INFO("module.ollamachat", "Persona card storage works again.");
    }

    PersonaPool _pool;
    std::list<std::variant<QueryCallback, TransactionCallback>> _callbacks;
    bool _failing = false;
};

struct PersonaDbSession
{
    PreparedPersonaDb io;
    PersonaDbPort port{io};
};

std::unique_ptr<PersonaDbSession> dbSession;

class PersonaDbKeepAliveScript final : public DatabaseScript
{
public:
    PersonaDbKeepAliveScript()
        : DatabaseScript("OllamaPersonaDbKeepAliveScript", {DATABASEHOOK_ON_MODULE_DATABASES_KEEPALIVE})
    {
    }

    void OnModuleDatabasesKeepAlive() override
    {
        if (dbSession)
            dbSession->io.KeepAlive();
    }
};
}  // namespace

void AddPersonaDbScripts() { new PersonaDbKeepAliveScript(); }

void StartPersonaDb()
{
    Store& store = WorldStore();
    if (!store.Enabled() || dbSession)
        return;
    auto session = std::make_unique<PersonaDbSession>();
    if (!session->io.Open() || !store.SetPort(&session->port))
    {
        LOG_ERROR("module.ollamachat", "Persona card storage unavailable. Check the Characters update and restart.");
        return;
    }
    dbSession = std::move(session);
}

void PumpPersonaDb()
{
    if (!dbSession)
        return;
    dbSession->port.Pump();
    WorldStore().Pump(std::chrono::steady_clock::now());
}

void DiscardStagedPersonaWrites(std::optional<uint64_t> botGuid)
{
    if (dbSession)
        dbSession->port.DiscardStaged(botGuid);
}

void ForgetPersona(uint64_t botGuid) { ForgetPersona(WorldStore(), dbSession ? &dbSession->port : nullptr, botGuid); }

void RetirePersonas(uint64_t generation)
{
    RetirePersonas(WorldStore(), dbSession ? &dbSession->port : nullptr, generation);
}

void StopPersonaDb()
{
    Store& store = WorldStore();
    store.Configure(false);
    if (!dbSession)
        return;
    dbSession->port.Close();
    auto const now = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < MaxStoreOperations; i += MaxStoreCompletionsPerPump)
        store.Pump(now);
    if (store.SetPort(nullptr))
        dbSession.reset();
}
}  // namespace OllamaPersona
