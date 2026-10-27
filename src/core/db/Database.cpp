#include "db/Database.hpp"

#include "util/Text.hpp"

#include <sqlite3.h>
#include <stdexcept>

namespace fo4mx::db {

namespace {
[[noreturn]] void raise(sqlite3* db, std::string_view what)
{
    throw std::runtime_error(std::string(what) + ": " + (db ? sqlite3_errmsg(db) : "no database"));
}
} // namespace

Statement::Statement(sqlite3* db, std::string_view sql) : db_(db)
{
    if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr) != SQLITE_OK)
        raise(db, "prepare failed for: " + std::string(sql.substr(0, 120)));
}

Statement::~Statement()
{
    if (stmt_)
        sqlite3_finalize(stmt_);
}

Statement::Statement(Statement&& o) noexcept : db_(o.db_), stmt_(o.stmt_)
{
    o.stmt_ = nullptr;
}

Statement& Statement::operator=(Statement&& o) noexcept
{
    if (this != &o) {
        if (stmt_)
            sqlite3_finalize(stmt_);
        db_ = o.db_;
        stmt_ = o.stmt_;
        o.stmt_ = nullptr;
    }
    return *this;
}

Statement& Statement::bind(int index, int64_t v)
{
    sqlite3_bind_int64(stmt_, index, v);
    return *this;
}

Statement& Statement::bind(int index, double v)
{
    sqlite3_bind_double(stmt_, index, v);
    return *this;
}

Statement& Statement::bind(int index, std::string_view v)
{
    sqlite3_bind_text(stmt_, index, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    return *this;
}

Statement& Statement::bindBlob(int index, const void* data, size_t size)
{
    sqlite3_bind_blob64(stmt_, index, data, static_cast<sqlite3_uint64>(size), SQLITE_TRANSIENT);
    return *this;
}

std::vector<uint8_t> Statement::columnBlob(int col) const
{
    const auto* p = static_cast<const uint8_t*>(sqlite3_column_blob(stmt_, col));
    int n = sqlite3_column_bytes(stmt_, col);
    return p ? std::vector<uint8_t>(p, p + n) : std::vector<uint8_t>();
}

Statement& Statement::bindNull(int index)
{
    sqlite3_bind_null(stmt_, index);
    return *this;
}

bool Statement::step()
{
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW)
        return true;
    if (rc == SQLITE_DONE)
        return false;
    raise(db_, "step failed");
}

void Statement::run()
{
    while (step()) {
    }
    reset();
}

void Statement::reset()
{
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

int64_t Statement::columnInt(int col) const
{
    return sqlite3_column_int64(stmt_, col);
}

double Statement::columnDouble(int col) const
{
    return sqlite3_column_double(stmt_, col);
}

std::string Statement::columnText(int col) const
{
    const auto* t = sqlite3_column_text(stmt_, col);
    return t ? std::string(reinterpret_cast<const char*>(t), static_cast<size_t>(sqlite3_column_bytes(stmt_, col)))
             : std::string();
}

bool Statement::columnIsNull(int col) const
{
    return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

Database::Database(const std::filesystem::path& path)
{
    open(path);
}

Database::~Database()
{
    close();
}

void Database::open(const std::filesystem::path& path)
{
    close();
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    if (sqlite3_open(pathToUtf8(path).c_str(), &db_) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("cannot open database " + pathToUtf8(path) + ": " + msg);
    }
    sqlite3_busy_timeout(db_, 10000);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("PRAGMA foreign_keys=ON");
    exec("PRAGMA temp_store=MEMORY");
}

void Database::close()
{
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

void Database::exec(std::string_view sql)
{
    char* err = nullptr;
    std::string s(sql);
    if (sqlite3_exec(db_, s.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("SQL error: " + msg + " in: " + s.substr(0, 160));
    }
}

Statement Database::prepare(std::string_view sql)
{
    return Statement(db_, sql);
}

int64_t Database::lastInsertId() const
{
    return sqlite3_last_insert_rowid(db_);
}

std::optional<int64_t> Database::scalar(std::string_view sql)
{
    Statement st(db_, sql);
    if (st.step() && !st.columnIsNull(0))
        return st.columnInt(0);
    return std::nullopt;
}

} // namespace fo4mx::db
