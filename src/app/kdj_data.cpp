#include "app/kdj.h"

// Settings, library queries and the session snapshot.
//
// Everything that runs as a background job or blocks on an OS dialog
// (import/scan/watcher, BPM+key analysis, update check, folder and file
// pickers, profile transfer, YouTube) lives in kdj_jobs.cpp.
// -------------------------------------------------------------- settings

std::string getSetting(Db& db, const char* key, const std::string& def) {
    Db::Stmt q;
    db.prepare(q, "SELECT value FROM settings WHERE key=?1");
    q.bind(1, std::string(key));
    return q.step() ? q.colText(0) : def;
}

void setSetting(Db& db, const char* key, const std::string& v) {
    Db::Stmt q;
    db.prepare(q, "INSERT INTO settings(key,value) VALUES(?1,?2) "
                  "ON CONFLICT(key) DO UPDATE SET value=?2");
    q.bind(1, std::string(key)).bind(2, v);
    q.step();
}

void loadSettings(App& a, UINT& winW, UINT& winH) {
    a.fadeSec = atof(getSetting(a.db, "fade_sec", "3.0").c_str());
    a.automixOn = getSetting(a.db, "automix_on", "1") == "1";
    a.repeatOn = getSetting(a.db, "repeat_on", "0") == "1";
    a.mixMode = getSetting(a.db, "mix_mode", "fade") == "smart" ? MixMode::Smart
                                                                : MixMode::Fade;
    a.chosenMonitor = atoi(getSetting(a.db, "video_monitor", "-1").c_str());
    a.singerName = wide(getSetting(a.db, "singer_name", "Guest"));
    a.sidebarOpen = getSetting(a.db, "sidebar_open", "1") == "1";
    a.queueOpen = getSetting(a.db, "queue_open", "1") == "1";
    a.sideW = std::clamp(float(atof(getSetting(a.db, "side_w", "220").c_str())), 160.f,
                         420.f);
    a.queueW = std::clamp(float(atof(getSetting(a.db, "queue_w", "330").c_str())),
                          300.f, 560.f);
    { // queue columns: plain width shares, "0.50,0.32,0.18"
        const std::string qs = getSetting(a.db, "queue_cols", "");
        float f[App::kQueueCols]{};
        if (sscanf_s(qs.c_str(), "%f,%f,%f", &f[0], &f[1], &f[2]) ==
            App::kQueueCols) {
            float sum = 0.f;
            for (float v : f) sum += v;
            if (sum > 0.f)
                for (int k = 0; k < App::kQueueCols; ++k)
                    a.qColFrac[k] = std::clamp(f[k] / sum, 0.08f, 0.84f);
        }
    }
    { // columns: "seq|show|frac" triplets, e.g. "0:1:0.34,1:1:0.24,..."
        const std::string cs = getSetting(a.db, "columns", "");
        int idx = 0;
        size_t pos = 0;
        while (idx < App::kNumCols && pos < cs.size()) {
            int id = 0, show = 1;
            float frac = 0.1f;
            if (sscanf_s(cs.c_str() + pos, "%d:%d:%f", &id, &show, &frac) == 3 &&
                id >= 0 && id < App::kNumCols) {
                a.colSeq[idx] = id;
                a.colShow[id] = show != 0;
                a.colFrac[id] = std::clamp(frac, 0.04f, 0.9f);
                ++idx;
            }
            const size_t c = cs.find(',', pos);
            if (c == std::string::npos) break;
            pos = c + 1;
        }
        if (idx > 0) {
            // Rebuild the order as a proper permutation: the ids the saved
            // layout named, in its order, then any it never mentioned. A
            // layout written by an older build lists fewer columns than we
            // have now, and a new column must appear rather than vanish
            // behind a duplicated id.
            int seq[App::kNumCols], n = 0;
            bool seen[App::kNumCols] = {};
            for (int k = 0; k < idx; ++k)
                if (!seen[a.colSeq[k]]) {
                    seen[a.colSeq[k]] = true;
                    seq[n++] = a.colSeq[k];
                }
            for (int id = 0; id < App::kNumCols; ++id)
                if (!seen[id]) seq[n++] = id;
            for (int k = 0; k < App::kNumCols; ++k) a.colSeq[k] = seq[k];
        }
        bool vis = false; // never end up with zero visible columns
        for (bool b : a.colShow) vis |= b;
        if (!vis) a.colShow[0] = true;
    }
    a.idleTitle = wide(getSetting(a.db, "idle_title", "\xE2\x99\xAA  KARAOKE NIGHT"));
    a.scanTags = getSetting(a.db, "scan_tags", "1") == "1";
    a.autoGainOn = getSetting(a.db, "auto_gain", "1") == "1";
    a.lang = getSetting(a.db, "lang", "0") == "1" ? 1 : 0;
    uiSetLanguage(a.lang);
    a.watchOn = getSetting(a.db, "watch_folders", "0") == "1";
    a.ytDir = wide(getSetting(a.db, "yt_dir", "")); // empty = the %APPDATA% cache
    a.webOn = getSetting(a.db, "web_on", "0") == "1"; // strictly opt-in
    a.webPass = wide(getSetting(a.db, "web_pass", ""));
    a.idleSub = wide(getSetting(a.db, "idle_sub", ""));
    a.idleLogoPath = wide(getSetting(a.db, "idle_logo", ""));
    if (!a.idleLogoPath.empty()) {
        a.idleLogoLoaded = loadImageFile(a.idleLogoPath, a.idleLogo);
        if (!a.idleLogoLoaded) a.idleLogoPath.clear(); // file gone or bad
    }
    a.idleEditorOpen = getSetting(a.db, "idle_editor", "0") == "1";
    a.idleBgPath = wide(getSetting(a.db, "idle_bg", ""));
    if (!a.idleBgPath.empty()) {
        a.idleBgLoaded = loadImageFile(a.idleBgPath, a.idleBg, 1920);
        if (!a.idleBgLoaded) a.idleBgPath.clear();
    }
    { // waiting-screen elements: "on:pos:size," x6 (logo title message
      // next-up singers qr); defaults mirror the classic layout
        static const IdleElem defs[6] = {{true, 1, 1}, {true, 4, 2},
                                         {true, 7, 1}, {true, 7, 1},
                                         {false, 3, 1}, {true, 8, 1}};
        for (int k = 0; k < 6; ++k) a.idleElems[k] = defs[k];
        const std::string es = getSetting(a.db, "idle_elems", "");
        int idx = 0;
        size_t pos = 0;
        while (idx < 6 && pos < es.size()) {
            int on = 1, pp = 4, sz = 1;
            if (sscanf_s(es.c_str() + pos, "%d:%d:%d", &on, &pp, &sz) == 3) {
                a.idleElems[idx].on = on != 0;
                a.idleElems[idx].pos = std::clamp(pp, 0, 8);
                a.idleElems[idx].size = std::clamp(sz, 0, 2);
                ++idx;
            }
            const size_t c = es.find(',', pos);
            if (c == std::string::npos) break;
            pos = c + 1;
        }
    }
    a.videoFit = std::clamp(atoi(getSetting(a.db, "video_fit", "0").c_str()), 0, 2);
    a.audioDevice = getSetting(a.db, "audio_device", "");
    a.winMax = getSetting(a.db, "win_max", "1") == "1"; // maximized by default
    a.appFull = getSetting(a.db, "win_full", "0") == "1"; // showtime mode
    winW = UINT((std::max)(900, atoi(getSetting(a.db, "win_w", "0").c_str())));
    winH = UINT((std::max)(600, atoi(getSetting(a.db, "win_h", "0").c_str())));
}

void saveSettings(App& a, UINT winW, UINT winH) {
    // One transaction: this runs every 5 s on the engine tick, and ~30
    // autocommits could each wait out the busy timeout during an import.
    a.db.exec("BEGIN");
    char b[32];
    snprintf(b, 32, "%.1f", a.fadeSec);
    setSetting(a.db, "fade_sec", b);
    setSetting(a.db, "automix_on", a.automixOn ? "1" : "0");
    setSetting(a.db, "repeat_on", a.repeatOn ? "1" : "0");
    setSetting(a.db, "mix_mode", a.mixMode == MixMode::Smart ? "smart" : "fade");
    setSetting(a.db, "video_monitor", std::to_string(a.chosenMonitor));
    setSetting(a.db, "singer_name", utf8(a.singerName));
    setSetting(a.db, "sidebar_open", a.sidebarOpen ? "1" : "0");
    setSetting(a.db, "queue_open", a.queueOpen ? "1" : "0");
    setSetting(a.db, "side_w", std::to_string(int(a.sideW)));
    setSetting(a.db, "queue_w", std::to_string(int(a.queueW)));
    {
        std::string cs;
        char cb[48];
        for (int k = 0; k < App::kNumCols; ++k) {
            const int id = a.colSeq[k];
            snprintf(cb, 48, "%s%d:%d:%.3f", k ? "," : "", id,
                     a.colShow[id] ? 1 : 0, a.colFrac[id]);
            cs += cb;
        }
        setSetting(a.db, "columns", cs);
    }
    {
        char qb[48];
        snprintf(qb, 48, "%.3f,%.3f,%.3f", a.qColFrac[0], a.qColFrac[1],
                 a.qColFrac[2]);
        setSetting(a.db, "queue_cols", qb);
    }
    setSetting(a.db, "idle_title", utf8(a.idleTitle));
    setSetting(a.db, "scan_tags", a.scanTags ? "1" : "0");
    setSetting(a.db, "auto_gain", a.autoGainOn ? "1" : "0");
    setSetting(a.db, "lang", a.lang ? "1" : "0");
    setSetting(a.db, "watch_folders", a.watchOn ? "1" : "0");
    setSetting(a.db, "web_on", a.webOn ? "1" : "0");
    setSetting(a.db, "web_pass", utf8(a.webPass));
    setSetting(a.db, "idle_sub", utf8(a.idleSub));
    setSetting(a.db, "idle_logo", utf8(a.idleLogoPath));
    setSetting(a.db, "idle_bg", utf8(a.idleBgPath));
    setSetting(a.db, "idle_editor", a.idleEditorOpen ? "1" : "0");
    {
        std::string es;
        char eb[32];
        for (int k = 0; k < 6; ++k) {
            snprintf(eb, 32, "%s%d:%d:%d", k ? "," : "",
                     a.idleElems[k].on ? 1 : 0, a.idleElems[k].pos,
                     a.idleElems[k].size);
            es += eb;
        }
        setSetting(a.db, "idle_elems", es);
    }
    setSetting(a.db, "video_fit", std::to_string(a.videoFit));
    setSetting(a.db, "audio_device", a.audioDevice);
    setSetting(a.db, "win_max", a.winMax ? "1" : "0");
    setSetting(a.db, "win_full", a.appFull ? "1" : "0");
    if (winW) { // 0 = mid-session save (crash guard): window size is exit-only
        setSetting(a.db, "win_w", std::to_string(winW));
        setSetting(a.db, "win_h", std::to_string(winH));
    }
    if (!a.db.exec("COMMIT")) a.db.exec("ROLLBACK");
}

// -------------------------------------------------------------- data loading

// Read a Match from columns base.. in the fixed order
// id, artist, title, path, type, duration_ms — the one SELECT shape every
// track query in this file uses.
Match readMatch(Db::Stmt& q, int base) {
    Match m;
    m.id = q.colInt(base);
    m.artist = wide(q.colText(base + 1));
    m.title = wide(q.colText(base + 2));
    m.label = m.artist.empty() ? m.title : m.artist + L" - " + m.title;
    m.path = wide(q.colText(base + 3));
    m.type = q.colText(base + 4);
    m.durMs = q.colInt(base + 5);
    return m;
}

void reloadNav(App& a) {
    a.playlists.clear();
    Db::Stmt p;
    a.db.prepare(p, "SELECT name,id FROM playlist ORDER BY name");
    while (p.step()) a.playlists.push_back({wide(p.colText(0)), p.colInt(1)});

    a.roots.clear();
    Db::Stmt r;
    a.db.prepare(r, "SELECT path FROM scan_root ORDER BY path");
    while (r.step()) a.roots.push_back(wide(r.colText(0)));

    a.allDirs.clear();
    a.flatFolders.clear();
    std::map<std::wstring, int> dirs;
    Db::Stmt q;
    a.db.prepare(q, "SELECT path FROM media_item WHERE type IN "
                    "('audio','mp3g','video','karaoke_zip')");
    while (q.step()) {
        const std::wstring path = wide(q.colText(0));
        const size_t s = path.find_last_of(L"\\/");
        if (s != std::wstring::npos) ++dirs[path.substr(0, s)];
    }
    for (auto& [dir, n] : dirs) {
        a.allDirs.push_back(dir);
        a.flatFolders.push_back({dir, n});
    }
    std::sort(a.flatFolders.begin(), a.flatFolders.end(),
              [](auto& x, auto& y) { return x.second > y.second; });
    if (a.flatFolders.size() > 20) a.flatFolders.resize(20);

    {
        Db::Stmt c;
        a.db.prepare(c, "SELECT COUNT(*) FROM media_item WHERE type IN "
                        "('audio','mp3g','video','karaoke_zip') "
                        "AND IFNULL(hidden,0)=0");
        a.libCount = c.step() ? int(c.colInt(0)) : 0;
        Db::Stmt h;
        a.db.prepare(h, "SELECT COUNT(*) FROM media_item WHERE hidden=1");
        a.hiddenCount = h.step() ? int(h.colInt(0)) : 0;
    }
    a.singers.clear();
    const std::wstring sf = foldW(a.singerFilter);
    Db::Stmt s;
    a.db.prepare(s, "SELECT sq.id, sq.position, sq.singer, sq.status, m.id, m.artist, "
                    "m.title, m.path, m.type, m.duration_ms "
                    "FROM singer_queue_item sq JOIN media_item m ON m.id=sq.media_id "
                    "ORDER BY CASE WHEN sq.status IN ('waiting','singing') "
                    "THEN 0 ELSE 1 END, sq.position");
    while (s.step()) {
        SingerRow row;
        row.itemId = s.colInt(0);
        row.pos = s.colInt(1);
        row.singer = wide(s.colText(2));
        row.status = s.colText(3);
        row.song = readMatch(s, 4);
        row.label = row.song.label;
        if (!sf.empty()) { // filtered: only this singer's UPCOMING entries
            if (row.status != "waiting" && row.status != "singing") continue;
            if (foldW(row.singer).find(sf) == std::wstring::npos) continue;
        }
        a.singers.push_back(std::move(row));
    }
    if (sf.empty()) { // waiting-screen singer list follows the live rotation
        a.idleSingerLines.clear();
        for (const SingerRow& r2 : a.singers) {
            if (a.idleSingerLines.size() >= 5) break; // next 5 only — keeps the screen uncluttered
            if (r2.itemId < 0 ||
                (r2.status != "waiting" && r2.status != "singing"))
                continue;
            const std::wstring song =
                r2.song.title.empty() ? r2.label : r2.song.title;
            a.idleSingerLines.push_back(
                std::to_wstring(a.idleSingerLines.size() + 1) + L".  " +
                r2.singer + L"  —  " + song);
        }
    }
    if (sf.empty()) { // full rotation: divider before the finished section
        for (size_t i = 0; i < a.singers.size(); ++i) {
            const std::string& st2 = a.singers[i].status;
            if (st2 != "waiting" && st2 != "singing") {
                if (i > 0) {
                    SingerRow sep;
                    sep.itemId = -1;
                    a.singers.insert(a.singers.begin() + i, std::move(sep));
                }
                break;
            }
        }
    } else {
        // Filtered: append the singer's play history (last 30 days).
        const size_t upcoming = a.singers.size();
        Db::Stmt hq;
        a.db.prepare(hq, "SELECT h.started_at, h.singer, m.id, m.artist, m.title, "
                         "m.path, m.type, m.duration_ms "
                         "FROM play_history h JOIN media_item m ON m.id=h.media_id "
                         "ORDER BY h.started_at DESC LIMIT 500");
        bool sep = false;
        while (hq.step()) {
            const std::wstring who = wide(hq.colText(1));
            if (foldW(who).find(sf) == std::wstring::npos) continue;
            if (!sep && upcoming > 0) {
                SingerRow d;
                d.itemId = -1;
                a.singers.push_back(std::move(d));
                sep = true;
            }
            SingerRow row;
            row.itemId = -2; // history entry (dbl-click replays)
            row.singer = who;
            row.status = "played";
            const time_t at = time_t(hq.colInt(0));
            const time_t now = time(nullptr);
            tm lt{}, ln{};
            localtime_s(&lt, &at);
            localtime_s(&ln, &now);
            wchar_t b[20];
            wcsftime(b, 20, lt.tm_year == ln.tm_year ? L"%m/%d %H:%M" : L"%m/%d/%y",
                     &lt);
            row.when = b;
            row.song = readMatch(hq, 2);
            row.label = row.song.label;
            a.singers.push_back(std::move(row));
        }
    }
    a.history.clear();
    a.playedTonight.clear();
    Db::Stmt hh;
    a.db.prepare(hh, "SELECT h.started_at, h.singer, m.id, m.artist, m.title, "
                     "m.path, m.type, m.duration_ms "
                     "FROM play_history h JOIN media_item m ON m.id=h.media_id "
                     "WHERE h.started_at > strftime('%s','now') - 43200 "
                     "ORDER BY h.started_at DESC LIMIT 300");
    while (hh.step()) {
        App::HistRow row;
        const time_t at = time_t(hh.colInt(0));
        tm lt{};
        localtime_s(&lt, &at);
        wchar_t b[8];
        wcsftime(b, 8, L"%H:%M", &lt);
        row.when = b;
        row.singer = wide(hh.colText(1));
        row.song = readMatch(hh, 2);
        a.playedTonight.insert(row.song.id);
        a.history.push_back(std::move(row));
    }
    a.navDirty = false;
}

void reloadBrowser(App& a) {
    const int64_t prevSelId =
        a.selLib >= 0 && a.selLib < int(a.results.size())
            ? a.results[a.selLib].id
            : 0;
    if (a.nav == NavMode::Library) {
        a.results = searchMedia(a.db, a.search, 200000, L"", a.sortCol, a.sortAsc,
                                a.showHidden);
    } else if (a.nav == NavMode::Folder) {
        a.results = searchMedia(a.db, a.search, 200000, a.navFolder, a.sortCol,
                                a.sortAsc);
    } else if (a.nav == NavMode::Playlist) {
        a.results.clear();
        a.resultsPlItem.clear();
        Db::Stmt q;
        a.db.prepare(q, "SELECT m.id, m.artist, m.title, m.path, m.type, m.duration_ms, "
                        "m.genre, m.year, m.bpm, IFNULL(m.music_key,0), pi.id "
                        "FROM playlist_item pi JOIN media_item m ON m.id=pi.media_id "
                        "WHERE pi.playlist_id=?1 ORDER BY pi.position");
        q.bind(1, a.navPlaylist);
        const std::wstring termLower = foldW(a.search);
        while (q.step()) {
            Match m = readMatch(q);
            m.genre = wide(q.colText(6));
            m.year = q.colInt(7);
            m.bpm = q.colInt(8);
            m.musicKey = q.colInt(9);
            if (!termLower.empty() &&
                foldW(m.label + L" " + m.path).find(termLower) == std::wstring::npos)
                continue;
            a.resultsPlItem.push_back(q.colInt(10));
            a.results.push_back(std::move(m));
        }
    }
    // Background refreshes must not steal the operator's place: keep the
    // selected track (by id) and the scroll offset, just clamped.
    a.selLib = a.results.empty() ? -1 : 0;
    if (prevSelId) {
        for (size_t i = 0; i < a.results.size(); ++i)
            if (a.results[i].id == prevSelId) {
                a.selLib = int(i);
                break;
            }
    }
    a.selRows.clear();
    if (a.selLib >= 0) a.selRows.insert(a.selLib);
    a.libScroll = std::clamp(a.libScroll, 0.f,
                             (std::max)(0.f, float(a.results.size()) - 1));
    a.searchDirty = false;
}

Match matchFromPath(App& a, const std::wstring& path) {
    Db::Stmt q; // prefer the library entry when the file is known
    a.db.prepare(q, "SELECT id,artist,title,path,type,duration_ms FROM media_item "
                    "WHERE path=?1");
    q.bind(1, utf8(path));
    if (q.step()) return readMatch(q);
    Match m;
    m.path = path;
    const std::wstring leaf = leafName(path);
    const size_t dot = leaf.find_last_of(L'.');
    m.label = dot == std::wstring::npos ? leaf : leaf.substr(0, dot);
    std::wstring audio, cdgPath;
    if (!hasExt(path, L".zip")) // don't extract a zip just to type-tag it
        resolveMedia(path, audio, cdgPath);
    m.type = hasExt(path, L".zip") ? "karaoke_zip"
             : !cdgPath.empty()    ? "mp3g"
             : (hasExt(path, L".mp4") || hasExt(path, L".mkv") || hasExt(path, L".mov") ||
                hasExt(path, L".avi") || hasExt(path, L".wmv"))
                 ? "video"
                 : "audio";
    return m;
}

// ------------------------------------------------------------ session snapshot
// FR-010: the live state that is NOT already in the database — what each deck
// holds and where it is, plus the queue — written to the settings table every
// few seconds when it changes. snap_clean marks a graceful exit; missing on
// startup means the app crashed, and the night restores automatically.
// Format: two deck lines "path \t seconds \t playing|cued|empty", then one
// queue path per line (paths cannot contain tab/newline on Windows).

std::wstring snapshotBlob(App& a) {
    std::wstring s;
    for (int d = 0; d < 2; ++d) {
        const bool has = !a.label[d].empty();
        const double pos =
            has ? double(a.decks[d]->framesPlayed.load()) / kRate : 0.0;
        s += (has ? a.deckMatch[d].path : L"") + L"\t" + std::to_wstring(pos) +
             L"\t";
        s += !has                              ? L"empty"
             : a.mixer.activeDeck.load() != d ? L"cued"
             : a.decks[d]->paused.load()      ? L"paused" // restores as cued:
                                              : L"playing"; // no surprise audio
        s += L"\n";
    }
    for (const auto& m : a.queue) s += m.path + L"\n";
    return s;
}

void saveSnapshot(App& a) {
    const std::wstring blob = snapshotBlob(a);
    if (blob == a.lastSnap) return;
    setSetting(a.db, "snap_blob", utf8(blob));
    a.lastSnap = blob;
}

void restoreSnapshot(App& a) {
    const std::wstring blob = wide(getSetting(a.db, "snap_blob", ""));
    if (blob.empty()) return;
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start < blob.size()) {
        const size_t nl = blob.find(L'\n', start);
        lines.push_back(blob.substr(start, nl - start));
        if (nl == std::wstring::npos) break;
        start = nl + 1;
    }
    int tracks = 0;
    for (int d = 0; d < 2 && d < int(lines.size()); ++d) {
        const size_t t1 = lines[d].find(L'\t');
        const size_t t2 = lines[d].find(L'\t', t1 + 1);
        if (t1 == std::wstring::npos || t2 == std::wstring::npos) continue;
        const std::wstring path = lines[d].substr(0, t1);
        const double pos = _wtof(lines[d].substr(t1 + 1, t2 - t1 - 1).c_str());
        const std::wstring st = lines[d].substr(t2 + 1);
        if (path.empty() || st == L"empty") continue;
        if (!loadTo(a, d, matchFromPath(a, path))) continue;
        if (pos > 1.0) {
            a.decks[d]->seek(pos); // worker-applied; safe while Loading
            if (a.hasVid[d]) a.vdec[d].seek(int64_t(pos * 10000000.0));
        }
        if (st == L"playing") { // resume the music as fast as possible
            a.pendingFade = d;
            a.pendingDur = 0.3;
        }
        ++tracks;
    }
    for (size_t i = 2; i < lines.size(); ++i) {
        if (lines[i].empty()) continue;
        a.queue.push_back(matchFromPath(a, lines[i]));
        ++tracks;
    }
    if (tracks)
        a.status = L"crash recovery: session restored (" +
                   std::to_wstring(tracks) + L" tracks)";
}
