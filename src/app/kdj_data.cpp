#include "app/kdj.h"

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
    { // columns: "seq|show|frac" triplets, e.g. "0:1:0.34,1:1:0.24,..."
        const std::string cs = getSetting(a.db, "columns", "");
        int idx = 0;
        size_t pos = 0;
        while (idx < 6 && pos < cs.size()) {
            int id = 0, show = 1;
            float frac = 0.1f;
            if (sscanf_s(cs.c_str() + pos, "%d:%d:%f", &id, &show, &frac) == 3 &&
                id >= 0 && id < 6) {
                a.colSeq[idx] = id;
                a.colShow[id] = show != 0;
                a.colFrac[id] = std::clamp(frac, 0.04f, 0.9f);
                ++idx;
            }
            const size_t c = cs.find(',', pos);
            if (c == std::string::npos) break;
            pos = c + 1;
        }
        bool vis = false; // never end up with zero visible columns
        for (bool b : a.colShow) vis |= b;
        if (!vis) a.colShow[0] = true;
    }
    a.idleTitle = wide(getSetting(a.db, "idle_title", "\xE2\x99\xAA  KARAOKE NIGHT"));
    a.scanTags = getSetting(a.db, "scan_tags", "1") == "1";
    a.autoGainOn = getSetting(a.db, "auto_gain", "1") == "1";
    a.webOn = getSetting(a.db, "web_on", "0") == "1"; // strictly opt-in
    a.videoFit = std::clamp(atoi(getSetting(a.db, "video_fit", "0").c_str()), 0, 2);
    a.audioDevice = getSetting(a.db, "audio_device", "");
    winW = UINT((std::max)(900, atoi(getSetting(a.db, "win_w", "0").c_str())));
    winH = UINT((std::max)(600, atoi(getSetting(a.db, "win_h", "0").c_str())));
}

void saveSettings(App& a, UINT winW, UINT winH) {
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
        for (int k = 0; k < 6; ++k) {
            const int id = a.colSeq[k];
            snprintf(cb, 48, "%s%d:%d:%.3f", k ? "," : "", id,
                     a.colShow[id] ? 1 : 0, a.colFrac[id]);
            cs += cb;
        }
        setSetting(a.db, "columns", cs);
    }
    setSetting(a.db, "idle_title", utf8(a.idleTitle));
    setSetting(a.db, "scan_tags", a.scanTags ? "1" : "0");
    setSetting(a.db, "auto_gain", a.autoGainOn ? "1" : "0");
    setSetting(a.db, "web_on", a.webOn ? "1" : "0");
    setSetting(a.db, "video_fit", std::to_string(a.videoFit));
    setSetting(a.db, "audio_device", a.audioDevice);
    setSetting(a.db, "win_w", std::to_string(winW));
    setSetting(a.db, "win_h", std::to_string(winH));
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
                         "WHERE h.started_at > strftime('%s','now') - 2592000 "
                         "ORDER BY h.started_at DESC LIMIT 200");
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
            tm lt{};
            localtime_s(&lt, &at);
            wchar_t b[20];
            wcsftime(b, 20, L"%m/%d %H:%M", &lt);
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
    if (a.nav == NavMode::Library) {
        a.results = searchMedia(a.db, a.search, 200, L"", a.sortCol, a.sortAsc);
    } else if (a.nav == NavMode::Folder) {
        a.results = searchMedia(a.db, a.search, 200, a.navFolder, a.sortCol, a.sortAsc);
    } else if (a.nav == NavMode::Playlist) {
        a.results.clear();
        Db::Stmt q;
        a.db.prepare(q, "SELECT m.id, m.artist, m.title, m.path, m.type, m.duration_ms, "
                        "m.genre, m.year, m.bpm "
                        "FROM playlist_item pi JOIN media_item m ON m.id=pi.media_id "
                        "WHERE pi.playlist_id=?1 ORDER BY pi.position");
        q.bind(1, a.navPlaylist);
        const std::wstring termLower = foldW(a.search);
        while (q.step()) {
            Match m = readMatch(q);
            m.genre = wide(q.colText(6));
            m.year = q.colInt(7);
            m.bpm = q.colInt(8);
            if (!termLower.empty() &&
                foldW(m.label + L" " + m.path).find(termLower) == std::wstring::npos)
                continue;
            a.results.push_back(std::move(m));
        }
    }
    a.selLib = a.results.empty() ? -1 : 0;
    a.selRows.clear();
    if (a.selLib >= 0) a.selRows.insert(0);
    a.libScroll = 0;
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

// ------------------------------------------------------------------- import

// Walks every library row with unknown BPM and detects it from the audio.
// Undetectable files are marked -1 (shown blank) so launches don't re-chew
// them; the deck's full-track scan still refines those on first play.
void startBpmAnalysis(App& a) {
    if (a.scanning.load()) return; // the import-finished handler restarts us
    a.bpmStop.store(true);
    if (a.bpmThread.joinable()) a.bpmThread.join();
    a.bpmStop.store(false);
    a.bpmDone.store(0);
    a.bpmTotal.store(0);
    const std::wstring dbPath = a.dbPath;
    a.bpmBusy.store(true);
    a.bpmThread = std::thread([&a, dbPath]() {
        // ponytail: OS background mode throttles CPU/IO scheduling; if decode
        // ever audibly competes with playback on the gig laptop, add an
        // "either deck live -> sleep" gate here.
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            Db db;
            if (db.open(dbPath)) {
            struct Row { int64_t id; std::wstring path; };
            std::vector<Row> rows;
            {
                Db::Stmt q; // zips would need extraction first; the deck
                            // scan covers those on first play
                db.prepare(q, "SELECT id, path FROM media_item WHERE bpm=0 "
                              "AND type IN ('audio','mp3g','video')");
                while (q.step())
                    rows.push_back({q.colInt(0), wide(q.colText(1))});
            }
            a.bpmTotal.store(int(rows.size()));
            for (const Row& r : rows) {
                if (a.bpmStop.load()) break;
                const int bpm = analyzeBpm(r.path);
                Db::Stmt u;
                db.prepare(u,
                           "UPDATE media_item SET bpm=?2 WHERE id=?1 AND bpm=0");
                u.bind(1, r.id).bind(2, int64_t(bpm > 0 ? bpm : -1));
                u.step();
                a.bpmDone.fetch_add(1);
            }
            }
        }
        CoUninitialize();
        a.bpmBusy.store(false);
        if (a.bpmTotal.load() > 0) a.bpmFinished.store(true);
    });
}

void startImport(App& a, const std::wstring& folder) {
    if (a.scanning.load() || folder.empty()) return;
    a.bpmStop.store(true); // the import owns the disk; analysis resumes after
    if (a.scanThread.joinable()) a.scanThread.join();
    a.scanning.store(true);
    a.status = L"importing: " + folder;
    const std::wstring dbPath = a.dbPath;
    const bool tags = a.scanTags;
    a.scanProg.reset();
    a.scanThread = std::thread([&a, folder, dbPath, tags]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            Db sdb;
            if (sdb.open(dbPath)) {
                scanDirectory(sdb, folder, &a.scanProg, tags);
                Db::Stmt q;
                sdb.prepare(q, "INSERT OR IGNORE INTO scan_root(path) VALUES(?1)");
                q.bind(1, utf8(folder));
                q.step();
            }
        }
        CoUninitialize();
        a.scanning.store(false);
        a.scanFinished.store(true);
    });
}

std::wstring pickFolder(HWND owner) {
    IFileDialog* fd = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&fd))))
        return L"";
    DWORD opts = 0;
    fd->GetOptions(&opts);
    fd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    std::wstring out;
    if (SUCCEEDED(fd->Show(owner))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(fd->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = p;
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    fd->Release();
    return out;
}

// ------------------------------------------------------------------ youtube
// Paste a URL into the search box and press Enter: yt-dlp.exe (dropped next to
// the app, or on PATH) downloads it into %APPDATA%\KaraokeDJ\youtube and the
// file joins the queue like any library track.

// Run a console command hidden, capture stdout+stderr (UTF-8 → wide).
std::wstring runCapture(const std::wstring& cmd, DWORD& exitCode) {
    exitCode = DWORD(-1);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return L"";
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cl = cmd; // CreateProcess may scribble on the buffer
    std::string out;
    if (CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(wr);
        wr = nullptr;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (wr) CloseHandle(wr);
    CloseHandle(rd);
    return wide(out);
}

std::wstring youtubeCacheDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = n ? std::wstring(buf) + L"\\KaraokeDJ" : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\youtube";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void startYoutube(App& a, std::wstring url) {
    if (a.ytBusy.load()) {
        a.status = L"a YouTube download is already running";
        return;
    }
    if (a.ytThread.joinable()) a.ytThread.join();
    url.erase(std::remove_if(url.begin(), url.end(),
                             [](wchar_t c) { return c == L'"' || c < 32; }),
              url.end());
    a.ytBusy.store(true);
    a.status = L"YouTube: downloading…  " + url;
    a.ytThread = std::thread([&a, url]() {
        const std::wstring dir = youtubeCacheDir();
        // MP4 (H.264+AAC) so Media Foundation plays it like any library video.
        const std::wstring cmd =
            L"yt-dlp.exe --no-playlist --encoding utf-8 "
            L"-f \"b[ext=mp4]/bv*[ext=mp4]+ba[ext=m4a]/b\" --remux-video mp4 "
            L"--no-simulate --print after_move:filepath "
            L"-o \"" + dir + L"\\%(title)s [%(id)s].%(ext)s\" \"" + url + L"\"";
        DWORD code = DWORD(-1);
        std::wstring out = runCapture(cmd, code);
        // Last non-empty line of stdout is the final file path.
        std::wstring path;
        const size_t end = out.find_last_not_of(L"\r\n");
        if (end != std::wstring::npos) {
            out.resize(end + 1);
            const size_t nl = out.find_last_of(L'\n');
            path = nl == std::wstring::npos ? out : out.substr(nl + 1);
            while (!path.empty() && path.back() == L'\r') path.pop_back();
        }
        if (code == 0 && !path.empty() &&
            GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            a.ytPath = path;
            a.ytError.clear();
        } else {
            a.ytPath.clear();
            a.ytError = out.empty()
                            ? L"yt-dlp.exe not found — put it next to the app"
                            : L"download failed (" + // yt-dlp errors are at the tail
                                  out.substr(out.size() > 160 ? out.size() - 160 : 0) +
                                  L")";
        }
        a.ytBusy.store(false);
        a.ytDone.store(true);
    });
}
