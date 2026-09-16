// Library CLI (Phase 3): scan folders into SQLite, search, stats, saved
// playlists (FR-005), and handoff to the two-deck player.
#include <windows.h>
#include <objbase.h>

#include <fcntl.h>
#include <io.h>
#include <process.h>

#include <cstdio>
#include <string>
#include <vector>

#include "library/db.h"
#include "library/media_query.h"
#include "library/scanner.h"

static int spawnPlayer(const std::vector<std::wstring>& paths,
                       const std::vector<std::wstring>& passthrough) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring player(self);
    player = player.substr(0, player.find_last_of(L"\\/") + 1) + L"video_harness.exe";

    auto quote = [](const std::wstring& s) {
        return s.find_first_of(L" \t") == std::wstring::npos ? s : L"\"" + s + L"\"";
    };
    std::vector<std::wstring> argStore;
    argStore.push_back(quote(player));
    for (auto& p : paths) argStore.push_back(quote(p));
    for (auto& e : passthrough) argStore.push_back(quote(e));
    std::vector<const wchar_t*> argp;
    for (auto& a : argStore) argp.push_back(a.c_str());
    argp.push_back(nullptr);
    fflush(stdout);
    return int(_wspawnv(_P_WAIT, player.c_str(), argp.data()));
}

// Words up to the first -flag; flags (and onward) land in passthrough.
static std::wstring joinTerm(const std::vector<std::wstring>& args, size_t from,
                             std::vector<std::wstring>& passthrough) {
    std::wstring term;
    for (size_t i = from; i < args.size(); ++i) {
        if (!args[i].empty() && args[i][0] == L'-') {
            passthrough.assign(args.begin() + i, args.end());
            break;
        }
        if (!term.empty()) term += L' ';
        term += args[i];
    }
    return term;
}

static int64_t playlistId(Db& db, const std::wstring& name) {
    Db::Stmt q;
    db.prepare(q, "SELECT id FROM playlist WHERE name=?1");
    q.bind(1, utf8(name));
    return q.step() ? q.colInt(0) : -1;
}

static void touchPlaylist(Db& db, int64_t id) {
    Db::Stmt q;
    db.prepare(q, "UPDATE playlist SET updated_at=strftime('%s','now') WHERE id=?1");
    q.bind(1, id);
    q.step();
}

static int playlistCmd(Db& db, const std::vector<std::wstring>& args) {
    if (args.size() < 2) {
        wprintf(L"usage: librarian playlist create|delete|add|remove|move|show|list|"
                L"play ...\n");
        return 2;
    }
    const std::wstring sub = args[1];

    if (sub == L"list") {
        Db::Stmt q;
        db.prepare(q, "SELECT p.name, COUNT(pi.id), IFNULL(SUM(m.duration_ms),0) "
                      "FROM playlist p LEFT JOIN playlist_item pi ON pi.playlist_id=p.id "
                      "LEFT JOIN media_item m ON m.id=pi.media_id "
                      "GROUP BY p.id ORDER BY p.name");
        while (q.step())
            wprintf(L"%-24ls %3lld tracks  %5.1f min\n", wide(q.colText(0)).c_str(),
                    q.colInt(1), double(q.colInt(2)) / 60000.0);
        return 0;
    }

    if (args.size() < 3) { wprintf(L"playlist %ls: need a playlist name\n", sub.c_str()); return 2; }
    const std::wstring name = args[2];

    if (sub == L"create") {
        Db::Stmt q;
        db.prepare(q, "INSERT INTO playlist(name,created_at,updated_at) "
                      "VALUES(?1,strftime('%s','now'),strftime('%s','now'))");
        q.bind(1, utf8(name));
        q.step();
        wprintf(playlistId(db, name) >= 0 ? L"created '%ls'\n" : L"failed: '%ls' exists?\n",
                name.c_str());
        return 0;
    }

    const int64_t pid = playlistId(db, name);
    if (pid < 0) { wprintf(L"no playlist named '%ls'\n", name.c_str()); return 1; }

    if (sub == L"delete") {
        Db::Stmt q;
        db.prepare(q, "DELETE FROM playlist WHERE id=?1"); // items cascade
        q.bind(1, pid);
        q.step();
        wprintf(L"deleted '%ls'\n", name.c_str());
        return 0;
    }

    if (sub == L"add") {
        std::vector<std::wstring> ignored;
        const std::wstring term = joinTerm(args, 3, ignored);
        if (term.empty()) { wprintf(L"add: need a search term\n"); return 2; }
        const auto matches = searchMedia(db, term);
        if (matches.empty()) { wprintf(L"no matches for '%ls'\n", term.c_str()); return 1; }
        for (const auto& m : matches) {
            Db::Stmt q;
            db.prepare(q, "INSERT INTO playlist_item(playlist_id,media_id,position) "
                          "VALUES(?1,?2,(SELECT IFNULL(MAX(position),0)+1 FROM "
                          "playlist_item WHERE playlist_id=?1))");
            q.bind(1, pid).bind(2, m.id);
            q.step();
            wprintf(L"  + %ls\n", m.label.c_str());
        }
        touchPlaylist(db, pid);
        wprintf(L"added %zu track(s) to '%ls'\n", matches.size(), name.c_str());
        return 0;
    }

    if (sub == L"show") {
        Db::Stmt q;
        db.prepare(q, "SELECT pi.position, m.artist, m.title, m.type, m.duration_ms "
                      "FROM playlist_item pi JOIN media_item m ON m.id=pi.media_id "
                      "WHERE pi.playlist_id=?1 ORDER BY pi.position");
        q.bind(1, pid);
        while (q.step()) {
            const int64_t d = q.colInt(4);
            wprintf(L"%3lld. [%-5hs %lld:%02lld] %ls - %ls\n", q.colInt(0),
                    q.colText(3).c_str(), d / 60000, d / 1000 % 60,
                    wide(q.colText(1)).c_str(), wide(q.colText(2)).c_str());
        }
        return 0;
    }

    if (sub == L"remove" && args.size() >= 4) {
        const int64_t pos = _wtoi(args[3].c_str());
        Db::Stmt d;
        db.prepare(d, "DELETE FROM playlist_item WHERE playlist_id=?1 AND position=?2");
        d.bind(1, pid).bind(2, pos);
        d.step();
        Db::Stmt r;
        db.prepare(r, "UPDATE playlist_item SET position=position-1 "
                      "WHERE playlist_id=?1 AND position>?2");
        r.bind(1, pid).bind(2, pos);
        r.step();
        touchPlaylist(db, pid);
        wprintf(L"removed #%lld from '%ls'\n", pos, name.c_str());
        return 0;
    }

    if (sub == L"move" && args.size() >= 5) {
        const int64_t from = _wtoi(args[3].c_str()), to = _wtoi(args[4].c_str());
        Db::Stmt g;
        db.prepare(g, "SELECT id FROM playlist_item WHERE playlist_id=?1 AND position=?2");
        g.bind(1, pid).bind(2, from);
        if (!g.step()) { wprintf(L"no item at position %lld\n", from); return 1; }
        const int64_t itemId = g.colInt(0);
        Db::Stmt s;
        if (from < to) {
            db.prepare(s, "UPDATE playlist_item SET position=position-1 "
                          "WHERE playlist_id=?1 AND position>?2 AND position<=?3");
            s.bind(1, pid).bind(2, from).bind(3, to);
        } else {
            db.prepare(s, "UPDATE playlist_item SET position=position+1 "
                          "WHERE playlist_id=?1 AND position>=?3 AND position<?2");
            s.bind(1, pid).bind(2, from).bind(3, to);
        }
        s.step();
        Db::Stmt u;
        db.prepare(u, "UPDATE playlist_item SET position=?2 WHERE id=?1");
        u.bind(1, itemId).bind(2, to);
        u.step();
        touchPlaylist(db, pid);
        wprintf(L"moved %lld -> %lld in '%ls'\n", from, to, name.c_str());
        return 0;
    }

    if (sub == L"play") {
        std::vector<std::wstring> passthrough;
        joinTerm(args, 3, passthrough); // name is args[2]; rest = player flags
        Db::Stmt q;
        db.prepare(q, "SELECT m.path FROM playlist_item pi "
                      "JOIN media_item m ON m.id=pi.media_id "
                      "WHERE pi.playlist_id=?1 ORDER BY pi.position");
        q.bind(1, pid);
        std::vector<std::wstring> paths;
        while (q.step()) paths.push_back(wide(q.colText(0)));
        if (paths.empty()) { wprintf(L"'%ls' is empty\n", name.c_str()); return 1; }
        wprintf(L"playing '%ls' (%zu tracks)\n", name.c_str(), paths.size());
        return spawnPlayer(paths, passthrough);
    }

    wprintf(L"unknown playlist command: %ls\n", sub.c_str());
    return 2;
}

// Next waiting singer, or id -1.
struct NextUp {
    int64_t itemId = -1, mediaId = -1, keyShift = 0;
    std::wstring singer, label, path;
};
static NextUp nextWaiting(Db& db) {
    NextUp n;
    Db::Stmt q;
    db.prepare(q, "SELECT sq.id, sq.singer, sq.key_shift, m.id, m.path, m.artist, m.title "
                  "FROM singer_queue_item sq JOIN media_item m ON m.id=sq.media_id "
                  "WHERE sq.status='waiting' ORDER BY sq.position LIMIT 1");
    if (!q.step()) return n;
    n.itemId = q.colInt(0);
    n.singer = wide(q.colText(1));
    n.keyShift = q.colInt(2);
    n.mediaId = q.colInt(3);
    n.path = wide(q.colText(4));
    n.label = wide(q.colText(5)) + L" - " + wide(q.colText(6));
    return n;
}

static void setSingerStatus(Db& db, int64_t itemId, const char* status) {
    Db::Stmt q;
    db.prepare(q, "UPDATE singer_queue_item SET status=?2 WHERE id=?1");
    q.bind(1, itemId).bind(2, std::string(status));
    q.step();
}

// Plays one singer: status/history bookkeeping around the player run.
// Returns false when the rotation had no waiting singer.
static bool playNextSinger(Db& db, const std::vector<std::wstring>& flags) {
    const NextUp n = nextWaiting(db);
    if (n.itemId < 0) { wprintf(L"rotation: no waiting singers\n"); return false; }
    wchar_t key[16] = L"";
    if (n.keyShift) swprintf(key, 16, L"  key %+lld", n.keyShift);
    wprintf(L"NOW SINGING: %ls  ->  %ls%ls\n", n.singer.c_str(), n.label.c_str(), key);
    setSingerStatus(db, n.itemId, "singing");
    Db::Stmt h;
    db.prepare(h, "INSERT INTO play_history(media_id,singer,started_at) "
                  "VALUES(?1,?2,strftime('%s','now'))");
    h.bind(1, n.mediaId).bind(2, utf8(n.singer));
    h.step();
    const int64_t histId = db.lastId();
    const int rc = spawnPlayer({n.path}, flags);
    setSingerStatus(db, n.itemId, rc == 0 ? "completed" : "waiting"); // failed run requeues
    Db::Stmt u;
    db.prepare(u, "UPDATE play_history SET completed_at=strftime('%s','now'),result=?2 "
                  "WHERE id=?1");
    u.bind(1, histId).bind(2, std::string(rc == 0 ? "ok" : "error"));
    u.step();
    return true;
}

static int rotationCmd(Db& db, const std::vector<std::wstring>& args) {
    if (args.size() < 2) {
        wprintf(L"usage: librarian rotation add <singer> <song term> [--key n] "
                L"[--notes t] | list | next [flags] | run [--filler <playlist>] "
                L"[flags] | skip <pos> | noshow <pos> | remove <pos> | clear\n");
        return 2;
    }
    const std::wstring sub = args[1];

    if (sub == L"add") {
        if (args.size() < 4) { wprintf(L"add: rotation add <singer> <song term>\n"); return 2; }
        const std::wstring singer = args[2];
        std::wstring term, notes;
        int64_t key = 0;
        for (size_t i = 3; i < args.size(); ++i) {
            if (args[i] == L"--key" && i + 1 < args.size()) key = _wtoi(args[++i].c_str());
            else if (args[i] == L"--notes" && i + 1 < args.size()) notes = args[++i];
            else { if (!term.empty()) term += L' '; term += args[i]; }
        }
        const auto matches = searchMedia(db, term);
        if (matches.empty()) { wprintf(L"no song matches '%ls'\n", term.c_str()); return 1; }
        Db::Stmt q;
        db.prepare(q, "INSERT INTO singer_queue_item(singer,media_id,key_shift,notes,"
                      "position) VALUES(?1,?2,?3,?4,(SELECT IFNULL(MAX(position),0)+1 "
                      "FROM singer_queue_item))");
        q.bind(1, utf8(singer)).bind(2, matches[0].id).bind(3, key).bind(4, utf8(notes));
        q.step();
        wprintf(L"queued %ls -> %ls\n", singer.c_str(), matches[0].label.c_str());
        return 0;
    }

    if (sub == L"list") {
        Db::Stmt q;
        db.prepare(q, "SELECT sq.position, sq.singer, sq.status, sq.key_shift, "
                      "m.artist, m.title FROM singer_queue_item sq "
                      "LEFT JOIN media_item m ON m.id=sq.media_id ORDER BY sq.position");
        bool onDeckShown = false;
        while (q.step()) {
            const std::string status = q.colText(2);
            std::wstring shown = wide(status);
            if (status == "waiting" && !onDeckShown) { shown = L"on deck"; onDeckShown = true; }
            wchar_t key[16] = L"";
            if (q.colInt(3)) swprintf(key, 16, L"  key %+lld", q.colInt(3));
            wprintf(L"%3lld. %-10ls %-12ls %ls - %ls%ls\n", q.colInt(0), shown.c_str(),
                    wide(q.colText(1)).c_str(), wide(q.colText(4)).c_str(),
                    wide(q.colText(5)).c_str(), key);
        }
        return 0;
    }

    if (sub == L"next") {
        std::vector<std::wstring> flags;
        joinTerm(args, 2, flags);
        return playNextSinger(db, flags) ? 0 : 1;
    }

    if (sub == L"run") {
        std::wstring filler;
        std::vector<std::wstring> flags;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == L"--filler" && i + 1 < args.size()) filler = args[++i];
            else flags.push_back(args[i]);
        }
        std::vector<std::wstring> fillerPaths;
        if (!filler.empty()) {
            const int64_t pid = playlistId(db, filler);
            if (pid < 0) { wprintf(L"no playlist named '%ls'\n", filler.c_str()); return 1; }
            Db::Stmt q;
            db.prepare(q, "SELECT m.path FROM playlist_item pi JOIN media_item m ON "
                          "m.id=pi.media_id WHERE pi.playlist_id=?1 ORDER BY pi.position");
            q.bind(1, pid);
            while (q.step()) fillerPaths.push_back(wide(q.colText(0)));
        }
        size_t fillerIdx = 0;
        while (playNextSinger(db, flags)) {
            if (nextWaiting(db).itemId < 0) break; // nobody left: no filler needed
            if (!fillerPaths.empty()) {
                wprintf(L"-- filler music --\n");
                spawnPlayer({fillerPaths[fillerIdx++ % fillerPaths.size()]}, flags);
            }
        }
        wprintf(L"rotation finished\n");
        return 0;
    }

    if ((sub == L"skip" || sub == L"noshow" || sub == L"remove") && args.size() >= 3) {
        const int64_t pos = _wtoi(args[2].c_str());
        if (sub == L"remove") {
            Db::Stmt d;
            db.prepare(d, "DELETE FROM singer_queue_item WHERE position=?1");
            d.bind(1, pos);
            d.step();
            Db::Stmt r;
            db.prepare(r, "UPDATE singer_queue_item SET position=position-1 WHERE position>?1");
            r.bind(1, pos);
            r.step();
        } else {
            Db::Stmt q;
            db.prepare(q, "UPDATE singer_queue_item SET status=?2 WHERE position=?1");
            q.bind(1, pos).bind(2, std::string(sub == L"skip" ? "skipped" : "noshow"));
            q.step();
        }
        wprintf(L"%ls #%lld\n", sub.c_str(), pos);
        return 0;
    }

    if (sub == L"clear") {
        db.exec("DELETE FROM singer_queue_item;");
        wprintf(L"rotation cleared\n");
        return 0;
    }

    wprintf(L"unknown rotation command: %ls\n", sub.c_str());
    return 2;
}

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT); // accented titles survive the console
    std::wstring dbPath = defaultDbPath();
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--db" && i + 1 < argc) dbPath = argv[++i];
        else args.push_back(a);
    }
    if (args.empty()) {
        wprintf(L"usage: librarian scan <dir>... | search <term>... | stats\n"
                L"       librarian play <term>... [player options e.g. --fade 3]\n"
                L"       librarian playlist create|delete|add|remove|move|show|list|play\n"
                L"       librarian rotation add|list|next|run|skip|noshow|remove|clear\n"
                L"       librarian history\n"
                L"       (--db file to use another database)\n");
        return 2;
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Db db;
    if (!db.open(dbPath)) { wprintf(L"cannot open %ls\n", dbPath.c_str()); return 1; }
    const std::wstring cmd = args[0];

    if (cmd == L"scan") {
        if (args.size() < 2) { wprintf(L"scan: need at least one directory\n"); return 2; }
        for (size_t i = 1; i < args.size(); ++i) {
            wprintf(L"scanning %ls\n", args[i].c_str());
            const ScanStats st = scanDirectory(db, args[i]);
            wprintf(L"  seen=%d added=%d updated=%d unchanged=%d unsupported=%d\n",
                    st.seen, st.added, st.updated, st.unchanged, st.unsupported);
        }
        return 0;
    }

    if (cmd == L"stats") {
        Db::Stmt q;
        db.prepare(q, "SELECT type, COUNT(*), SUM(duration_ms) FROM media_item "
                      "GROUP BY type ORDER BY 2 DESC");
        while (q.step())
            wprintf(L"%-12hs %6lld items  %8.1f h\n", q.colText(0).c_str(), q.colInt(1),
                    double(q.colInt(2)) / 3600000.0);
        return 0;
    }

    if (cmd == L"search") {
        std::vector<std::wstring> ignored;
        const std::wstring term = joinTerm(args, 1, ignored);
        const auto matches = searchMedia(db, term);
        for (const auto& m : matches) wprintf(L"%5lld  %ls\n       %ls\n", m.id,
                                              m.label.c_str(), m.path.c_str());
        wprintf(L"%zu result(s)\n", matches.size());
        return 0;
    }

    if (cmd == L"play") {
        std::vector<std::wstring> passthrough;
        const std::wstring term = joinTerm(args, 1, passthrough);
        if (term.empty()) { wprintf(L"play: need a search term\n"); return 2; }
        const auto matches = searchMedia(db, term);
        if (matches.empty()) { wprintf(L"no matches for '%ls'\n", term.c_str()); return 1; }
        std::vector<std::wstring> paths;
        for (const auto& m : matches) {
            wprintf(L"%2zu. %ls\n", paths.size() + 1, m.label.c_str());
            paths.push_back(m.path);
        }
        return spawnPlayer(paths, passthrough);
    }

    if (cmd == L"playlist") return playlistCmd(db, args);
    if (cmd == L"rotation") return rotationCmd(db, args);

    if (cmd == L"history") {
        Db::Stmt q;
        db.prepare(q, "SELECT h.singer, m.artist, m.title, "
                      "datetime(h.started_at,'unixepoch','localtime'), h.result "
                      "FROM play_history h LEFT JOIN media_item m ON m.id=h.media_id "
                      "ORDER BY h.id DESC LIMIT 20");
        while (q.step())
            wprintf(L"%ls  %-10ls %ls - %ls  [%hs]\n", wide(q.colText(3)).c_str(),
                    wide(q.colText(0)).c_str(), wide(q.colText(1)).c_str(),
                    wide(q.colText(2)).c_str(), q.colText(4).c_str());
        return 0;
    }

    wprintf(L"unknown command: %ls\n", cmd.c_str());
    return 2;
}
