#include "library/db.h"

#include <windows.h>

#include "sqlite3.h"

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

std::wstring foldW(const std::wstring& s) {
    if (s.empty()) return {};
    // NFD decomposition splits "é" into "e" + combining accent…
    int n = NormalizeString(NormalizationD, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring d(size_t((std::max)(n, 0)) + 8, 0);
    n = NormalizeString(NormalizationD, s.c_str(), int(s.size()), d.data(),
                        int(d.size()));
    if (n <= 0) d = s; // normalization unavailable: fold case only
    else d.resize(n);
    std::wstring out;
    out.reserve(d.size());
    for (wchar_t c : d) {
        if (c >= 0x0300 && c <= 0x036F) continue; // …and this drops the accents
        out += wchar_t(towlower(c));
    }
    return out;
}

// SQL fold(x): the C++ folding exposed to queries (deterministic → hoistable).
static void sqlFold(sqlite3_context* ctx, int, sqlite3_value** argv) {
    const unsigned char* t = sqlite3_value_text(argv[0]);
    if (!t) {
        sqlite3_result_null(ctx);
        return;
    }
    const std::string folded = utf8(foldW(wide(reinterpret_cast<const char*>(t))));
    sqlite3_result_text(ctx, folded.c_str(), int(folded.size()), SQLITE_TRANSIENT);
}

std::wstring defaultDbPath() {
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = n ? std::wstring(buf) + L"\\KaraokeDJ" : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\library.db";
}

static const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS media_item(
  id INTEGER PRIMARY KEY,
  path TEXT UNIQUE NOT NULL,
  type TEXT NOT NULL,            -- audio | video | mp3g | karaoke_zip | unsupported
  title TEXT, artist TEXT,
  duration_ms INTEGER DEFAULT 0,
  file_size INTEGER, modified_time INTEGER,
  status TEXT DEFAULT 'ok',
  genre TEXT, year INTEGER
);
CREATE INDEX IF NOT EXISTS idx_media_title ON media_item(title);
CREATE INDEX IF NOT EXISTS idx_media_artist ON media_item(artist);
CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS playlist(
  id INTEGER PRIMARY KEY,
  name TEXT UNIQUE NOT NULL,
  created_at INTEGER, updated_at INTEGER
);
CREATE TABLE IF NOT EXISTS playlist_item(
  id INTEGER PRIMARY KEY,
  playlist_id INTEGER NOT NULL REFERENCES playlist(id) ON DELETE CASCADE,
  media_id INTEGER NOT NULL REFERENCES media_item(id),
  position INTEGER NOT NULL,
  cue_in_ms INTEGER DEFAULT 0, cue_out_ms INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_pl_item ON playlist_item(playlist_id, position);
CREATE TABLE IF NOT EXISTS singer_queue_item(
  id INTEGER PRIMARY KEY,
  singer TEXT NOT NULL,
  media_id INTEGER REFERENCES media_item(id),
  key_shift INTEGER DEFAULT 0,
  notes TEXT DEFAULT '',
  status TEXT DEFAULT 'waiting',  -- waiting|singing|completed|skipped|noshow
  position INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS scan_root(path TEXT PRIMARY KEY);
CREATE TABLE IF NOT EXISTS play_history(
  id INTEGER PRIMARY KEY,
  media_id INTEGER,
  singer TEXT,
  started_at INTEGER,
  completed_at INTEGER,
  result TEXT
);
)sql";

bool Db::open(const std::wstring& path) {
    close();
    if (sqlite3_open(utf8(path).c_str(), &db_) != SQLITE_OK) { close(); return false; }
    sqlite3_busy_timeout(db_, 3000); // background import scans share the file
    sqlite3_create_function(db_, "fold", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                            nullptr, sqlFold, nullptr, nullptr);
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");
    if (!exec(kSchema)) return false;
    // migrations for databases created before these columns existed
    exec("ALTER TABLE media_item ADD COLUMN genre TEXT;");
    exec("ALTER TABLE media_item ADD COLUMN year INTEGER;");
    exec("ALTER TABLE media_item ADD COLUMN cue_in_ms INTEGER DEFAULT 0;");
    exec("ALTER TABLE media_item ADD COLUMN cue_out_ms INTEGER DEFAULT 0;");
    exec("ALTER TABLE media_item ADD COLUMN bpm INTEGER DEFAULT 0;");
    // music_key: 0 = never analysed, -1 = analysed with no clear key,
    // 1..24 = tonic 0..11 (C..B) major then the same twelve minor, all +1.
    exec("ALTER TABLE media_item ADD COLUMN music_key INTEGER DEFAULT 0;");
    // Pre-folded search text (title\nartist\npath): a 100k-row library
    // cannot afford the NFD fold() per row per keystroke, so it's folded
    // once here / at insert and searched as plain text. Kept in sync by the
    // scanner upsert and the tag editor.
    exec("ALTER TABLE media_item ADD COLUMN search_f TEXT;");
    // hidden=1: excluded from search/browse/phone requests (broken versions);
    // restorable from the sidebar's Excluded view.
    exec("ALTER TABLE media_item ADD COLUMN hidden INTEGER DEFAULT 0;");
    exec("UPDATE media_item SET search_f = fold(COALESCE(title,'')) || "
         "char(10) || fold(COALESCE(artist,'')) || char(10) || fold(path) "
         "WHERE search_f IS NULL;");
    return true;
}

void Db::close() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

bool Db::exec(const char* sql) {
    if (!db_) return false;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) busy_ = true;
    return rc == SQLITE_OK;
}

int64_t Db::lastId() { return db_ ? sqlite3_last_insert_rowid(db_) : 0; }

bool Db::prepare(Stmt& st, const char* sql) {
    if (st.s) { sqlite3_finalize(st.s); st.s = nullptr; }
    st.owner = this;
    if (!db_) return false;
    const int rc = sqlite3_prepare_v2(db_, sql, -1, &st.s, nullptr);
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) busy_ = true;
    return rc == SQLITE_OK;
}

Db::Stmt::~Stmt() {
    if (s) sqlite3_finalize(s);
}

Db::Stmt& Db::Stmt::bind(int idx, int64_t v) {
    sqlite3_bind_int64(s, idx, v);
    return *this;
}

Db::Stmt& Db::Stmt::bind(int idx, const std::string& v) {
    sqlite3_bind_text(s, idx, v.c_str(), int(v.size()), SQLITE_TRANSIENT);
    return *this;
}

Db::Stmt& Db::Stmt::bindNull(int idx) {
    sqlite3_bind_null(s, idx);
    return *this;
}

bool Db::Stmt::step() {
    const int rc = sqlite3_step(s);
    if ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) && owner) owner->busy_ = true;
    return rc == SQLITE_ROW;
}

bool Db::Stmt::run() {
    const int rc = sqlite3_step(s);
    if ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) && owner) owner->busy_ = true;
    return rc == SQLITE_DONE || rc == SQLITE_ROW;
}

int64_t Db::Stmt::colInt(int i) const { return sqlite3_column_int64(s, i); }

std::string Db::Stmt::colText(int i) const {
    const unsigned char* t = sqlite3_column_text(s, i);
    return t ? reinterpret_cast<const char*>(t) : "";
}
