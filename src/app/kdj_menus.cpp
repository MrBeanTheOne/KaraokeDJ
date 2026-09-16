#include "app/kdj.h"

// -------------------------------------------------------------- context menus

int showMenu(HWND hwnd, float scale, float cx, float cy,
                    const std::vector<std::wstring>& items, int checked) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); ++i)
        AppendMenuW(m, MF_STRING | (int(i) == checked ? MF_CHECKED : 0), UINT(i + 1),
                    items[i].c_str());
    POINT pt{LONG(cx * scale), LONG(cy * scale)}; // DIPs -> physical client
    ClientToScreen(hwnd, &pt);
    const int r = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd,
                                 nullptr);
    DestroyMenu(m);
    return r - 1;
}

// Track context menu with "Add to rotation ▸" and "Add to playlist ▸"
// submenus inserted at display position subAt. Returns 0..n-1 for base items,
// 1000+i for singers[i], 1099 for "New singer…", 2000+i for playlists[i],
// -1 for dismissed.
int showTrackMenu(App& a, HWND hwnd, float cx, float cy,
                         const std::vector<std::wstring>& items, size_t subAt,
                         const std::vector<std::wstring>& singers) {
    HMENU sub = CreatePopupMenu();
    const size_t nS = (std::min)(singers.size(), size_t(90)); // ids stay < 1099
    for (size_t i = 0; i < nS; ++i)
        AppendMenuW(sub, MF_STRING, UINT(1001 + i), singers[i].c_str());
    if (nS) AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sub, MF_STRING, 1100, L"New singer…");
    HMENU plSub = CreatePopupMenu();
    const size_t nP = (std::min)(a.playlists.size(), size_t(90));
    for (size_t i = 0; i < nP; ++i)
        AppendMenuW(plSub, MF_STRING, UINT(2001 + i), a.playlists[i].first.c_str());
    if (!nP) AppendMenuW(plSub, MF_STRING | MF_GRAYED, 0, L"(no playlists yet)");
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i <= items.size(); ++i) {
        if (i == subAt) {
            AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(sub),
                        L"Add to rotation");
            AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(plSub),
                        L"Add to playlist");
        }
        if (i < items.size())
            AppendMenuW(m, MF_STRING, UINT(i + 1), items[i].c_str());
    }
    POINT pt{LONG(cx * a.uiScale), LONG(cy * a.uiScale)}; // DIPs -> physical
    ClientToScreen(hwnd, &pt);
    const int r = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd,
                                 nullptr);
    DestroyMenu(m); // destroys the submenu too
    return r - 1;
}

// Persist a row move inside a playlist: renumber items 1..N by their current
// position order (robust to gaps left by folder removal, and to duplicates —
// it moves playlist_item ids, not media ids).
void movePlaylistItem(App& a, int64_t playlistId, int from, int to) {
    std::vector<int64_t> ids;
    Db::Stmt q;
    a.db.prepare(q,
                 "SELECT id FROM playlist_item WHERE playlist_id=?1 ORDER BY position");
    q.bind(1, playlistId);
    while (q.step()) ids.push_back(q.colInt(0));
    if (from < 0 || from >= int(ids.size()) || to < 0 || to >= int(ids.size()) ||
        from == to)
        return;
    const int64_t id = ids[from];
    ids.erase(ids.begin() + from);
    ids.insert(ids.begin() + to, id);
    a.db.exec("BEGIN");
    for (size_t i = 0; i < ids.size(); ++i) {
        Db::Stmt u;
        a.db.prepare(u, "UPDATE playlist_item SET position=?2 WHERE id=?1");
        u.bind(1, ids[i]).bind(2, int64_t(i + 1));
        u.step();
    }
    a.db.exec("COMMIT");
}

// Reorder the rotation: ONLY the active singers (waiting/singing) are
// renumbered, in their new display order. Finished rows keep their original
// positions — the history section must never reshuffle. Actives get positions
// above every existing one so the two groups can't collide.
void moveSingerRow(App& a, int from, int to) {
    std::vector<int64_t> act;
    int64_t maxPos = 0;
    for (const auto& sr : a.singers) {
        if (sr.itemId < 0) continue;
        if (sr.status == "waiting" || sr.status == "singing")
            act.push_back(sr.itemId);
        maxPos = (std::max)(maxPos, sr.pos);
    }
    if (from < 0 || from >= int(act.size()) || to < 0 || to >= int(act.size()) ||
        from == to)
        return;
    const int64_t id = act[from];
    act.erase(act.begin() + from);
    act.insert(act.begin() + to, id);
    a.db.exec("BEGIN");
    int64_t pos = maxPos + 1;
    for (int64_t v : act) {
        Db::Stmt u;
        a.db.prepare(u, "UPDATE singer_queue_item SET position=?2 WHERE id=?1");
        u.bind(1, v).bind(2, pos++);
        u.step();
    }
    a.db.exec("COMMIT");
    a.navDirty = true;
}

void queueAllPlaylist(App& a, int64_t playlistId) {
    Db::Stmt q;
    a.db.prepare(q, "SELECT m.id, m.artist, m.title, m.path, m.type, m.duration_ms "
                    "FROM playlist_item pi JOIN media_item m ON m.id=pi.media_id "
                    "WHERE pi.playlist_id=?1 ORDER BY pi.position");
    q.bind(1, playlistId);
    size_t n = 0;
    for (; q.step(); ++n) a.queue.push_back(readMatch(q));
    a.status = L"queued " + std::to_wstring(n) + L" tracks";
}

// Remove everything under a folder from the library DATABASE (files on disk
// are untouched). Playlist/rotation entries pointing at those tracks go too
// (their FKs have no cascade). Paths are BINARY-collated, so "prefix <= path
// < prefix+1" is an index range covering the whole subtree — no LIKE escaping.
void removeFolder(App& a, const std::wstring& folder) {
    const std::string lo = utf8(folder + L"\\");
    std::string hi = lo;
    ++hi.back();
    int64_t n = 0;
    {
        Db::Stmt c;
        a.db.prepare(c, "SELECT COUNT(*) FROM media_item WHERE path>=?1 AND path<?2");
        c.bind(1, lo).bind(2, hi);
        if (c.step()) n = c.colInt(0);
    }
    a.confirmPath = folder;
    askConfirm(a, App::ConfirmAction::RemoveFolder, L"REMOVE FOLDER FROM LIBRARY",
               std::to_wstring(n) + L" tracks leave the library: " + folder,
               L"Playlist / rotation entries using them go too. Files on disk "
               L"are NOT touched.");
}

void performRemoveFolder(App& a, const std::wstring& folder) {
    const std::string lo = utf8(folder + L"\\");
    std::string hi = lo;
    ++hi.back(); // '\\' -> ']'
    int64_t n = 0;
    {
        Db::Stmt c;
        a.db.prepare(c, "SELECT COUNT(*) FROM media_item WHERE path>=?1 AND path<?2");
        c.bind(1, lo).bind(2, hi);
        if (c.step()) n = c.colInt(0);
    }
    a.db.exec("BEGIN");
    {
        Db::Stmt q;
        a.db.prepare(q, "DELETE FROM playlist_item WHERE media_id IN "
                        "(SELECT id FROM media_item WHERE path>=?1 AND path<?2)");
        q.bind(1, lo).bind(2, hi);
        q.step();
    }
    {
        Db::Stmt q;
        a.db.prepare(q, "DELETE FROM singer_queue_item WHERE media_id IN "
                        "(SELECT id FROM media_item WHERE path>=?1 AND path<?2)");
        q.bind(1, lo).bind(2, hi);
        q.step();
    }
    {
        Db::Stmt q;
        a.db.prepare(q, "DELETE FROM media_item WHERE path>=?1 AND path<?2");
        q.bind(1, lo).bind(2, hi);
        q.step();
    }
    {
        Db::Stmt q; // the folder itself and any imported roots inside it
        a.db.prepare(q, "DELETE FROM scan_root WHERE path=?3 OR "
                        "(path>=?1 AND path<?2)");
        q.bind(1, lo).bind(2, hi).bind(3, utf8(folder));
        q.step();
    }
    a.db.exec("COMMIT");
    const std::wstring pfx = folder + L"\\";
    if (a.nav == NavMode::Folder && a.navFolder.compare(0, pfx.size(), pfx) == 0) {
        a.nav = NavMode::Library; // we were browsing inside the removed folder
        a.navFolder.clear();
    }
    a.expanded.erase(folder);
    a.navDirty = a.searchDirty = true;
    a.status = L"removed " + std::to_wstring(n) + L" tracks: " + folder;
}

void addToPlaylistDb(App& a, int64_t playlistId, const Match& m) {
    if (!m.id) return;
    Db::Stmt q;
    a.db.prepare(q, "INSERT INTO playlist_item(playlist_id,media_id,position) "
                    "VALUES(?1,?2,(SELECT IFNULL(MAX(position),0)+1 FROM playlist_item "
                    "WHERE playlist_id=?1))");
    q.bind(1, playlistId).bind(2, m.id);
    q.step();
}

void handleMenu(App& a, HWND hwnd) {
    const MenuReq req = a.menu;
    a.menu = {};
    if (req.index <= -2 && req.index >= -4) { // folder / image pickers (STA)
        if (a.pickThread.joinable()) return; // picker already open
        a.pickKind = req.index == -3 ? 1 : req.index == -4 ? 2 : 0;
        const bool file = req.index <= -3;
        a.pickThread = std::thread([&a, hwnd, file]() {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            a.pickResult = file ? pickFile(hwnd) : pickFolder(hwnd);
            CoUninitialize();
            a.pickDone.store(true);
        });
        return;
    }
    if (req.kind == MenuReq::None) return;

    if (req.kind == MenuReq::VideoOut) {
        std::vector<std::wstring> items = {L"Off"};
        const int n = VideoWindow::monitorCount();
        for (int i = 0; i < n; ++i) {
            const RECT r = VideoWindow::monitorRect(i);
            wchar_t t[64];
            swprintf(t, 64, L"Monitor %d  (%ldx%ld)%ls", i, r.right - r.left,
                     r.bottom - r.top, i == 0 ? L" — main" : L"");
            items.push_back(t);
        }
        const int sel = showMenu(hwnd, a.uiScale, req.x, req.y, items,
                                 a.outMonitor < 0 ? 0 : a.outMonitor + 1);
        if (sel == 0) { a.fullOut.reset(); a.outMonitor = -1; }
        else if (sel > 0) {
            a.fullOut.reset();
            a.fullOut = std::make_unique<VideoWindow>();
            if (a.fullOut->create(sel - 1)) a.outMonitor = a.chosenMonitor = sel - 1;
            else { a.fullOut.reset(); a.outMonitor = -1; }
        }
    } else if (req.kind == MenuReq::Automix) {
        const int sel = showMenu(hwnd, a.uiScale, req.x, req.y,
                                 {L"Fade transition", L"Smart  (skip/detect silence)"},
                                 a.mixMode == MixMode::Smart ? 1 : 0);
        if (sel == 0) { a.mixMode = MixMode::Fade; a.automixOn = true; }
        else if (sel == 1) { a.mixMode = MixMode::Smart; a.automixOn = true; }
    } else if (req.kind == MenuReq::BrowserRow && req.index >= 0 &&
               req.index < int(a.results.size())) {
        const Match m = a.results[req.index];
        const std::vector<std::wstring> items = {L"Mix now", L"Add to queue",
                                                 L"Play next (queue front)",
                                                 L"Edit tags…"};
        const auto singers = rotationSingers(a);
        const int sel = showTrackMenu(a, hwnd, req.x, req.y, items, 4, singers);
        if (sel == 0) playNow(a, m);
        else if (sel == 1) a.queue.push_back(m);
        else if (sel == 2) { // play next: displace an auto-cued deck if needed
            const int act = a.mixer.activeDeck.load();
            const int idle = act < 0 ? 0 : 1 - act;
            if (a.autoCued[idle] && a.mixer.fadeTo.load() < 0 && a.pendingFade != idle) {
                rescueAutoCue(a, idle);
                stopDeck(a, idle); // freed: the new front preloads next tick
            }
            a.queue.push_front(m);
        }
        else if (sel == 3) { // tag editor (replaces the old artist<->title swap)
            if (m.id) {
                a.tagEditItem = m;
                a.tagField[0] = m.artist;
                a.tagField[1] = m.title;
                a.tagField[2] = m.genre;
                a.tagField[3] = m.year > 0 ? std::to_wstring(m.year) : L"";
                a.tagFocus = 1;
                a.prompt = App::Prompt::TagEdit;
            } else {
                a.status = L"not in the library (import it first)";
            }
        }
        else if (sel >= 2000 && sel - 2000 < int(a.playlists.size())) {
            addToPlaylistDb(a, a.playlists[sel - 2000].second, m);
            a.status = L"added to " + a.playlists[sel - 2000].first;
            if (a.nav == NavMode::Playlist) a.searchDirty = true;
        }
        else if (sel >= 1000 && sel - 1000 < int(singers.size())) {
            a.singerName = singers[sel - 1000]; // picked singer becomes current
            addToRotationAs(a, m, a.singerName);
        }
        else if (sel == 1099) {
            a.prompt = App::Prompt::NewSinger;
            a.rotAddPending = m;
            a.promptText.clear();
        }
    } else if (req.kind == MenuReq::QueueRow && req.index >= 0 &&
               req.index < int(a.queue.size())) {
        const Match m = a.queue[req.index];
        const auto singers = rotationSingers(a);
        const int sel = showTrackMenu(a, hwnd, req.x, req.y,
                                      {L"Play now", L"Remove"}, 2, singers);
        if (sel == 0) {
            a.queue.erase(a.queue.begin() + req.index);
            playNow(a, m);
        } else if (sel == 1) {
            a.queue.erase(a.queue.begin() + req.index);
        } else if (sel >= 2000 && sel - 2000 < int(a.playlists.size())) {
            addToPlaylistDb(a, a.playlists[sel - 2000].second, m);
            a.status = L"added to " + a.playlists[sel - 2000].first;
        } else if (sel >= 1000 && sel - 1000 < int(singers.size())) {
            a.singerName = singers[sel - 1000];
            addToRotationAs(a, m, a.singerName); // stays in the queue too
        } else if (sel == 1099) {
            a.prompt = App::Prompt::NewSinger;
            a.rotAddPending = m;
            a.promptText.clear();
        }
    } else if (req.kind == MenuReq::DeckWave && req.index >= 0 && req.index < 2) {
        const int d = req.index;
        const Match& m = a.deckMatch[d];
        const uint64_t totalFr = a.decks[d]->totalFrames.load();
        if (m.id && totalFr) {
            const D2D1_RECT_F& wf = a.rcWave[d];
            const float frac =
                std::clamp((req.x - wf.left) / (wf.right - wf.left), 0.f, 1.f);
            const int64_t ms = int64_t(frac * double(totalFr) / kRate * 1000.0);
            const int sel = showMenu(hwnd, a.uiScale, req.x, req.y,
                                     {L"Set START marker here",
                                      L"Set END marker here", L"Clear markers"});
            if (sel >= 0) {
                if (sel == 0) a.cueIn[d] = ms;
                else if (sel == 1) a.cueOut[d] = ms;
                else a.cueIn[d] = a.cueOut[d] = 0;
                if (a.cueIn[d] && a.cueOut[d] && a.cueOut[d] <= a.cueIn[d])
                    a.cueOut[d] = 0; // markers must stay ordered
                Db::Stmt q;
                a.db.prepare(q, "UPDATE media_item SET cue_in_ms=?2, cue_out_ms=?3 "
                                "WHERE id=?1");
                q.bind(1, m.id).bind(2, a.cueIn[d]).bind(3, a.cueOut[d]);
                q.step();
                a.status = sel == 2 ? L"markers cleared: " + m.label
                           : sel == 0
                               ? L"START marker @ " + fmtTime(double(ms) / 1000) +
                                     L": " + m.label
                               : L"END marker @ " + fmtTime(double(ms) / 1000) +
                                     L": " + m.label;
            }
        } else {
            a.status = L"markers need a library track";
        }
    } else if (req.kind == MenuReq::RotationRow) {
        const int sel = showMenu(hwnd, a.uiScale, req.x, req.y,
                                 {L"Clear finished singers",
                                  L"Clear ENTIRE rotation (new night)"});
        if (sel == 0) {
            a.db.exec("DELETE FROM singer_queue_item WHERE status NOT IN "
                      "('waiting','singing')");
            a.navDirty = true;
            a.status = L"finished singers cleared";
        } else if (sel == 1) {
            askConfirm(a, App::ConfirmAction::ClearRotation,
                       L"NEW NIGHT — CLEAR ROTATION",
                       L"All waiting and finished singers are removed.",
                       L"Tonight's play history is kept.");
        }
    } else if (req.kind == MenuReq::HistoryRow) {
        const int sel = showMenu(hwnd, a.uiScale, req.x, req.y,
                                 {L"Clear tonight's history (new night)"});
        if (sel == 0) {
            askConfirm(a, App::ConfirmAction::ClearHistory,
                       L"NEW NIGHT — CLEAR HISTORY",
                       L"Tonight's play history and the played-tonight dots reset.",
                       L"Older history is kept.");
        }
    } else if (req.kind == MenuReq::CleanMissing) {
        cleanMissingFiles(a);
    } else if (req.kind == MenuReq::PlaylistRow && req.plId >= 0) {
        const int sel = showMenu(hwnd, a.uiScale, req.x, req.y,
                                 {L"Queue all", L"Delete playlist"});
        if (sel == 0) queueAllPlaylist(a, req.plId);
        else if (sel == 1) {
            std::wstring name;
            for (auto& [n, id] : a.playlists)
                if (id == req.plId) name = n;
            a.confirmId = req.plId;
            askConfirm(a, App::ConfirmAction::DeletePlaylist, L"DELETE PLAYLIST",
                       L"Delete \"" + name + L"\"?",
                       L"Tracks stay in the library; anything queued keeps "
                       L"playing.");
        }
    } else if (req.kind == MenuReq::FolderRow && !req.path.empty()) {
        const int sel = showMenu(hwnd, a.uiScale, req.x, req.y,
                                 {L"Update library (rescan folder)",
                                  L"Remove from library"});
        if (sel == 0) queueRescan(a, req.path);
        else if (sel == 1) removeFolder(a, req.path);
    } else if (req.kind == MenuReq::SingerRow && req.index >= 0 &&
               req.index < int(a.singers.size())) {
        const SingerRow s = a.singers[req.index];
        const int sel = showMenu(hwnd, a.uiScale, req.x, req.y,
                                 {L"Sing now", L"Mark completed", L"Skip", L"No show"});
        if (sel == 0) singNow(a, s);
        else if (sel == 1) setSingerStatus(a, s.itemId, "completed");
        else if (sel == 2) setSingerStatus(a, s.itemId, "skipped");
        else if (sel == 3) setSingerStatus(a, s.itemId, "noshow");
    }
}

// ---------------------------------------------------------------- drop files

void dropExternal(App& a, float x, float y, const std::vector<std::wstring>& files) {
    bool first = true;
    for (const auto& f : files) {
        const DWORD attr = GetFileAttributesW(f.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            startImport(a, f); // a dropped folder is an import
            continue;
        }
        const Match m = matchFromPath(a, f);
        if (hit(a.rcDeck[0], x, y) && first) dropOnDeck(a, 0, m);
        else if (hit(a.rcDeck[1], x, y) && first) dropOnDeck(a, 1, m);
        else if (hit(a.rcBrowser, x, y) && a.nav == NavMode::Playlist && m.id) {
            addToPlaylistDb(a, a.navPlaylist, m);
            a.searchDirty = true;
        } else {
            a.queue.push_back(m);
        }
        first = false;
    }
    if (!files.empty()) a.status = L"dropped " + std::to_wstring(files.size()) + L" item(s)";
}
