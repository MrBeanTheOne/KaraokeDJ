#pragma once
#include <cstdint>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

std::string utf8(const std::wstring& w);
std::wstring wide(const std::string& s);

// Search folding: lowercase + strip diacritics ("Église" -> "eglise").
// Registered on every Db connection as the SQL function fold(x).
std::wstring foldW(const std::wstring& s);

// Canonical library location: %APPDATA%\KaraokeDJ\library.db (dir created).
// One fixed path — a CWD-relative database ends up duplicated and stale.
std::wstring defaultDbPath();

// Minimal SQLite wrapper: open+schema, exec, and a prepared statement with
// positional binds. Just enough for the library; grows as features need it.
class Db {
public:
    bool open(const std::wstring& path); // creates schema if missing
    void close();
    ~Db() { close(); }

    bool exec(const char* sql);
    int64_t lastId();

    class Stmt {
    public:
        ~Stmt();
        Stmt& bind(int idx, int64_t v);
        Stmt& bind(int idx, const std::string& v);
        Stmt& bindNull(int idx);
        bool step(); // true while a row is available
        // For statements that return no rows: true unless SQLite reported an
        // error. step() answers "did a row come back", which is ALWAYS false
        // for an UPDATE, so it cannot tell success from a failed constraint —
        // callers that need to roll back must use this.
        bool run();
        int64_t colInt(int i) const;
        std::string colText(int i) const;
        sqlite3_stmt* s = nullptr;
    };
    bool prepare(Stmt& st, const char* sql);

private:
    sqlite3* db_ = nullptr;
};
