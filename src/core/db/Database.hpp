#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace fo4mx::db {

class Statement {
public:
    Statement() = default;
    Statement(sqlite3* db, std::string_view sql);
    ~Statement();
    Statement(Statement&& o) noexcept;
    Statement& operator=(Statement&& o) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& bind(int index, int64_t v);
    Statement& bind(int index, int v) { return bind(index, static_cast<int64_t>(v)); }
    Statement& bind(int index, uint32_t v) { return bind(index, static_cast<int64_t>(v)); }
    Statement& bind(int index, uint64_t v) { return bind(index, static_cast<int64_t>(v)); }
    Statement& bind(int index, double v);
    Statement& bind(int index, std::string_view v);
    Statement& bind(int index, const char* v) { return bind(index, std::string_view(v)); }
    Statement& bind(int index, const std::string& v) { return bind(index, std::string_view(v)); }
    Statement& bindNull(int index);
    Statement& bindBlob(int index, const void* data, size_t size);

    // Returns true while rows are available.
    bool step();
    // Executes to completion and resets for reuse.
    void run();
    void reset();

    int64_t columnInt(int col) const;
    double columnDouble(int col) const;
    std::string columnText(int col) const;
    bool columnIsNull(int col) const;
    std::vector<uint8_t> columnBlob(int col) const;

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

class Database {
public:
    Database() = default;
    explicit Database(const std::filesystem::path& path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void open(const std::filesystem::path& path);
    void close();
    bool isOpen() const { return db_ != nullptr; }

    void exec(std::string_view sql);
    Statement prepare(std::string_view sql);
    int64_t lastInsertId() const;
    std::optional<int64_t> scalar(std::string_view sql);

    void begin() { exec("BEGIN IMMEDIATE"); }
    void commit() { exec("COMMIT"); }
    void rollback() { exec("ROLLBACK"); }

    sqlite3* handle() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(Database& db) : db_(db) { db_.begin(); }
    ~Transaction()
    {
        if (!done_) {
            try {
                db_.rollback();
            } catch (...) {
            }
        }
    }
    void commit()
    {
        db_.commit();
        done_ = true;
    }

private:
    Database& db_;
    bool done_ = false;
};

} // namespace fo4mx::db
