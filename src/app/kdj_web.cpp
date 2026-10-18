#include "app/kdj.h"

// WEB view backend: YouTube search (yt-dlp flat-playlist JSON), the download
// worker with live progress, and the thumbnail fetcher. Split out of
// kdj_jobs.cpp — this is a self-contained subsystem around three workers.
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

// ------------------------------------------------------------------ WEB view
// Search YouTube from inside the app (Enter in the WEB view), pick a quality,
// download with live progress, and the file lands in the yt folder and gets
// imported. Replaces the old paste-a-URL one-shot that gave no feedback and
// looked like a frozen search box.

const wchar_t* kWebFmtNames[4] = {L"MP4 720p", L"MP4 BEST", L"MP3", L"WAV"};
static const wchar_t* kWebFmtArgs[4] = {
    L"-f \"bv*[height<=720][ext=mp4]+ba[ext=m4a]/b[ext=mp4]/b\" --remux-video mp4",
    L"-f \"bv*[ext=mp4]+ba[ext=m4a]/b[ext=mp4]/b\" --remux-video mp4",
    L"-f \"ba/b\" -x --audio-format mp3 --audio-quality 0",
    L"-f \"ba/b\" -x --audio-format wav",
};

// Pull "key":"string value" out of one dump-json line, with the escapes a
// title actually contains (\" \\ \uXXXX). Not a JSON parser; doesn't need
// to be — yt-dlp emits one flat object per line.
static std::wstring jsonStr(const std::string& line, const char* key) {
    const std::string pat = std::string("\"") + key + "\":";
    size_t b = line.find(pat);
    if (b == std::string::npos) return L"";
    b += pat.size();
    // yt-dlp emits json.dumps defaults: a space after the colon
    while (b < line.size() && line[b] == ' ') ++b;
    if (b >= line.size() || line[b] != '"') return L"";
    ++b;
    std::string raw;
    for (size_t i = b; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') break;
        if (c != '\\') { raw += c; continue; }
        if (++i >= line.size()) break;
        switch (line[i]) {
        case 'n': case 't': raw += ' '; break;
        case 'u':
            if (i + 4 < line.size()) {
                const unsigned cp = strtoul(line.substr(i + 1, 4).c_str(), nullptr, 16);
                // encode as UTF-8 so wide() below rebuilds the real char
                if (cp < 0x80) raw += char(cp);
                else if (cp < 0x800) {
                    raw += char(0xC0 | (cp >> 6));
                    raw += char(0x80 | (cp & 0x3F));
                } else {
                    raw += char(0xE0 | (cp >> 12));
                    raw += char(0x80 | ((cp >> 6) & 0x3F));
                    raw += char(0x80 | (cp & 0x3F));
                }
                i += 4;
            }
            break;
        default: raw += line[i];
        }
    }
    return wide(raw);
}

static int jsonNum(const std::string& line, const char* key) {
    const std::string pat = std::string("\"") + key + "\":";
    const size_t b = line.find(pat);
    if (b == std::string::npos) return 0;
    return atoi(line.c_str() + b + pat.size());
}

void webSearch(App& a, const std::wstring& term) {
    if (a.webSearchBusy.load()) return; // one search at a time
    if (a.webSearchThread.joinable()) a.webSearchThread.join();
    std::wstring t = term;
    t.erase(std::remove_if(t.begin(), t.end(),
                           [](wchar_t c) { return c == L'"' || c < 32; }),
            t.end());
    if (t.empty()) return;
    a.webSearchBusy.store(true);
    a.status = L"searching YouTube…  " + t;
    a.webSearchThread = std::thread([&a, t]() {
        DWORD code = DWORD(-1);
        const std::wstring out = runCapture(
            L"yt-dlp.exe \"ytsearch25:" + t +
                L"\" --flat-playlist --dump-json --no-warnings --encoding utf-8",
            code);
        std::vector<WebHit> hits;
        const std::string u = utf8(out);
        size_t at = 0;
        while (at < u.size()) {
            size_t e = u.find('\n', at);
            if (e == std::string::npos) e = u.size();
            const std::string line = u.substr(at, e - at);
            at = e + 1;
            if (line.find("\"id\":") == std::string::npos) continue;
            WebHit h;
            h.id = jsonStr(line, "id");
            h.title = jsonStr(line, "title");
            h.channel = jsonStr(line, "channel");
            if (h.channel.empty()) h.channel = jsonStr(line, "uploader");
            h.durSec = jsonNum(line, "duration");
            if (!h.id.empty() && !h.title.empty()) hits.push_back(std::move(h));
        }
        // Already in the library? Downloads are named "title [id].ext", so
        // the id between brackets is the stable key. '_' and '-' appear in
        // ids and '_' is a LIKE wildcard — escape it.
        {
            Db db;
            if (!hits.empty() && db.open(a.dbPath)) {
                for (WebHit& h : hits) {
                    std::string pat = "%[";
                    for (const char c : utf8(h.id)) {
                        if (c == '%' || c == '_' || c == '!') pat += '!';
                        pat += c;
                    }
                    pat += "]%";
                    Db::Stmt q;
                    db.prepare(q, "SELECT 1 FROM media_item WHERE path LIKE ?1 "
                                  "ESCAPE '!' LIMIT 1");
                    q.bind(1, pat);
                    h.inLib = q.step();
                }
            }
        }
        std::lock_guard<std::mutex> lk(a.webMx);
        a.webFound = std::move(hits);
        a.webErr = a.webFound.empty()
                       ? (out.empty()
                              ? L"yt-dlp.exe not found — put it next to the app"
                              : L"no results (yt-dlp may need an update — "
                                L"Settings)")
                       : L"";
        a.webSearchBusy.store(false);
        a.webSearchDone.store(true);
    });
}

// Thumbnail fetcher: one worker pulls ids, grabs the mqdefault.jpg from
// i.ytimg.com over the existing WinHTTP helper, decodes via WIC into a BGRA
// frame the UI uploads into its image cache. Misses just leave the grey box.
static void webThumbWorker(App& a) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // WIC
    wchar_t tmpDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmpDir);
    for (;;) {
        std::wstring id;
        {
            std::unique_lock<std::mutex> lk(a.webMx);
            a.webThumbCv.wait(lk, [&] {
                return !a.webThumbReq.empty() || a.webQuit.load();
            });
            if (a.webQuit.load()) break;
            id = a.webThumbReq.front();
            a.webThumbReq.pop_front();
            if (a.webThumbs.count(id)) continue;
        }
        const std::string jpg =
            httpsGet(L"i.ytimg.com", (L"/vi/" + id + L"/mqdefault.jpg").c_str());
        if (jpg.size() < 512) continue; // error page / nothing
        const std::wstring tmp = std::wstring(tmpDir) + L"kdj_thumb.jpg";
        const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                     nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD n = 0;
        WriteFile(h, jpg.data(), DWORD(jpg.size()), &n, nullptr);
        CloseHandle(h);
        VideoFrame f;
        if (loadImageFile(tmp, f, 320)) {
            std::lock_guard<std::mutex> lk(a.webMx);
            a.webThumbs[id] = std::move(f);
        }
        DeleteFileW(tmp.c_str());
    }
    CoUninitialize();
}

// The download worker: one at a time (venue bandwidth), progress parsed from
// yt-dlp --newline lines, final path from --print after_move:filepath.
static void webDlWorker(App& a) {
    // rows move while we work (CLEAR DONE erases), so address by stable key
    const auto byKey = [&a](int key) -> WebDl* { // caller holds webMx
        for (WebDl& d : a.webDls)
            if (d.key == key) return &d;
        return nullptr;
    };
    for (;;) {
        int job = 0;
        std::wstring target;
        int fmt = 0;
        {
            std::unique_lock<std::mutex> lk(a.webMx);
            a.webDlCv.wait(lk, [&] {
                if (a.webQuit.load()) return true;
                for (const WebDl& d : a.webDls)
                    if (d.state == 0) return true;
                return false;
            });
            if (a.webQuit.load()) return;
            for (WebDl& d : a.webDls)
                if (d.state == 0) {
                    job = d.key;
                    d.state = 1;
                    target = d.target;
                    fmt = d.fmt;
                    break;
                }
        }
        if (!job) continue;
        const std::wstring dir = youtubeCacheDir(a.ytDir);
        const std::wstring url =
            target.rfind(L"http", 0) == 0
                ? target
                : L"https://www.youtube.com/watch?v=" + target;
        const std::wstring cmd =
            L"yt-dlp.exe --no-playlist --encoding utf-8 --newline " +
            std::wstring(kWebFmtArgs[std::clamp(fmt, 0, 3)]) +
            L" --no-simulate --print after_move:filepath "
            L"-o \"" + dir + L"\\%(title)s [%(id)s].%(ext)s\" \"" + url + L"\"";

        // Stream the pipe line by line so progress is live.
        DWORD code = DWORD(-1);
        std::wstring lastLine;
        {
            SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
            HANDLE rd = nullptr, wr = nullptr;
            if (!CreatePipe(&rd, &wr, &sa, 0)) continue;
            SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = wr;
            si.hStdError = wr;
            PROCESS_INFORMATION pi{};
            std::wstring cl = cmd;
            if (CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(wr);
                wr = nullptr;
                std::string acc;
                char buf[2048];
                DWORD n = 0;
                while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) {
                    acc.append(buf, n);
                    size_t nl;
                    while ((nl = acc.find('\n')) != std::string::npos) {
                        std::string ln = acc.substr(0, nl);
                        acc.erase(0, nl + 1);
                        while (!ln.empty() && ln.back() == '\r') ln.pop_back();
                        if (ln.empty()) continue;
                        lastLine = wide(ln);
                        // "[download]  42.3% of ..." → live percentage
                        const size_t pc = ln.find('%');
                        if (ln.rfind("[download]", 0) == 0 &&
                            pc != std::string::npos && pc >= 4) {
                            const int pct = atoi(ln.c_str() + 10);
                            std::lock_guard<std::mutex> lk(a.webMx);
                            if (WebDl* d = byKey(job))
                                d->pct = std::clamp(pct, 0, 100);
                        }
                    }
                }
                WaitForSingleObject(pi.hProcess, INFINITE);
                GetExitCodeProcess(pi.hProcess, &code);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            } else {
                lastLine = L"yt-dlp.exe not found — put it next to the app";
            }
            if (wr) CloseHandle(wr);
            CloseHandle(rd);
        }
        const bool ok = code == 0 && !lastLine.empty() &&
                        GetFileAttributesW(lastLine.c_str()) !=
                            INVALID_FILE_ATTRIBUTES;
        {
            std::lock_guard<std::mutex> lk(a.webMx);
            if (WebDl* d = byKey(job)) {
                d->state = ok ? 2 : 3;
                if (ok) {
                    d->pct = 100;
                    d->path = lastLine;
                } else {
                    d->error = lastLine.substr(0, 160);
                }
            }
        }
        if (ok) a.webRescan.store(true); // main loop imports the yt folder
    }
}

void webEnqueue(App& a, const std::wstring& target, const std::wstring& title,
                int fmt) {
    if (!a.webDlThread.joinable())
        a.webDlThread = std::thread(webDlWorker, std::ref(a));
    {
        std::lock_guard<std::mutex> lk(a.webMx);
        WebDl d;
        d.key = ++a.webDlKey;
        d.target = target;
        d.title = title.empty() ? target : title;
        d.fmt = fmt >= 0 ? fmt : a.webFmt;
        a.webDls.push_back(std::move(d));
    }
    a.webDlCv.notify_one();
    a.status = L"download queued: " + (title.empty() ? target : title);
}

void stopWebWorkers(App& a) {
    a.webQuit.store(true);
    a.webDlCv.notify_all();
    a.webThumbCv.notify_all();
    if (a.webSearchThread.joinable()) a.webSearchThread.join();
    if (a.webDlThread.joinable()) a.webDlThread.join();
    if (a.webThumbThread.joinable()) a.webThumbThread.join();
}

void webPoll(App& a) {
    if (a.webSearchDone.exchange(false)) {
        std::lock_guard<std::mutex> lk(a.webMx);
        a.webHits = a.webFound;
        a.status = a.webErr.empty()
                       ? std::to_wstring(a.webHits.size()) + L" YouTube results"
                       : a.webErr;
        // Fetch the thumbnails for this result set (newest first beats a
        // stale backlog), invalidate the UI's uploaded image slots, and
        // evict cached images the new set doesn't use — each decoded thumb
        // is ~230 KB, so an unbounded cache is a slow leak over a night.
        a.webThumbReq.clear();
        std::set<std::wstring> keep;
        for (const WebHit& h : a.webHits) {
            keep.insert(h.id);
            if (!a.webThumbs.count(h.id)) a.webThumbReq.push_back(h.id);
        }
        for (auto it = a.webThumbs.begin(); it != a.webThumbs.end();)
            it = keep.count(it->first) ? std::next(it) : a.webThumbs.erase(it);
        a.webThumbSlotDone.clear();
        if (!a.webThumbThread.joinable())
            a.webThumbThread = std::thread(webThumbWorker, std::ref(a));
        a.webThumbCv.notify_one();
    }
    if (a.webRescan.exchange(false)) {
        // Fast for the unchanged bulk; registers the folder as a root the
        // first time, so downloads are ordinary library tracks afterwards.
        if (!a.scanning.load()) startImport(a, youtubeCacheDir(a.ytDir));
        else a.webRescan.store(true); // import running: try next tick
    }
}

