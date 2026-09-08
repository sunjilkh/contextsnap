#include <contextsnap/storage/database.hpp>

#include <contextsnap/core/logging.hpp>

#include <chrono>
#include <filesystem>
#include <utility>

#include <sqlite3.h>

namespace contextsnap::storage {
namespace {

core::Error sqlite_error(sqlite3* db, std::string_view what, std::string_view context) {
    const int code = db != nullptr ? sqlite3_errcode(db) : SQLITE_ERROR;
    const char* message = db != nullptr ? sqlite3_errmsg(db) : "no database handle";
    return core::err::database(std::string(what) + ": " + message, std::string(context), code);
}

std::int64_t now_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

Statement::Statement(sqlite3_stmt* stmt, sqlite3* db) : stmt_(stmt), db_(db) {}

Statement::~Statement() {
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
    }
}

Statement::Statement(Statement&& other) noexcept
    : stmt_(std::exchange(other.stmt_, nullptr)), db_(std::exchange(other.db_, nullptr)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
        stmt_ = std::exchange(other.stmt_, nullptr);
        db_ = std::exchange(other.db_, nullptr);
    }
    return *this;
}

Statement& Statement::bind(int index, std::nullptr_t) {
    sqlite3_bind_null(stmt_, index);
    return *this;
}

Statement& Statement::bind(int index, std::int64_t value) {
    sqlite3_bind_int64(stmt_, index, value);
    return *this;
}

Statement& Statement::bind(int index, std::int32_t value) {
    sqlite3_bind_int(stmt_, index, value);
    return *this;
}

Statement& Statement::bind(int index, double value) {
    sqlite3_bind_double(stmt_, index, value);
    return *this;
}

Statement& Statement::bind(int index, bool value) {
    sqlite3_bind_int(stmt_, index, value ? 1 : 0);
    return *this;
}

Statement& Statement::bind(int index, std::string_view value) {
    // SQLITE_TRANSIENT: SQLite copies the bytes, so temporaries are safe.
    sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                      SQLITE_TRANSIENT);
    return *this;
}

Statement& Statement::bind(int index, const std::vector<std::uint8_t>& blob) {
    sqlite3_bind_blob(stmt_, index, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
    return *this;
}

Statement& Statement::bind_optional(int index, const std::optional<std::string>& value) {
    return value.has_value() ? bind(index, *value) : bind(index, nullptr);
}

Result<bool> Statement::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    return sqlite_error(db_, "step failed", "storage.statement");
}

Status Statement::execute() {
    auto stepped = step();
    if (!stepped) {
        return stepped.error();
    }
    return Status::success();
}

std::int64_t Statement::column_int(int index) const { return sqlite3_column_int64(stmt_, index); }

double Statement::column_double(int index) const { return sqlite3_column_double(stmt_, index); }

bool Statement::column_bool(int index) const { return sqlite3_column_int(stmt_, index) != 0; }

std::string Statement::column_text(int index) const {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, index));
    if (text == nullptr) {
        return {};
    }
    return std::string(text, static_cast<std::size_t>(sqlite3_column_bytes(stmt_, index)));
}

std::optional<std::string> Statement::column_text_optional(int index) const {
    if (column_is_null(index)) {
        return std::nullopt;
    }
    const std::string value = column_text(index);
    if (value.empty()) {
        return std::nullopt;  // Empty string and NULL are equivalent here.
    }
    return value;
}

std::vector<std::uint8_t> Statement::column_blob(int index) const {
    const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(stmt_, index));
    const int size = sqlite3_column_bytes(stmt_, index);
    if (data == nullptr || size <= 0) {
        return {};
    }
    return std::vector<std::uint8_t>(data, data + size);
}

bool Statement::column_is_null(int index) const {
    return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
}

int Statement::column_count() const { return sqlite3_column_count(stmt_); }

Statement& Statement::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
    return *this;
}

Transaction::Transaction(Database& db, bool immediate) : db_(db) {
    active_ = static_cast<bool>(db_.execute(immediate ? "BEGIN IMMEDIATE" : "BEGIN"));
}

Transaction::~Transaction() {
    if (active_) {
        rollback();
    }
}

Status Transaction::commit() {
    if (!active_) {
        return core::err::internal("commit on an inactive transaction", "storage.transaction");
    }
    active_ = false;
    return db_.execute("COMMIT");
}

void Transaction::rollback() {
    if (!active_) {
        return;
    }
    active_ = false;
    if (const Status status = db_.execute("ROLLBACK"); !status) {
        core::log::warn("transaction rollback failed",
                        {core::log::field("error", status.error().to_string())});
    }
}

Database::~Database() {
    if (db_ != nullptr) {
        sqlite3_close_v2(db_);
    }
}

Database::Database(Database&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), path_(std::move(other.path_)) {}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        if (db_ != nullptr) {
            sqlite3_close_v2(db_);
        }
        db_ = std::exchange(other.db_, nullptr);
        path_ = std::move(other.path_);
    }
    return *this;
}

Result<Database> Database::open(const DatabaseOptions& options) {
    int flags = options.read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
    if (!options.read_only && options.create_if_missing) {
        flags |= SQLITE_OPEN_CREATE;
        const std::filesystem::path parent = std::filesystem::path(options.path).parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
    }

    Database database;
    database.path_ = options.path;
    if (sqlite3_open_v2(options.path.c_str(), &database.db_, flags, nullptr) != SQLITE_OK) {
        return sqlite_error(database.db_, "cannot open database", "storage.open");
    }

#if defined(CONTEXTSNAP_WITH_SQLCIPHER)
    if (options.encryption_key.has_value()) {
        // Raw-key form: the key is already 256 random bits, so SQLCipher's own
        // KDF would add cost without adding entropy.
        if (const Status keyed =
                database.execute("PRAGMA key = \"x'" + *options.encryption_key + "'\"");
            !keyed) {
            return keyed.error();
        }
    }
#endif

    const std::string pragmas = "PRAGMA journal_mode = WAL;"
                                "PRAGMA synchronous = NORMAL;"
                                "PRAGMA foreign_keys = ON;"
                                "PRAGMA temp_store = MEMORY;"
                                "PRAGMA busy_timeout = " +
                                std::to_string(options.busy_timeout_ms) + ";PRAGMA cache_size = -" +
                                std::to_string(options.cache_size_kb) + ";";
    if (const Status status = database.execute(pragmas); !status) {
        return status.error();
    }
    return database;
}

Result<Database> Database::open_memory() {
    DatabaseOptions options;
    options.path = ":memory:";
    return open(options);
}

Result<Statement> Database::prepare(std::string_view sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr) !=
        SQLITE_OK) {
        return sqlite_error(db_, "prepare failed", "storage.prepare");
    }
    return Statement(stmt, db_);
}

Status Database::execute(std::string_view sql) {
    char* message = nullptr;
    const std::string statement(sql);
    if (sqlite3_exec(db_, statement.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
        const std::string detail = message != nullptr ? message : "unknown error";
        sqlite3_free(message);
        return core::err::database("exec failed: " + detail, "storage.execute", sqlite3_errcode(db_));
    }
    return Status::success();
}

std::int64_t Database::last_insert_rowid() const { return sqlite3_last_insert_rowid(db_); }

int Database::changes() const { return sqlite3_changes(db_); }

Status Database::migrate() {
    auto current = user_version();
    if (!current) {
        return current.error();
    }
    for (const Migration& migration : migrations()) {
        if (migration.version <= current.value()) {
            continue;
        }
        core::log::info("applying migration",
                        {core::log::field("version", static_cast<std::int64_t>(migration.version)),
                         core::log::field("name", std::string(migration.name))});

        Transaction transaction(*this);
        if (const Status applied = execute(migration.sql); !applied) {
            return applied;
        }
        auto insert = prepare(
            "INSERT OR REPLACE INTO schema_migrations (version, name, applied_at, checksum) "
            "VALUES (?, ?, ?, ?)");
        if (!insert) {
            return insert.error();
        }
        insert.value()
            .bind(1, static_cast<std::int64_t>(migration.version))
            .bind(2, migration.name)
            .bind(3, now_millis())
            .bind(4, std::string_view{});
        if (const Status recorded = insert.value().execute(); !recorded) {
            return recorded;
        }
        if (const Status bumped = set_user_version(migration.version); !bumped) {
            return bumped;
        }
        if (const Status committed = transaction.commit(); !committed) {
            return committed;
        }
    }
    return Status::success();
}

Result<std::int32_t> Database::user_version() {
    auto statement = prepare("PRAGMA user_version");
    if (!statement) {
        return statement.error();
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        return stepped.error();
    }
    return stepped.value() ? static_cast<std::int32_t>(statement.value().column_int(0)) : 0;
}

Status Database::set_user_version(std::int32_t version) {
    // PRAGMA does not accept bound parameters; the value is an internal integer.
    return execute("PRAGMA user_version = " + std::to_string(version));
}

Status Database::vacuum() { return execute("VACUUM"); }

Status Database::checkpoint() { return execute("PRAGMA wal_checkpoint(TRUNCATE)"); }

Result<std::uint64_t> Database::file_size_bytes() const {
    if (path_ == ":memory:") {
        return static_cast<std::uint64_t>(0);
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path_, ec);
    if (ec) {
        return core::err::io("cannot stat database file", "storage.file_size", ec.value());
    }
    return static_cast<std::uint64_t>(size);
}

Result<bool> Database::integrity_check() {
    auto statement = prepare("PRAGMA integrity_check");
    if (!statement) {
        return statement.error();
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        return stepped.error();
    }
    return stepped.value() && statement.value().column_text(0) == "ok";
}

Status Database::rekey(std::string_view new_key) {
#if defined(CONTEXTSNAP_WITH_SQLCIPHER)
    return execute("PRAGMA rekey = \"x'" + std::string(new_key) + "'\"");
#else
    (void)new_key;
    return core::err::unsupported("rekey requires -DCONTEXTSNAP_WITH_SQLCIPHER=ON",
                                  "storage.rekey");
#endif
}

}  // namespace contextsnap::storage
