// Thin RAII wrapper over the SQLite C API: connection setup, prepared
// statements, transactions and the migration runner.
//
// Pragmas applied on open (rationale in docs/adr/0003-sqlite-wal-storage.md):
//   journal_mode = WAL        concurrent readers during a capture write
//   synchronous  = NORMAL     WAL makes this durable enough for user data
//   foreign_keys = ON         cascade deletes keep orphan rows impossible
//   busy_timeout = 3000       CLI and daemon can touch the file simultaneously
//   temp_store   = MEMORY     no temp files with user URLs on disk
#pragma once

#include <contextsnap/core/result.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace contextsnap::storage {

using core::Result;
using core::Status;

class Database;

/// Prepared statement with a fluent binder. Parameter indices are 1-based, as
/// in SQLite itself.
class Statement {
public:
    ~Statement();

    Statement(Statement&&) noexcept;
    Statement& operator=(Statement&&) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& bind(int index, std::nullptr_t);
    Statement& bind(int index, std::int64_t value);
    Statement& bind(int index, std::int32_t value);
    Statement& bind(int index, double value);
    Statement& bind(int index, bool value);
    Statement& bind(int index, std::string_view value);
    Statement& bind(int index, const std::vector<std::uint8_t>& blob);
    Statement& bind_optional(int index, const std::optional<std::string>& value);

    /// Advances one row. Returns true while rows remain.
    [[nodiscard]] Result<bool> step();

    /// Executes a statement expected to produce no rows.
    [[nodiscard]] Status execute();

    [[nodiscard]] std::int64_t column_int(int index) const;
    [[nodiscard]] double column_double(int index) const;
    [[nodiscard]] bool column_bool(int index) const;
    [[nodiscard]] std::string column_text(int index) const;
    [[nodiscard]] std::optional<std::string> column_text_optional(int index) const;
    [[nodiscard]] std::vector<std::uint8_t> column_blob(int index) const;
    [[nodiscard]] bool column_is_null(int index) const;
    [[nodiscard]] int column_count() const;

    Statement& reset();

private:
    friend class Database;

    explicit Statement(sqlite3_stmt* stmt, sqlite3* db);

    sqlite3_stmt* stmt_{nullptr};
    sqlite3* db_{nullptr};
};

/// RAII transaction. Rolls back unless commit() was called.
class Transaction {
public:
    explicit Transaction(Database& db, bool immediate = true);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    [[nodiscard]] Status commit();
    void rollback();
    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    Database& db_;
    bool active_{false};
};

struct DatabaseOptions {
    std::string path;
    bool read_only{false};
    bool create_if_missing{true};
    std::optional<std::string> encryption_key;  ///< SQLCipher only.
    std::uint32_t busy_timeout_ms{3000};
    std::uint32_t cache_size_kb{4096};
};

class Database {
public:
    ~Database();

    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    [[nodiscard]] static Result<Database> open(const DatabaseOptions& options);

    /// Opens a purely in-memory database — used by tests and by `--dry-run`.
    [[nodiscard]] static Result<Database> open_memory();

    [[nodiscard]] Result<Statement> prepare(std::string_view sql);
    [[nodiscard]] Status execute(std::string_view sql);

    [[nodiscard]] std::int64_t last_insert_rowid() const;
    [[nodiscard]] int changes() const;

    /// Applies every pending migration from sql/ (embedded at build time).
    [[nodiscard]] Status migrate();
    [[nodiscard]] Result<std::int32_t> user_version();
    [[nodiscard]] Status set_user_version(std::int32_t version);

    [[nodiscard]] Status vacuum();
    [[nodiscard]] Status checkpoint();
    [[nodiscard]] Result<std::uint64_t> file_size_bytes() const;

    /// `PRAGMA integrity_check` — surfaced by `contextsnap doctor`.
    [[nodiscard]] Result<bool> integrity_check();

    /// Re-keys an encrypted database (key rotation).
    [[nodiscard]] Status rekey(std::string_view new_key);

    [[nodiscard]] sqlite3* handle() const noexcept { return db_; }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    Database() = default;

    sqlite3* db_{nullptr};
    std::string path_;
};

/// One migration step, embedded from sql/*.sql at build time.
struct Migration {
    std::int32_t version{0};
    std::string_view name;
    std::string_view sql;
};

/// The ordered, compiled-in migration list (src/storage/migrations.cpp).
[[nodiscard]] const std::vector<Migration>& migrations();

}  // namespace contextsnap::storage
