// A file moved inside the library must keep its media_id (and with it the
// playlists, BPM and history) instead of coming back as a new song; a real
// duplicate copy must stay a separate row.
#include <windows.h>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "library/db.h"
#include "library/scanner.h"

namespace fs = std::filesystem;

static int64_t one(Db& db, const char* sql) {
    Db::Stmt q;
    db.prepare(q, sql);
    return q.step() ? q.colInt(0) : -1;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / L"kdj_test_scan";
    fs::remove_all(dir);
    fs::create_directories(dir / L"Old");
    fs::create_directories(dir / L"New");
    std::ofstream(dir / L"Old" / L"Artist - Song.mp3", std::ios::binary) << "not really audio";
    std::ofstream(dir / L"Old" / L"Other.mp3", std::ios::binary) << "different size!!!!";
    {
        Db db;
        assert(db.open((dir / L"lib.db").wstring()));
        assert(scanDirectory(db, dir.wstring(), nullptr, false).added == 2);
        const int64_t id = one(db, "SELECT id FROM media_item WHERE path LIKE '%Song.mp3'");
        db.exec("UPDATE media_item SET bpm=120 WHERE path LIKE '%Song.mp3'");
        db.exec("INSERT INTO playlist(name) VALUES('p')");
        db.exec("INSERT INTO playlist_item(playlist_id,media_id,position) "
                "SELECT 1, id, 0 FROM media_item WHERE path LIKE '%Song.mp3'");

        // Move (keeps mtime) and rescan: same row, new path.
        assert(MoveFileW((dir / L"Old" / L"Artist - Song.mp3").c_str(),
                         (dir / L"New" / L"Artist - Song.mp3").c_str()));
        const ScanStats st = scanDirectory(db, dir.wstring(), nullptr, false);
        assert(st.moved == 1);
        assert(one(db, "SELECT COUNT(*) FROM media_item") == 2);
        assert(one(db, "SELECT id FROM media_item WHERE path LIKE '%New\\Artist - Song.mp3'") == id);
        assert(one(db, "SELECT bpm FROM media_item WHERE id IN (SELECT media_id FROM playlist_item)") == 120);
        assert(one(db, "SELECT COUNT(*) FROM media_item WHERE search_f LIKE '%new%'") == 1);

        // A real copy (CopyFileW keeps mtime) is a duplicate, not a move.
        assert(CopyFileW((dir / L"New" / L"Artist - Song.mp3").c_str(),
                         (dir / L"Old" / L"Artist - Song.mp3").c_str(), TRUE));
        assert(scanDirectory(db, dir.wstring(), nullptr, false).moved == 0);
        assert(one(db, "SELECT COUNT(*) FROM media_item") == 3);

        // Moved AND renamed: still the same row; the title guessed from the
        // old name follows the new one, and a ghost hidden for not loading
        // does not hide the playable file.
        std::ofstream(dir / L"Old" / L"A - X.mp3", std::ios::binary) << "a third, longer file";
        scanDirectory(db, dir.wstring(), nullptr, false);
        const int64_t x = one(db, "SELECT id FROM media_item WHERE title='X'");
        db.exec("UPDATE media_item SET hidden=1 WHERE title='X'");
        assert(MoveFileW((dir / L"Old" / L"A - X.mp3").c_str(),
                         (dir / L"New" / L"A - Y.mp3").c_str()));
        assert(scanDirectory(db, dir.wstring(), nullptr, false).moved == 1);
        assert(one(db, "SELECT id FROM media_item WHERE title='Y' AND hidden=0") == x);
        assert(one(db, "SELECT COUNT(*) FROM media_item") == 4);
    }
    fs::remove_all(dir);
    std::puts("test_scan OK");
    return 0;
}
