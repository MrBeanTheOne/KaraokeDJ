#include "library/scanner.h"

#include <windows.h>
#include <shobjidl.h>
#include <propsys.h>
#include <initguid.h>
#include <propkey.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <set>
#include <thread>
#include <vector>

#include "library/db.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

static const std::set<std::wstring> kAudioExt = {L".mp3", L".wav", L".flac", L".m4a",
                                                 L".aac", L".ogg", L".wma"};
static const std::set<std::wstring> kVideoExt = {L".mp4", L".mkv", L".mov", L".mpg",
                                                 L".mpeg", L".avi", L".wmv", L".m4v"};

static std::wstring lowerCopy(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

static std::wstring swapExt(const std::wstring& p, const std::wstring& ext) {
    const size_t dot = p.find_last_of(L'.');
    return (dot == std::wstring::npos ? p : p.substr(0, dot)) + ext;
}

// "01 - Artist - Title" / "Artist - Title" / "Title"
static void filenameGuess(const fs::path& p, std::wstring& artist, std::wstring& title) {
    std::wstring stem = p.stem().wstring();
    size_t i = 0; // strip a leading track number / junk prefix
    while (i < stem.size() && (iswdigit(stem[i]) || stem[i] == L'+')) ++i;
    if (i > 0 && i + 3 < stem.size() && stem.compare(i, 3, L" - ") == 0) stem = stem.substr(i + 3);
    const size_t sep = stem.find(L" - ");
    if (sep != std::wstring::npos) {
        artist = stem.substr(0, sep);
        title = stem.substr(sep + 3);
    } else {
        title = stem;
    }
    while (!title.empty() && title.front() == L' ') title.erase(title.begin());
    while (!artist.empty() && artist.front() == L' ') artist.erase(artist.begin());
}

static void readTags(const std::wstring& path, std::wstring& artist, std::wstring& title,
                     std::wstring& genre, int64_t& year, int64_t& durMs,
                     int64_t& bpm) {
    IPropertyStore* ps = nullptr;
    if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT,
                                                 IID_PPV_ARGS(&ps))))
        return;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(PKEY_Title, &v)) && v.vt == VT_LPWSTR && v.pwszVal[0])
        title = v.pwszVal;
    PropVariantClear(&v);
    if (SUCCEEDED(ps->GetValue(PKEY_Music_Artist, &v))) {
        if (v.vt == (VT_VECTOR | VT_LPWSTR) && v.calpwstr.cElems)
            artist = v.calpwstr.pElems[0];
        else if (v.vt == VT_LPWSTR && v.pwszVal[0])
            artist = v.pwszVal;
    }
    PropVariantClear(&v);
    if (SUCCEEDED(ps->GetValue(PKEY_Music_Genre, &v))) {
        if (v.vt == (VT_VECTOR | VT_LPWSTR) && v.calpwstr.cElems)
            genre = v.calpwstr.pElems[0];
        else if (v.vt == VT_LPWSTR && v.pwszVal[0])
            genre = v.pwszVal;
    }
    PropVariantClear(&v);
    if (SUCCEEDED(ps->GetValue(PKEY_Media_Year, &v)) && v.vt == VT_UI4)
        year = v.ulVal;
    PropVariantClear(&v);
    if (SUCCEEDED(ps->GetValue(PKEY_Media_Duration, &v)) && v.vt == VT_UI8)
        durMs = int64_t(v.uhVal.QuadPart / 10000);
    PropVariantClear(&v);
    if (SUCCEEDED(ps->GetValue(PKEY_Music_BeatsPerMinute, &v)) && v.vt == VT_LPWSTR) {
        const int b = _wtoi(v.pwszVal);
        if (b >= 40 && b <= 250) bpm = b; // sane range only
    }
    PropVariantClear(&v);
    ps->Release();
}

namespace {
struct Item {
    std::wstring path;
    std::string type;
    int64_t size = 0, mtime = 0;
    bool existed = false;
};
struct Meta {
    std::wstring artist, title, genre;
    int64_t year = 0, durMs = 0, bpm = 0;
    std::atomic<bool> ready{false};
};
} // namespace

// Flips the calling thread into/out of OS background mode (CPU + I/O
// deprioritized) following ScanProgress::gentle. Begin/end must pair on the
// same thread, so each scan thread owns one of these.
namespace {
struct GentleMode {
    bool on = false;
    void apply(bool want) {
        if (want == on) return;
        SetThreadPriority(GetCurrentThread(), want ? THREAD_MODE_BACKGROUND_BEGIN
                                                   : THREAD_MODE_BACKGROUND_END);
        on = want;
    }
    ~GentleMode() { apply(false); }
};
} // namespace

// A file moved inside the library comes back from the walk under a NEW path,
// so the upsert inserts a fresh row and the old one lingers as a ghost — the
// user's "reorganised the drive, rescan added 4000 songs". Same size + same
// mtime (100 ns ticks; moves and Explorer copies keep it) is the same file:
// fold the fresh row into its ghost so the media_id — playlists, rotation,
// history, cues, BPM, key, hidden — survives, as relocateFolder does for a
// whole folder. Library-wide, not just under the scanned root, so a move
// between two roots is caught and a library that already has the duplicates
// heals on its next scan. A ghost is only a ghost when its drive is present
// and the file is not on it (unplugged drives and UNC are never judged — see
// decisions.md). Both copies present = a real duplicate, left alone.
// ponytail: stats every row of every same-size+mtime group per scan; fine at
// ~100k rows, track "rows added this scan" if genuine duplicates explode.
static int reconcileMoves(Db& db) {
    struct Row {
        int64_t id;
        std::wstring path, artist, title;
    };
    struct Pair {
        int64_t ghost, fresh;
        std::wstring path, guessArtist, guessTitle;
    };
    int driveUp[26] = {}; // 0 unknown, 1 present, -1 absent: one stat per letter
    auto missing = [&](const std::wstring& p) {
        if (p.size() < 3 || p[1] != L':' || !iswalpha(p[0])) return false; // UNC
        int& up = driveUp[towupper(p[0]) - L'A'];
        if (!up)
            up = GetFileAttributesW(p.substr(0, 3).c_str()) == INVALID_FILE_ATTRIBUTES
                     ? -1 : 1;
        if (up < 0) return false; // drive absent: says nothing about the file
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
        // only "not there" is gone; a USB I/O hiccup is not judged
        const DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
    };
    auto fileName = [](const std::wstring& p) {
        return lowerCopy(p.substr(p.find_last_of(L'\\') + 1));
    };
    std::vector<Pair> pairs;
    auto pairGroup = [&](std::vector<Row>& g) {
        std::vector<Row> ghosts, fresh;
        for (Row& r : g)
            if (missing(r.path)) ghosts.push_back(std::move(r));
            else fresh.push_back(std::move(r));
        g.clear();
        if (ghosts.empty() || fresh.empty()) return;
        // fresh rows on disk only — stat them once a ghost needs a partner
        std::erase_if(fresh, [](const Row& r) {
            return GetFileAttributesW(r.path.c_str()) == INVALID_FILE_ATTRIBUTES;
        });
        // The moved file's row was inserted after its ghost (higher id).
        // Same-name partners first, so a renamed ghost cannot take another
        // ghost's partner; then the leftovers by newest row.
        for (const bool sameName : {true, false})
            for (const Row& gh : ghosts) { // ordered by id
                int best = -1;
                for (int i = int(fresh.size()) - 1; i >= 0 && best < 0; --i)
                    if (fresh[i].id > gh.id &&
                        (!sameName || fileName(fresh[i].path) == fileName(gh.path)))
                        best = i;
                if (best < 0) continue;
                if (std::any_of(pairs.begin(), pairs.end(),
                                [&](const Pair& p) { return p.ghost == gh.id; }))
                    continue; // paired in the same-name pass
                std::wstring ga, gt; // what the scanner guessed from the old name
                filenameGuess(fs::path(gh.path), ga, gt);
                pairs.push_back({gh.id, fresh[best].id, std::move(fresh[best].path),
                                 ga, gt});
                fresh.erase(fresh.begin() + best);
            }
    };
    {
        Db::Stmt q;
        db.prepare(q, "SELECT m.id, m.path, m.file_size, m.modified_time, "
                      "IFNULL(m.artist,''), IFNULL(m.title,'') "
                      "FROM media_item m JOIN (SELECT file_size fs, modified_time mt "
                      "FROM media_item WHERE file_size>0 GROUP BY 1,2 "
                      "HAVING COUNT(*)>1) g ON m.file_size=g.fs AND "
                      "m.modified_time=g.mt ORDER BY 3, 4, 1");
        std::vector<Row> g;
        int64_t fs = -1, mt = -1;
        while (q.step()) {
            if (q.colInt(2) != fs || q.colInt(3) != mt) {
                pairGroup(g);
                fs = q.colInt(2);
                mt = q.colInt(3);
            }
            g.push_back({q.colInt(0), wide(q.colText(1)), wide(q.colText(4)),
                         wide(q.colText(5))});
        }
        pairGroup(g);
    }
    if (pairs.empty()) return 0;

    // The ghost keeps its identity (playlists, rotation, history) and the
    // user's edits; the fresh row contributes the new path, its CDG pairing,
    // anything the ghost never got (analysis, cues), its title/artist when
    // the ghost's were only the scanner's guess from the old file name, and
    // its visibility — a ghost hidden because it would not load must not
    // take the playable copy out of search with it.
    static const char* kSql[] = {
        "UPDATE playlist_item SET media_id=?1 WHERE media_id=?2",
        "UPDATE singer_queue_item SET media_id=?1 WHERE media_id=?2",
        "UPDATE play_history SET media_id=?1 WHERE media_id=?2",
        "UPDATE media_item AS g SET (type, bpm, music_key, duration_ms, genre, "
        "year, cue_in_ms, cue_out_ms, hidden, artist, title) = "
        "(SELECT f.type, CASE WHEN IFNULL(g.bpm,0)>0 THEN g.bpm ELSE f.bpm END, "
        "CASE WHEN IFNULL(g.music_key,0)<>0 THEN g.music_key ELSE f.music_key END, "
        "CASE WHEN IFNULL(g.duration_ms,0)>0 THEN g.duration_ms ELSE f.duration_ms END, "
        "COALESCE(g.genre, f.genre), "
        "CASE WHEN IFNULL(g.year,0)>0 THEN g.year ELSE f.year END, "
        "CASE WHEN IFNULL(g.cue_in_ms,0)>0 THEN g.cue_in_ms ELSE f.cue_in_ms END, "
        "CASE WHEN IFNULL(g.cue_out_ms,0)>0 THEN g.cue_out_ms ELSE f.cue_out_ms END, "
        "MIN(IFNULL(g.hidden,0), IFNULL(f.hidden,0)), "
        "CASE WHEN IFNULL(g.artist,'')=?4 AND IFNULL(g.title,'')=?5 "
        "THEN f.artist ELSE g.artist END, "
        "CASE WHEN IFNULL(g.artist,'')=?4 AND IFNULL(g.title,'')=?5 "
        "THEN f.title ELSE g.title END "
        "FROM media_item f WHERE f.id=?2) WHERE g.id=?1",
        "DELETE FROM media_item WHERE id=?2", // frees the UNIQUE path
        "UPDATE media_item SET path=?3, search_f=fold(COALESCE(title,''))||"
        "char(10)||fold(COALESCE(artist,''))||char(10)||fold(?3) WHERE id=?1",
    };
    bool ok = db.exec("BEGIN;");
    for (const Pair& p : pairs) {
        for (const char* sql : kSql) {
            Db::Stmt s;
            ok = ok && db.prepare(s, sql);
            if (!ok) break;
            // unused parameters just fail to bind (SQLITE_RANGE, ignored)
            s.bind(1, p.ghost).bind(2, p.fresh).bind(3, utf8(p.path));
            s.bind(4, utf8(p.guessArtist)).bind(5, utf8(p.guessTitle));
            ok = s.run();
        }
        if (!ok) break;
    }
    // all or nothing: a half-merged pair would strand a playlist entry
    db.exec(ok ? "COMMIT;" : "ROLLBACK;");
    return ok ? int(pairs.size()) : 0;
}

ScanStats scanDirectory(Db& db, const std::wstring& root, ScanProgress* prog,
                        bool readFileTags) {
    ScanStats st;
    ScanProgress local;
    if (!prog) prog = &local;
    prog->reset();

    // Phase 1 — one pass over the tree: collect media entries and a lowercase
    // path set, so CDG pairing needs no per-file exists() round-trips (those
    // are painfully slow on external drives).
    struct Raw {
        std::wstring path, ext;
        int64_t size, mtime;
    };
    std::vector<Raw> raw;
    std::set<std::wstring> all; // lowercased paths of every media file seen
    std::error_code ec;
    GentleMode gm; // walk yields to live playback (phase 4 commits stay
                   // normal priority: they hold the write lock briefly)
    for (fs::recursive_directory_iterator
             it(root, fs::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec)) {
        if (ec || prog->cancel.load(std::memory_order_relaxed)) break;
        gm.apply(prog->gentle.load(std::memory_order_relaxed));
        if (!it->is_regular_file(ec)) continue;
        const fs::path& p = it->path();
        std::wstring ext = lowerCopy(p.extension().wstring());
        if (!kAudioExt.count(ext) && !kVideoExt.count(ext) && ext != L".zip" &&
            ext != L".cdg")
            continue;
        const std::wstring w = p.wstring();
        raw.push_back({w, ext, int64_t(it->file_size(ec)),
                       int64_t(fs::last_write_time(p, ec).time_since_epoch().count())});
        all.insert(lowerCopy(w));
        prog->walked.store(int(raw.size()), std::memory_order_relaxed);
    }

    // Classify in memory (pairing against the set).
    std::vector<Item> items;
    items.reserve(raw.size());
    for (Raw& r : raw) {
        std::string type;
        if (kAudioExt.count(r.ext)) {
            type = all.count(lowerCopy(swapExt(r.path, L".cdg"))) ? "mp3g" : "audio";
        } else if (kVideoExt.count(r.ext)) {
            type = "video";
        } else if (r.ext == L".zip") {
            // Recorded unvalidated; content is checked at load time by
            // extractKaraokeZip (a bad zip errors and the queue self-heals).
            type = "karaoke_zip";
        } else { // .cdg: represented by its audio partner when one exists
            bool paired = false;
            for (const auto& ae : kAudioExt)
                if (all.count(lowerCopy(swapExt(r.path, ae)))) { paired = true; break; }
            if (paired) continue;
            type = "unsupported"; // orphan .cdg
        }
        items.push_back({std::move(r.path), std::move(type), r.size, r.mtime});
    }
    st.seen = int(items.size());

    // Phase 2 — diff against the library: unchanged files skip everything.
    std::vector<Item> work;
    work.reserve(items.size());
    for (Item& i : items) {
        Db::Stmt q;
        db.prepare(q, "SELECT id, file_size, modified_time, (genre IS NULL) "
                      "FROM media_item WHERE path=?1");
        q.bind(1, utf8(i.path));
        if (q.step()) {
            i.existed = true;
            // genre NULL = row predates tag columns: rescan it once
            if (q.colInt(1) == i.size && q.colInt(2) == i.mtime && q.colInt(3) == 0) {
                ++st.unchanged;
                continue;
            }
        }
        work.push_back(std::move(i));
    }
    prog->total.store(int(work.size()));
    prog->phase.store(1);

    // Phase 3 — parallel tag reads: the dominant cost (each read opens and
    // parses the file), and external drives love queue depth.
    std::vector<Meta> metas(work.size());
    std::atomic<size_t> next{0};
    if (!readFileTags) // fast import: filenames only, nothing to read
        for (Meta& m : metas) m.ready.store(true);
    const unsigned nThreads =
        readFileTags ? std::clamp(std::thread::hardware_concurrency(), 2u, 8u) : 0;
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < nThreads; ++t) {
        workers.emplace_back([&]() {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            GentleMode gm; // tag reads yield to live playback
            for (;;) {
                gm.apply(prog->gentle.load(std::memory_order_relaxed));
                const size_t i = next.fetch_add(1);
                if (i >= work.size() || prog->cancel.load(std::memory_order_relaxed))
                    break;
                Meta& m = metas[i];
                if (work[i].type != "unsupported")
                    readTags(work[i].path, m.artist, m.title, m.genre, m.year,
                             m.durMs, m.bpm);
                m.ready.store(true, std::memory_order_release);
            }
            CoUninitialize();
        });
    }

    // Phase 4 — insert in order as results land; chunked commits keep most of
    // a long import if anything dies mid-way.
    auto junk = [](std::wstring t) { // rip-tool tags lose to the filename
        std::transform(t.begin(), t.end(), t.begin(), ::towlower);
        for (auto* j : {L"piste", L"track", L"audiotrack", L"unknown"})
            if (t.rfind(j, 0) == 0) return true;
        return t.empty();
    };
    db.exec("BEGIN;");
    bool cancelled = false;
    for (size_t i = 0; i < work.size() && !cancelled; ++i) {
        while (!metas[i].ready.load(std::memory_order_acquire)) {
            if (prog->cancel.load(std::memory_order_relaxed)) {
                cancelled = true; // committed rows stay; rescan resumes here
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        if (cancelled) break;
        const Item& w = work[i];
        const Meta& m = metas[i];
        std::wstring artist, title;
        filenameGuess(fs::path(w.path), artist, title);
        if (!junk(m.title)) title = m.title;
        if (!junk(m.artist)) artist = m.artist;

        Db::Stmt up;
        db.prepare(up,
                   "INSERT INTO media_item(path,type,title,artist,duration_ms,file_size,"
                   "modified_time,status,genre,year,bpm,search_f) "
                   "VALUES(?1,?2,?3,?4,?5,?6,?7,'ok',?8,?9,?10,"
                   "fold(?3)||char(10)||fold(?4)||char(10)||fold(?1)) "
                   "ON CONFLICT(path) DO UPDATE SET type=?2,title=?3,artist=?4,"
                   "duration_ms=?5,file_size=?6,modified_time=?7,genre=?8,year=?9,"
                   // only changed files reach this statement, so a file that
                   // was marked unreadable gets another chance once it changes
                   "status='ok',"
                   "bpm=CASE WHEN ?10>0 THEN ?10 ELSE bpm END,"
                   "search_f=fold(?3)||char(10)||fold(?4)||char(10)||fold(?1)");
        up.bind(1, utf8(w.path)).bind(2, w.type).bind(3, utf8(title)).bind(4, utf8(artist));
        up.bind(5, m.durMs).bind(6, w.size).bind(7, w.mtime).bind(9, m.year);
        up.bind(10, m.bpm);
        if (readFileTags) up.bind(8, utf8(m.genre));
        else up.bindNull(8); // NULL genre = "rescan once" marker: a later
                             // tags-enabled import fills these rows in
        up.step();
        if (w.type == "unsupported") ++st.unsupported;
        w.existed ? ++st.updated : ++st.added;
        prog->done.store(int(i + 1), std::memory_order_relaxed);
        if ((i + 1) % 500 == 0) {
            db.exec("COMMIT;");
            db.exec("BEGIN;");
            wprintf(L"  ...%zu / %zu\n", i + 1, work.size());
            fflush(stdout);
        }
    }
    db.exec("COMMIT;");
    for (auto& t : workers) t.join();
    if (!cancelled && !prog->cancel.load()) st.moved = reconcileMoves(db);
    return st;
}
