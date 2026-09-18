#include "app/kdj.h"

#include <winhttp.h>

// Background jobs and blocking OS dialogs: update check, library import,
// the folder watcher, BPM + musical key analysis, the STA file/folder
// pickers, profile transfer and YouTube download.
// Split out of kdj_data.cpp.

// ------------------------------------------------------------- update check

static std::string httpsGet(const wchar_t* host, const wchar_t* path) {
    std::string out;
    HINTERNET ses = WinHttpOpen(L"KaraokeDJ", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return out;
    if (HINTERNET con = WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0)) {
        if (HINTERNET req = WinHttpOpenRequest(con, L"GET", path, nullptr,
                                               WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE)) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD n = 0;
                do {
                    char buf[4096];
                    n = 0;
                    if (!WinHttpReadData(req, buf, sizeof(buf), &n)) break;
                    out.append(buf, n);
                } while (n > 0 && out.size() < 1 << 20);
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
    return out;
}

// "1.2.10" -> {1,2,10}; missing parts are 0.
static void parseVer(const char* s, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    sscanf_s(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
}

void startUpdateCheck(App& a, bool manual) {
    if (a.updBusy.load()) return;
    if (a.updThread.joinable()) a.updThread.join();
    a.updBusy.store(true);
    a.updManual = manual;
    a.updThread = std::thread([&a]() {
        const std::string body = httpsGet(
            L"api.github.com", L"/repos/MrBeanTheOne/KaraokeDJ/releases/latest");
        std::wstring latest;
        std::wstring err = L"couldn't reach GitHub";
        const size_t k = body.find("\"tag_name\":\"");
        if (k != std::string::npos) {
            size_t b = k + 12;
            if (b < body.size() && body[b] == 'v') ++b;
            const size_t e = body.find('\"', b);
            if (e != std::string::npos && e - b < 24) {
                const std::string tag = body.substr(b, e - b);
                int cur[3], rel[3];
                parseVer(KDJ_VERSION, cur);
                parseVer(tag.c_str(), rel);
                const bool newer = rel[0] != cur[0]   ? rel[0] > cur[0]
                                   : rel[1] != cur[1] ? rel[1] > cur[1]
                                                      : rel[2] > cur[2];
                if (newer) latest = wide(tag);
                err.clear();
            }
        }
        a.updLatest = std::move(latest); // written before updDone is set
        a.updError = std::move(err);
        a.updBusy.store(false);
        a.updDone.store(true);
    });
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
    a.bpmWaiting.store(false);
    const std::wstring dbPath = a.dbPath;
    a.bpmBusy.store(true);
    a.bpmThread = std::thread([&a, dbPath]() {
        // Coordinator: builds the worklist, then hands it to a few workers.
        // The cost of a library pass is almost entirely DECODE, which is
        // per-file independent, so this parallelises nearly linearly — a
        // 100k-track library is the difference between half a day and a
        // couple of hours.
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        struct Row { int64_t id; std::wstring path; };
        std::vector<Row> rows;
        {
            Db db;
            if (db.open(dbPath)) {
                Db::Stmt q; // zips would need extraction first; the deck
                            // scan covers those on first play
                db.prepare(q, "SELECT id, path FROM media_item "
                              "WHERE (bpm=0 OR IFNULL(music_key,0)=0) "
                              "AND type IN ('audio','mp3g','video')");
                while (q.step())
                    rows.push_back({q.colInt(0), wide(q.colText(1))});
            }
        }
        a.bpmTotal.store(int(rows.size()));

        // A deck at its end-of-stream is silent and stays "playing" until
        // something replaces it, so eos must not hold the gate shut —
        // otherwise the last song of the night stops analysis for good.
        // Paused is silent too, and its ring is already full.
        auto audible = [&a]() {
            for (int d = 0; d < 2; ++d)
                if (a.decks[d]->playing.load() && !a.decks[d]->paused.load() &&
                    !a.decks[d]->eos.load())
                    return true;
            return false;
        };

        std::atomic<size_t> next{0};
        // Half the cores, 2..4. Decode is CPU+IO and this must never be the
        // reason a gig stutters; the gate already stops it during playback,
        // so there is nothing to gain from being greedier.
        const unsigned hw = std::thread::hardware_concurrency();
        const unsigned nWorkers = std::clamp(hw ? hw / 2 : 2u, 2u, 4u);
        std::vector<std::thread> workers;
        for (unsigned w = 0; w < nWorkers; ++w) {
            workers.emplace_back([&]() {
                // Each worker owns its COM apartment, its decoder and its own
                // SQLite connection. WAL takes one writer at a time and the
                // connection already carries a 3 s busy timeout; the writes
                // are one tiny UPDATE per track, so they never queue up.
                SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                {
                    Db db;
                    if (db.open(dbPath)) {
                        for (;;) {
                            if (a.bpmStop.load()) break;
                            // Stand down while a deck is live. Checked BEFORE
                            // claiming an index, so a held worker never sits
                            // on a row nobody else can take.
                            if (audible()) {
                                a.bpmWaiting.store(true);
                                std::this_thread::sleep_for(250ms);
                                continue;
                            }
                            a.bpmWaiting.store(false);
                            const size_t i =
                                next.fetch_add(1, std::memory_order_relaxed);
                            if (i >= rows.size()) break;
                            const Row& r = rows[i];
                            int bpm = 0, key = -1;
                            // One decode, both answers. A file we couldn't
                            // open at all (unplugged drive) is left untouched
                            // so it gets another go next launch — recording
                            // "nothing found" would retire it for good.
                            if (!analyzeTrack(r.path, bpm, key, &a.bpmStop)) {
                                a.bpmDone.fetch_add(1);
                                continue;
                            }
                            Db::Stmt u; // each column fills only its own slot
                            db.prepare(u,
                                       "UPDATE media_item SET "
                                       "bpm = CASE WHEN bpm=0 THEN ?2 ELSE bpm END, "
                                       "music_key = CASE WHEN IFNULL(music_key,0)=0 "
                                       "THEN ?3 ELSE music_key END WHERE id=?1");
                            u.bind(1, r.id)
                                .bind(2, int64_t(bpm > 0 ? bpm : -1))
                                .bind(3, int64_t(keyToDb(key)));
                            u.step();
                            a.bpmDone.fetch_add(1);
                        }
                    }
                }
                CoUninitialize();
            });
        }
        for (auto& t : workers) t.join(); // rows/next outlive every worker
        CoUninitialize();
        a.bpmWaiting.store(false);
        a.bpmBusy.store(false);
        if (a.bpmTotal.load() > 0) a.bpmFinished.store(true);
    });
}

// Import now, or line up behind the one that's running (the finished-import
// handler in the main loop starts the next one).
void queueRescan(App& a, const std::wstring& folder) {
    if (folder.empty()) return;
    if (!a.scanning.load()) {
        startImport(a, folder);
        return;
    }
    for (const std::wstring& q : a.rescanQueue)
        if (q == folder) return; // already queued
    a.rescanQueue.push_back(folder);
}

void rescanAll(App& a) {
    Db::Stmt q;
    a.db.prepare(q, "SELECT path FROM scan_root ORDER BY path");
    int n = 0;
    while (q.step()) {
        queueRescan(a, wide(q.colText(0)));
        ++n;
    }
    a.status = n ? L"updating " + std::to_wstring(n) + L" library folder(s)"
                 : L"no imported folders yet";
}

void stopWatcher(App& a) {
    if (a.watchStop) SetEvent(a.watchStop);
    if (a.watchThread.joinable()) a.watchThread.join();
    if (a.watchStop) {
        CloseHandle(a.watchStop);
        a.watchStop = nullptr;
    }
}

void startWatcher(App& a) {
    stopWatcher(a);
    if (!a.watchOn) return;
    std::vector<std::wstring> roots;
    {
        Db::Stmt q;
        a.db.prepare(q, "SELECT path FROM scan_root ORDER BY path");
        while (q.step()) roots.push_back(wide(q.colText(0)));
    }
    if (roots.empty()) return;
    a.watchStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    a.watchThread = std::thread([&a, roots]() {
        // One change handle per reachable root (an unplugged drive just isn't
        // watched until the watcher restarts). Any event marks the root
        // dirty; the main loop rescans after things go quiet.
        std::vector<HANDLE> handles{a.watchStop};
        std::vector<std::wstring> owner{L""};
        for (const std::wstring& r : roots) {
            HANDLE h = FindFirstChangeNotificationW(
                r.c_str(), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
            if (h != INVALID_HANDLE_VALUE) {
                handles.push_back(h);
                owner.push_back(r);
            }
        }
        for (;;) {
            const DWORD w = WaitForMultipleObjects(DWORD(handles.size()),
                                                   handles.data(), FALSE,
                                                   INFINITE);
            if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break; // stop event
            const size_t i = w - WAIT_OBJECT_0;
            if (i >= handles.size()) break;
            {
                std::lock_guard<std::mutex> lk(a.watchMx);
                a.watchDirtyRoots.insert(owner[i]);
            }
            a.watchLastEvent.store(GetTickCount64());
            if (!FindNextChangeNotification(handles[i])) break;
        }
        for (size_t i = 1; i < handles.size(); ++i)
            FindCloseChangeNotification(handles[i]);
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

std::wstring pickFile(HWND owner) {
    IFileDialog* fd = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&fd))))
        return L"";
    static const COMDLG_FILTERSPEC kImg[] = {
        {L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp"},
        {L"All files", L"*.*"}};
    fd->SetFileTypes(2, kImg);
    DWORD opts = 0;
    fd->GetOptions(&opts);
    fd->SetOptions(opts | FOS_FORCEFILESYSTEM);
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

// Everything worth moving between machines: settings (minus this machine's
// window/audio/monitor), per-track cues/bpm/tags/hidden keyed by PATH (the
// external drive keeps its paths), playlists by name. One .kdjprofile file =
// a SQLite database, so no hand-rolled serialization.
bool exportProfile(App& a, const std::wstring& file) {
    DeleteFileW(file.c_str()); // a save dialog already confirmed overwrite
    Db::Stmt at;
    a.db.prepare(at, "ATTACH ?1 AS exp");
    at.bind(1, utf8(file));
    at.step(); // DDL: no rows; failure surfaces in the execs below
    const bool ok =
        a.db.exec("CREATE TABLE exp.settings AS SELECT key, value FROM settings "
                  "WHERE key NOT IN ('win_w','win_h','audio_device',"
                  "'video_monitor','snap_blob','snap_clean');") &&
        a.db.exec("CREATE TABLE exp.media AS SELECT path, artist, title, genre, "
                  "year, bpm, IFNULL(music_key,0) music_key, cue_in_ms, "
                  "cue_out_ms, IFNULL(hidden,0) hidden FROM media_item;") &&
        a.db.exec("CREATE TABLE exp.pl AS SELECT id, name FROM playlist;") &&
        a.db.exec("CREATE TABLE exp.pli AS SELECT pi.playlist_id, m.path, "
                  "pi.position FROM playlist_item pi "
                  "JOIN media_item m ON m.id = pi.media_id;");
    a.db.exec("DETACH exp;");
    return ok;
}

bool importProfile(App& a, const std::wstring& file) {
    Db::Stmt at;
    a.db.prepare(at, "ATTACH ?1 AS imp");
    at.bind(1, utf8(file));
    at.step();
    // A profile is a sqlite db with these tables; anything else fails here.
    Db::Stmt chk;
    if (!a.db.prepare(chk, "SELECT COUNT(*) FROM imp.media") || !chk.step()) {
        a.db.exec("DETACH imp;");
        return false;
    }
    // Profiles written before key detection have no music_key column. Probe
    // for it rather than rejecting those files as "not a profile".
    Db::Stmt probe;
    const bool hasKey =
        a.db.prepare(probe, "SELECT music_key FROM imp.media LIMIT 1");
    const bool ok =
        a.db.exec("INSERT OR REPLACE INTO settings SELECT key, value "
                  "FROM imp.settings;") &&
        a.db.exec(hasKey
                  ? "UPDATE media_item SET (artist,title,genre,year,bpm,"
                    "music_key,cue_in_ms,cue_out_ms,hidden) = "
                    "(SELECT im.artist, im.title, im.genre, im.year, im.bpm, "
                    "im.music_key, im.cue_in_ms, im.cue_out_ms, im.hidden "
                    "FROM imp.media im WHERE im.path = media_item.path), "
                    "search_f = NULL "
                    "WHERE path IN (SELECT path FROM imp.media);"
                  : "UPDATE media_item SET "
                    "(artist,title,genre,year,bpm,cue_in_ms,cue_out_ms,hidden) = "
                    "(SELECT im.artist, im.title, im.genre, im.year, im.bpm, "
                    "im.cue_in_ms, im.cue_out_ms, im.hidden FROM imp.media im "
                    "WHERE im.path = media_item.path), "
                    "search_f = NULL "
                    "WHERE path IN (SELECT path FROM imp.media);") &&
        // refold whatever the import touched (same rule as the migration)
        a.db.exec("UPDATE media_item SET search_f = fold(COALESCE(title,'')) || "
                  "char(10) || fold(COALESCE(artist,'')) || char(10) || "
                  "fold(path) WHERE search_f IS NULL;") &&
        a.db.exec("INSERT INTO playlist(name) SELECT name FROM imp.pl "
                  "WHERE name NOT IN (SELECT name FROM playlist);") &&
        a.db.exec("DELETE FROM playlist_item WHERE playlist_id IN "
                  "(SELECT p.id FROM playlist p JOIN imp.pl ip "
                  "ON ip.name = p.name);") &&
        a.db.exec("INSERT INTO playlist_item(playlist_id, media_id, position) "
                  "SELECT p.id, m.id, ipi.position FROM imp.pli ipi "
                  "JOIN imp.pl ip ON ip.id = ipi.playlist_id "
                  "JOIN playlist p ON p.name = ip.name "
                  "JOIN media_item m ON m.path = ipi.path;");
    a.db.exec("DETACH imp;");
    if (ok) { // re-apply the imported settings to the live app
        UINT dw = 0, dh = 0;
        loadSettings(a, dw, dh);
        a.web.setPassword(a.webPass);
        a.web.setLanguage(a.lang);
        startWatcher(a);
        a.navDirty = a.searchDirty = true;
    }
    return ok;
}

std::wstring pickProfile(HWND owner, bool save) {
    IFileDialog* fd = nullptr;
    const HRESULT hr =
        save ? CoCreateInstance(__uuidof(FileSaveDialog), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&fd))
             : CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&fd));
    if (FAILED(hr)) return L"";
    static const COMDLG_FILTERSPEC kProf[] = {
        {L"Karaoke DJ profile", L"*.kdjprofile"}};
    fd->SetFileTypes(1, kProf);
    fd->SetDefaultExtension(L"kdjprofile");
    if (save) fd->SetFileName(L"karaoke-dj");
    DWORD opts = 0;
    fd->GetOptions(&opts);
    fd->SetOptions(opts | FOS_FORCEFILESYSTEM);
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

std::wstring youtubeCacheDir(const std::wstring& custom) {
    // Operator-chosen folder (settings): point it at a library folder and the
    // download is just another track the watcher/import picks up.
    if (!custom.empty()) {
        CreateDirectoryW(custom.c_str(), nullptr); // no-op when it exists
        const DWORD at = GetFileAttributesW(custom.c_str());
        if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY))
            return custom;
        // Unplugged drive, or a path that came over in someone else's profile:
        // fall through rather than hand yt-dlp a folder that isn't there.
    }
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
    const std::wstring dir = youtubeCacheDir(a.ytDir); // read on the UI thread
    a.ytThread = std::thread([&a, url, dir]() {
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
