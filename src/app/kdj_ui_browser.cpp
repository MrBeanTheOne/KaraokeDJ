#include "app/kdj.h"

// Left sidebar (library / playlists / folders / singers), the track
// browser and the queue drawer. Split out of kdj_ui.cpp.

std::vector<std::wstring> folderChildren(const App& a, const std::wstring& dir) {
    std::set<std::wstring> kids;
    const std::wstring prefix = dir + L"\\";
    for (const auto& d : a.allDirs) {
        if (d.size() <= prefix.size() || d.compare(0, prefix.size(), prefix) != 0)
            continue;
        const size_t sep = d.find(L'\\', prefix.size());
        kids.insert(sep == std::wstring::npos ? d : d.substr(0, sep));
    }
    return {kids.begin(), kids.end()};
}

void drawSidebar(App& a, Ui& ui, const D2D1_RECT_F& r) {
    ui.rect(r, cPanel, 10);
    ui.frameRect(r, cBorder, 10);

    if (!a.sidebarOpen) { // collapsed: the whole strip is the reopen button
        const bool over = hit(r, ui.in.mx, ui.in.my);
        if (over) ui.rect(r, cHover, 10);
        ui.text(rc(r.left, r.top + 8, r.right - r.left, 24), L"»", 16,
                over ? cText : cDim, 1, true);
        if (ui.in.pressed && hit(r, ui.in.pressX, ui.in.pressY))
            a.sidebarOpen = true;
        return;
    }

    // Import lives with the file browser: pinned at the bottom of the sidebar.
    const D2D1_RECT_F importBtn =
        rc(r.left + 10, r.bottom - 40, r.right - r.left - 54, 30);
    if (ui.button(402, importBtn,
                  a.scanning.load() ? L"IMPORTING…" : L"+ IMPORT FOLDER", cAccent) &&
        !a.scanning.load())
        a.menu = {MenuReq::None, -2, 0, 0}; // sentinel: open folder picker after draw
    if (ui.button(405, rc(r.right - 40, r.bottom - 40, 30, 30), L"«", cAccent))
        a.sidebarOpen = false;

    struct Row {
        std::wstring text, folder;
        int header = 0, depth = 0;
        bool expandable = false, expanded = false;
        NavMode mode = NavMode::Library;
        int64_t plId = -1;
    };
    std::vector<Row> rows;
    rows.push_back({L"BROWSE", L"", 1});
    wchar_t at[48];
    swprintf(at, 48, L"All tracks (%d)", a.libCount);
    rows.push_back({at, L"", 0, 0, false, false, NavMode::Library, -1});
    wchar_t hr2[48];
    swprintf(hr2, 48, L"Played tonight (%zu)", a.history.size());
    rows.push_back({hr2, L"", 0, 0, false, false, NavMode::History, -1});
    wchar_t hx[48];
    if (a.hiddenCount > 0) { // restore view for excluded versions
        swprintf(hx, 48, L"Excluded (%d)", a.hiddenCount);
        rows.push_back({hx, L"##hidden", 0, 0, false, false, NavMode::Library, -1});
    }
    rows.push_back({L"PLAYLISTS", L"", 1});
    for (auto& [name, id] : a.playlists)
        rows.push_back({name, L"", 0, 0, false, false, NavMode::Playlist, id});
    rows.push_back({L"+ New playlist", L"", 0, 0, false, false, NavMode::Playlist,
                    -2}); // plId -2 = create
    rows.push_back({L"FOLDERS", L"", 1});
    if (!a.roots.empty()) {
        // collapsible tree from the imported roots
        struct StackItem { std::wstring dir; int depth; };
        std::vector<StackItem> stack;
        for (auto it = a.roots.rbegin(); it != a.roots.rend(); ++it)
            stack.push_back({*it, 0});
        while (!stack.empty()) {
            const StackItem s = stack.back();
            stack.pop_back();
            const auto kids = folderChildren(a, s.dir);
            const bool exp = a.expanded.count(s.dir) != 0;
            rows.push_back({leafName(s.dir), s.dir, 0, s.depth, !kids.empty(), exp,
                            NavMode::Folder, -1});
            if (exp)
                for (auto it = kids.rbegin(); it != kids.rend(); ++it)
                    stack.push_back({*it, s.depth + 1});
        }
    } else { // legacy libraries scanned before roots existed: flat top folders
        for (auto& [dir, n] : a.flatFolders) {
            wchar_t t[96];
            swprintf(t, 96, L"%ls (%d)", leafName(dir).c_str(), n);
            rows.push_back({t, dir, 0, 0, false, false, NavMode::Folder, -1});
        }
    }
    rows.push_back({L"SINGERS", L"", 1});
    wchar_t sr[48];
    swprintf(sr, 48, L"Rotation (%zu)", a.singers.size());
    rows.push_back({sr, L"", 0, 0, false, false, NavMode::Singers, -1});

    const D2D1_RECT_F list = rc(r.left + 4, r.top + 6, r.right - r.left - 8,
                                r.bottom - r.top - 54); // bottom: import button
    ui.clipPush(list);
    if (hit(list, ui.in.mx, ui.in.my) && ui.in.wheel != 0)
        a.sideScroll = std::clamp(a.sideScroll - ui.in.wheel * 2, 0.f,
                                  (std::max)(0.f, float(rows.size()) -
                                                      (list.bottom - list.top) / 26.f));
    const float rowH = 26;
    for (size_t i = 0; i < rows.size(); ++i) {
        const float y = list.top + (float(i) - a.sideScroll) * rowH;
        if (y + rowH < list.top || y > list.bottom) continue;
        const Row& row = rows[i];
        const D2D1_RECT_F rr = rc(list.left, y, list.right - list.left, rowH - 2);
        if (row.header) {
            ui.text(rc(rr.left + 8, y + 4, rr.right - rr.left, rowH - 4), row.text, 10,
                    cDim, 0, true);
            continue;
        }
        const bool current =
            (row.mode == a.nav) &&
            (row.mode != NavMode::Playlist || row.plId == a.navPlaylist) &&
            (row.mode != NavMode::Folder || row.folder == a.navFolder) &&
            (row.mode != NavMode::Library ||
             (row.folder == L"##hidden") == a.showHidden);
        if (current) ui.rect(rr, cSel, 4);
        else if (hit(rr, ui.in.mx, ui.in.my)) ui.rect(rr, cHover, 4);

        const float indent = 8 + row.depth * 14.f;
        if (row.expandable)
            ui.text(rc(rr.left + indent, y, 16, rowH), row.expanded ? L"▾" : L"▸", 11,
                    cDim, 0, false);
        if (ui.in.rpressed && hit(rr, ui.in.rX, ui.in.rY)) {
            if (row.mode == NavMode::Playlist && row.plId >= 0)
                a.menu = {MenuReq::PlaylistRow, -1, ui.in.rX, ui.in.rY, row.plId};
            else if (row.mode == NavMode::Folder && !row.folder.empty())
                a.menu = {MenuReq::FolderRow, -1, ui.in.rX, ui.in.rY, -1, row.folder};
            else if (row.mode == NavMode::Singers)
                a.menu = {MenuReq::RotationRow, -1, ui.in.rX, ui.in.rY};
            else if (row.mode == NavMode::History)
                a.menu = {MenuReq::HistoryRow, -1, ui.in.rX, ui.in.rY};
        }
        if (ui.in.pressed && hit(rr, ui.in.pressX, ui.in.pressY) &&
            ui.in.pressX < list.right - 12) {
            const bool onArrow = row.expandable &&
                                 ui.in.pressX < rr.left + indent + 16;
            if (onArrow) {
                if (row.expanded) a.expanded.erase(row.folder);
                else a.expanded.insert(row.folder);
            } else if (row.mode == NavMode::Playlist && row.plId == -2) {
                a.prompt = App::Prompt::NewPlaylist; // "+ New playlist"
                a.promptText.clear();
            } else {
                a.nav = row.mode;
                a.navPlaylist = row.plId;
                a.navPlaylistName = row.mode == NavMode::Playlist ? row.text : L"";
                a.navFolder = row.mode == NavMode::Folder ? row.folder + L"\\" : L"";
                a.showHidden =
                    row.mode == NavMode::Library && row.folder == L"##hidden";
                a.focus = Focus::None;
                a.searchDirty = true;
                a.libScroll = 0; // new view: back to top
                a.selLib = -1;
            }
        }
        ui.text(rc(rr.left + indent + (row.expandable ? 16 : 4), y,
                   rr.right - rr.left - indent - 20, rowH),
                row.text, 12, current ? cText : cDim, 0, current);
    }
    ui.clipPop();
    scrollbar(a, ui, 701, list, rows.size(), rowH, a.sideScroll);
}

// Remove library rows whose file is gone. Rows on an absent DRIVE (unplugged
// external disk) are left alone — a disconnected library is not a deleted one.

void drawBrowser(App& a, Ui& ui, const D2D1_RECT_F& r) {
    a.rcBrowser = r;
    ui.rect(r, cPanel, 10);
    ui.frameRect(r, cBorder, 10);


    if (a.nav == NavMode::History) {
        wchar_t ht[48];
        swprintf(ht, 48, L"PLAYED TONIGHT  (%zu)", a.history.size());
        ui.text(rc(r.left + 14, r.top + 8, 300, 26), ht, 12, cDim, 0, true);
        ui.text(rc(r.left + 14, r.top + 34, r.right - r.left - 28, 18),
                L"double-click to play again", 11, cDim, 0, false);
    } else if (a.nav == NavMode::Singers) {
        ui.text(rc(r.left + 14, r.top + 8, 90, 26), L"SINGER", 12, cDim, 0, true);
        const D2D1_RECT_F nameBox = rc(r.left + 90, r.top + 8, 240, 26);
        ui.rect(nameBox, cInset, 7);
        ui.frameRect(nameBox, a.focus == Focus::SingerName ? cAccent : cBorder, 5);
        ui.text(rc(nameBox.left + 8, nameBox.top, nameBox.right - nameBox.left - 12, 26),
                a.singerFilter +
                    (a.focus == Focus::SingerName && caretOn() ? L"▏" : L""),
                13, cText, 0, false);
        if (ui.in.pressed && hit(nameBox, ui.in.pressX, ui.in.pressY))
            a.focus = Focus::SingerName;
        if (!a.singerFilter.empty() &&
            ui.button(306, rc(nameBox.right + 6, r.top + 8, 30, 26), L"✕", cRed)) {
            a.singerFilter.clear();
            a.navDirty = true;
        }
        ui.text(rc(nameBox.right + 44, r.top + 8, 400, 26),
                a.singerFilter.empty()
                    ? L"type a name: their upcoming songs + full history"
                    : L"upcoming on top, everything they sang below",
                11, cDim, 0, false);
        if (ui.button(303, rc(r.right - 132, r.top + 8, 120, 26), L"NEXT SINGER ▸",
                      cGreen, true)) {
            for (const auto& s : a.singers)
                if (s.status == "waiting") { singNow(a, s); break; }
        }
        if (!a.singerFilter.empty() && a.singers.empty())
            ui.text(rc(r.left + 14, r.top + 48, r.right - r.left - 28, 20),
                    L"No singer matching “" + a.singerFilter +
                        L"” — nothing upcoming and no plays recorded "
                        L"under that name.",
                    12, cDim, 0, false);
    } else {
        std::wstring title = a.nav == NavMode::Library ? L"SEARCH"
                             : a.nav == NavMode::Playlist ? a.navPlaylistName
                                                          : L"FOLDER";
        ui.text(rc(r.left + 14, r.top + 8, 150, 26), title, 12, cDim, 0, true);
        const D2D1_RECT_F box = rc(r.left + 100, r.top + 8, r.right - r.left - 290, 26);
        ui.rect(box, cInset, 7);
        ui.frameRect(box, a.focus == Focus::Search ? cAccent : cBorder, 5);
        ui.text(rc(box.left + 8, box.top, box.right - box.left - 12, 26),
                a.search +
                    (a.focus == Focus::Search && caretOn() ? L"▏" : L""),
                13, cText, 0, false);
        if (ui.in.pressed && hit(box, ui.in.pressX, ui.in.pressY)) a.focus = Focus::Search;
        if (ui.button(300, rc(r.right - 182, r.top + 8, 84, 26), L"+ QUEUE", cGreen))
            queueSelected(a);
        if (ui.button(301, rc(r.right - 92, r.top + 8, 80, 26), L"MIX ▸", cAccent, true)) {
            if (a.selLib >= 0 && a.selLib < int(a.results.size()))
                playNow(a, a.results[a.selLib]);
        }
    }

    // Column layout from the model: visible columns (colSeq order) share the
    // flexible width by normalized colFrac. Column ids: 0 TITLE 1 ARTIST
    // 2 GENRE 3 YEAR 4 BPM 5 TIME 6 KEY (sort ids map via kColSort).
    static const wchar_t* kColName[App::kNumCols] = {
        L"TITLE", L"ARTIST", L"GENRE", L"YEAR", L"BPM", L"TIME", L"KEY"};
    static const int kColSort[App::kNumCols] = {1, 0, 2, 3, 4, 5, 6};
    // BPM and TIME are numbers and read right-aligned; everything else left.
    auto colRight = [](int id) { return id == 4 || id == 5; };
    const float listLeft = r.left + 6, listRight = r.right - r.left - 12 + r.left + 6;
    const float cw = (listRight - listLeft) - 66 - 8;
    int visIds[App::kNumCols];
    int nVis = 0;
    float fracSum = 0.f;
    for (int k = 0; k < App::kNumCols; ++k) {
        const int id = a.colSeq[k];
        if (!a.colShow[id]) continue;
        visIds[nVis++] = id;
        fracSum += a.colFrac[id];
    }
    float colX[App::kNumCols]{}, colW[App::kNumCols]{};
    {
        float cx0 = listLeft + 66;
        for (int v = 0; v < nVis; ++v) {
            colX[v] = cx0;
            colW[v] = cw * (a.colFrac[visIds[v]] / fracSum);
            cx0 += colW[v];
        }
    }

    if (a.nav != NavMode::Singers && a.nav != NavMode::History) { // sort headers
        const float hy = r.top + 40;
        // Right-click the header strip = column manager (reorder/show).
        if (ui.in.rpressed && ui.in.rY >= hy - 4 && ui.in.rY <= hy + 20 &&
            ui.in.rX >= listLeft && ui.in.rX <= listRight)
            a.prompt = App::Prompt::Columns;
        // Dividers between consecutive visible columns: drag shifts the width
        // share between the two neighbours. Handled before sort clicks.
        for (int v = 1; v < nVis; ++v) {
            const D2D1_RECT_F dz = rc(colX[v] - 5, hy, 10, 20);
            const bool overDz = hit(dz, ui.in.mx, ui.in.my);
            if (overDz || a.colDrag == v) a.hoverResize = true;
            if (ui.in.pressed && hit(dz, ui.in.pressX, ui.in.pressY)) a.colDrag = v;
            ui.rect(rc(colX[v] - 4, hy, 1, 18),
                    a.colDrag == v || overDz ? cAccent : cBorder, 0);
        }
        if (!ui.in.down) a.colDrag = -1;
        if (a.colDrag >= 1 && a.colDrag < nVis && cw > 0) {
            const int li = visIds[a.colDrag - 1], ri = visIds[a.colDrag];
            const float pair = a.colFrac[li] + a.colFrac[ri];
            const float minF = 0.04f * fracSum;
            float newL = (ui.in.mx - colX[a.colDrag - 1]) / cw * fracSum;
            newL = std::clamp(newL, minF, pair - minF);
            a.colFrac[li] = newL;
            a.colFrac[ri] = pair - newL;
        }
        for (int v = 0; v < nVis; ++v) {
            const int id = visIds[v];
            const D2D1_RECT_F hr = rc(colX[v] + 8, hy, colW[v] - 16, 18);
            const bool active = a.sortCol == kColSort[id];
            std::wstring t = kColName[id];
            if (active) t += a.sortAsc ? L" ▲" : L" ▼";
            ui.text(hr, t, 10, active ? cAccent : cDim, colRight(id) ? 2 : 0, true);
            if (a.nav != NavMode::Playlist && a.colDrag < 0 && ui.in.pressed &&
                hit(hr, ui.in.pressX, ui.in.pressY)) {
                if (a.sortCol == kColSort[id]) a.sortAsc = !a.sortAsc;
                else { a.sortCol = kColSort[id]; a.sortAsc = true; }
                a.searchDirty = true;
                a.libScroll = 0; // new order: back to top
            }
        }
        ui.rect(rc(listLeft, hy + 19, listRight - listLeft, 1), cBorder, 0);
    }

    const D2D1_RECT_F list = rc(r.left + 6, r.top + 62, r.right - r.left - 12,
                                r.bottom - r.top - 68);
    ui.clipPush(list);
    const float rowH = 26;
    const size_t count = a.nav == NavMode::Singers    ? a.singers.size()
                         : a.nav == NavMode::History ? a.history.size()
                                                     : a.results.size();
    if (hit(list, ui.in.mx, ui.in.my) && ui.in.wheel != 0)
        a.libScroll = std::clamp(a.libScroll - ui.in.wheel * 3, 0.f,
                                 (std::max)(0.f, float(count) -
                                                     (list.bottom - list.top) / rowH));
    int sepIdx = INT_MAX; // singers: divider index, for per-section numbering
    if (a.nav == NavMode::Singers)
        for (size_t k = 0; k < a.singers.size(); ++k)
            if (a.singers[k].itemId < 0) { sepIdx = int(k); break; }
    for (size_t i = 0; i < count; ++i) {
        const float y = list.top + (float(i) - a.libScroll) * rowH;
        if (y + rowH < list.top || y > list.bottom) continue;
        const D2D1_RECT_F row = rc(list.left, y, list.right - list.left, rowH - 2);
        const bool over = hit(row, ui.in.mx, ui.in.my);
        if (int(i) == a.selLib || a.selRows.count(int(i))) ui.rect(row, cSel, 4);
        else if (over) ui.rect(row, cHover, 4);
        else if (i & 1) ui.rect(row, col(0x1A1B1F), 4); // zebra stripe
        if (ui.in.pressed && hit(row, ui.in.pressX, ui.in.pressY) &&
            ui.in.pressX < list.right - 12) {
            const bool multiView =
                a.nav != NavMode::Singers && a.nav != NavMode::History;
            if (a.nav == NavMode::Singers && a.singers[i].itemId >= 0 &&
                (a.singers[i].status == "waiting" ||
                 a.singers[i].status == "singing")) {
                a.dragArmed = true; // rotation reorder
                a.dragX0 = ui.in.pressX;
                a.dragY0 = ui.in.pressY;
                a.dragItem = Match{};
                a.dragItem.label = a.singers[i].singer + L" — " +
                                   a.singers[i].label;
                a.dragItems.clear();
                a.dragSinger = int(i);
                a.dragFromQueue = a.dragFromList = -1;
            }
            if (multiView && ui.in.shift && a.selLib >= 0) {
                a.selRows.clear(); // range from the anchor to here
                for (int k = (std::min)(a.selLib, int(i));
                     k <= (std::max)(a.selLib, int(i)); ++k)
                    a.selRows.insert(k);
            } else if (multiView && ui.in.ctrl) {
                if (!a.selRows.erase(int(i))) a.selRows.insert(int(i));
                a.selLib = int(i);
            } else {
                a.selLib = int(i);
                if (!a.selRows.count(int(i))) { // click outside the selection
                    a.selRows.clear();          // collapses it now; inside it
                    a.selRows.insert(int(i));   // stays armed for a group drag
                } else if (a.selRows.size() > 1) {
                    a.selCollapse = int(i); // no drag by release = collapse
                }
                if (multiView) {
                    a.dragArmed = true;
                    a.dragX0 = ui.in.pressX;
                    a.dragY0 = ui.in.pressY;
                    a.dragItem = a.results[i];
                    a.dragItems.clear();
                    if (a.selRows.size() > 1)
                        for (int k : a.selRows)
                            if (k >= 0 && k < int(a.results.size()))
                                a.dragItems.push_back(a.results[k]);
                    a.dragFromQueue = -1;
                    // Reorder only single rows in an unfiltered playlist view.
                    a.dragFromList = a.nav == NavMode::Playlist &&
                                             a.search.empty() &&
                                             a.dragItems.size() <= 1
                                         ? int(i) : -1;
                }
            }
        }
        if (ui.in.rpressed && hit(row, ui.in.rX, ui.in.rY) &&
            a.nav != NavMode::History &&
            !(a.nav == NavMode::Singers && a.singers[i].itemId < 0)) {
            a.selLib = int(i);
            a.menu = {a.nav == NavMode::Singers ? MenuReq::SingerRow : MenuReq::BrowserRow,
                      int(i), ui.in.rX, ui.in.rY};
        }
        if (a.nav == NavMode::History) {
            const App::HistRow& hrow = a.history[i];
            if (ui.in.dblclick && hit(row, ui.in.dblX, ui.in.dblY))
                playNow(a, hrow.song);
            ui.text(rc(row.left + 8, y, 52, rowH), hrow.when, 12, cDim, 0, false);
            ui.text(rc(row.left + 66, y, 130, rowH), hrow.singer, 13, cAccent, 0, true);
            ui.text(rc(row.left + 204, y, row.right - row.left - 264, rowH),
                    hrow.song.label, 13, cText, 0, false);
            ui.text(rc(row.right - 66, y, 48, rowH),
                    fmtTime(double(hrow.song.durMs) / 1000), 12, cDim, 2, false);
        } else if (a.nav == NavMode::Singers) {
            const SingerRow& s = a.singers[i];
            if (s.itemId == -1) { // section divider
                ui.text(rc(row.left + 8, y, 180, rowH),
                        a.singerFilter.empty() ? L"HISTORY — done tonight"
                                               : L"HISTORY — songs sung",
                        10, cDim, 0, true);
                ui.rect(rc(row.left + 190, y + 12, row.right - row.left - 196, 1),
                        cBorder, 0);
                continue;
            }
            if (ui.in.dblclick && hit(row, ui.in.dblX, ui.in.dblY)) {
                if (s.itemId == -2) playNow(a, s.song); // replay from history
                else singNow(a, s);
            }
            const D2D1_COLOR_F sc = s.status == "singing"    ? cGreen
                                    : s.status == "waiting"  ? cAccent
                                    : s.status == "completed" ? cDim
                                                              : cRed;
            if (s.itemId >= 0) { // rotation ordinal; history rows aren't slots
                wchar_t num[8]; // ordinal within its section, not raw position
                swprintf(num, 8, L"%d.",
                         int(i) < sepIdx ? int(i) + 1 : int(i) - sepIdx);
                ui.text(rc(row.left + 8, y, 30, rowH), num, 12, cDim, 0, false);
            }
            ui.text(rc(row.left + 40, y, 110, rowH), s.singer, 13, cText, 0, true);
            ui.text(rc(row.left + 156, y, 84, rowH),
                    s.itemId == -2 ? s.when : wide(s.status), 11, sc, 0, true);
            ui.text(rc(row.left + 244, y, row.right - row.left - 250, rowH), s.label, 13,
                    cDim, 0, false);
        } else {
            const Match& m = a.results[i];
            if (ui.in.dblclick && hit(row, ui.in.dblX, ui.in.dblY)) {
                a.selLib = int(i);
                playNow(a, m);
                if (a.nav == NavMode::Playlist) {
                    a.queue.clear();
                    for (size_t j = i + 1; j < a.results.size(); ++j)
                        if (!pathOffline(a.results[j].path, a.driveMask))
                            a.queue.push_back(a.results[j]);
                }
            }
            const bool played = m.id && a.playedTonight.count(m.id);
            // Its drive is unplugged: the row stays listed (searchable, and
            // it comes back by itself) but reads as unavailable rather than
            // waiting to fail on a double-click.
            const bool gone = pathOffline(m.path, a.driveMask);
            const D2D1_COLOR_F cMain = gone ? cGone : cText;
            const D2D1_COLOR_F cSub = gone ? cGone : cDim;
            if (played) ui.circle(row.left + 3, y + 12, 3, gone ? cGone : cRed);
            ui.text(rc(row.left + 8, y, 50, rowH), typeTag(m.type), 11,
                    gone ? cGone : played ? cDim : typeColor(m.type), 0, true);
            for (int v = 0; v < nVis; ++v) {
                const int id = visIds[v];
                const D2D1_RECT_F cell =
                    rc(colX[v], y, colW[v] - (colRight(id) ? 18.f : 8.f), rowH);
                switch (id) {
                case 0:
                    ui.text(cell, m.title.empty() ? m.label : m.title, 13, cMain,
                            0, false);
                    break;
                case 1: ui.text(cell, m.artist, 13, cSub, 0, false); break;
                case 2: ui.text(cell, m.genre, 12, cSub, 0, false); break;
                case 3:
                    ui.text(cell, m.year > 0 ? std::to_wstring(m.year) : L"", 12,
                            cSub, 0, false);
                    break;
                case 4:
                    ui.text(cell, m.bpm > 0 ? std::to_wstring(m.bpm) : L"", 12,
                            cSub, 2, false);
                    break;
                case 5:
                    ui.text(cell, fmtTime(double(m.durMs) / 1000), 12, cSub, 2,
                            false);
                    break;
                case 6: // detected musical key, blank until analysed
                    ui.text(cell, keyName(int(m.musicKey), 0), 12, cSub, 0,
                            false);
                    break;
                }
            }
        }
    }
    // Drop-target line while reordering the rotation (active section only).
    if (a.dragging && a.dragSinger >= 0 && a.nav == NavMode::Singers &&
        hit(list, ui.in.mx, ui.in.my)) {
        int nAct = 0;
        for (const auto& sr : a.singers)
            if (sr.itemId >= 0 &&
                (sr.status == "waiting" || sr.status == "singing"))
                ++nAct;
        const int idx = std::clamp(
            int((ui.in.my - list.top) / rowH + a.libScroll + 0.5f), 0, nAct);
        const float ly = list.top + (float(idx) - a.libScroll) * rowH - 1;
        ui.rect(rc(list.left, ly, list.right - list.left, 2), cAccent, 1);
    }
    // Drop-target line while reordering rows of this playlist.
    if (a.dragging && a.dragFromList >= 0 && a.nav == NavMode::Playlist &&
        hit(list, ui.in.mx, ui.in.my)) {
        const int idx = std::clamp(
            int((ui.in.my - list.top) / rowH + a.libScroll + 0.5f), 0, int(count));
        const float y = list.top + (float(idx) - a.libScroll) * rowH - 1;
        ui.rect(rc(list.left, y, list.right - list.left, 2), cAccent, 1);
    }
    ui.clipPop();
    a.rcBrowserList = list;
    scrollbar(a, ui, 702, list, count, rowH, a.libScroll);
}

void drawQueue(App& a, Ui& ui, const D2D1_RECT_F& r) {
    a.rcQueue = r;
    ui.rect(r, cPanel, 10);
    ui.frameRect(r, cBorder, 10);
    if (!a.queueOpen) { // collapsed: the whole strip is the reopen button
        const bool over = hit(r, ui.in.mx, ui.in.my);
        if (over) ui.rect(r, cHover, 10);
        ui.text(rc(r.left, r.top + 8, r.right - r.left, 24), L"«", 16,
                over ? cText : cDim, 1, true);
        if (!a.queue.empty()) { // count badge
            const float cx2 = (r.left + r.right) / 2;
            ui.rect(rc(cx2 - 11, r.top + 38, 22, 18), cSel, 9);
            ui.text(rc(cx2 - 11, r.top + 38, 22, 18),
                    std::to_wstring(a.queue.size()), 10, cText, 1, true);
        }
        if (ui.in.pressed && hit(r, ui.in.pressX, ui.in.pressY))
            a.queueOpen = true;
        return;
    }
    wchar_t qh[32];
    swprintf(qh, 32, L"QUEUE (%zu)", a.queue.size());
    ui.text(rc(r.left + 14, r.top + 8, // clipped: never collides with buttons
               (std::max)(0.f, (r.right - r.left) - 218 - 22), 26),
            qh, 12, cDim, 0, true);
    if (ui.toggle(305, rc(r.right - 218, r.top + 8, 48, 26), L"",
                  a.repeatOn, cGreen))
        a.repeatOn = !a.repeatOn; // repeat: finished tracks rejoin the tail
    if (ui.button(304, rc(r.right - 164, r.top + 8, 52, 26), L"",
                  cAccent)) {
        static std::mt19937 rng{std::random_device{}()};
        std::shuffle(a.queue.begin(), a.queue.end(), rng);
    }
    if (ui.button(302, rc(r.right - 106, r.top + 8, 56, 26), L"CLEAR", cRed) &&
        !a.queue.empty()) { // mid-gig misclick protection: confirm first
        askConfirm(a, App::ConfirmAction::ClearQueue, L"CLEAR THE QUEUE?",
                   std::to_wstring(a.queue.size()) + L" " +
                       uiTr(L"queued track(s) will be removed."),
                   L"Decks and the rotation are not touched.");
    }
    if (ui.button(406, rc(r.right - 44, r.top + 8, 32, 26), L"»", cAccent))
        a.queueOpen = false;
    // Room for the column header above the list and the running total below.
    const D2D1_RECT_F ql = rc(r.left + 6, r.top + 66, r.right - r.left - 12,
                              (std::max)(0.f, r.bottom - r.top - 92));
    a.rcQueueList = ql;

    // TITLE | ARTIST | TIME across the space the row text occupies (the number
    // and type tag on the left are fixed). Shares, normalised, so the columns
    // follow the drawer as it is resized.
    static const wchar_t* kQName[App::kQueueCols] = {L"TITLE", L"ARTIST", L"TIME"};
    const float qcLeft = ql.left + 80, qcRight = ql.right - 12;
    const float qcw = (std::max)(60.f, qcRight - qcLeft);
    float qfs = 0.f;
    for (float f : a.qColFrac) qfs += f;
    float qx[App::kQueueCols]{}, qcW[App::kQueueCols]{};
    {
        float cx = qcLeft;
        for (int v = 0; v < App::kQueueCols; ++v) {
            qx[v] = cx;
            qcW[v] = qcw * (a.qColFrac[v] / qfs);
            cx += qcW[v];
        }
    }
    {
        const float hy = r.top + 44;
        for (int v = 1; v < App::kQueueCols; ++v) { // draggable dividers
            const D2D1_RECT_F dz = rc(qx[v] - 5, hy, 10, 20);
            const bool overDz = hit(dz, ui.in.mx, ui.in.my);
            if (overDz || a.qColDrag == v) a.hoverResize = true;
            if (ui.in.pressed && hit(dz, ui.in.pressX, ui.in.pressY))
                a.qColDrag = v;
            ui.rect(rc(qx[v] - 4, hy, 1, 18),
                    a.qColDrag == v || overDz ? cAccent : cBorder, 0);
        }
        if (!ui.in.down) a.qColDrag = -1;
        if (a.qColDrag >= 1 && a.qColDrag < App::kQueueCols) {
            // The drag moves width between the two neighbours only, so the
            // set always still adds up and nothing else shifts under it.
            const float pair = a.qColFrac[a.qColDrag - 1] + a.qColFrac[a.qColDrag];
            const float minF = 0.08f * qfs;
            float newL = (ui.in.mx - qx[a.qColDrag - 1]) / qcw * qfs;
            newL = std::clamp(newL, minF, pair - minF);
            a.qColFrac[a.qColDrag - 1] = newL;
            a.qColFrac[a.qColDrag] = pair - newL;
        }
        for (int v = 0; v < App::kQueueCols; ++v)
            ui.text(rc(qx[v] + 8, hy, (std::max)(0.f, qcW[v] - 16), 18),
                    kQName[v], 10, cDim, v == App::kQueueCols - 1 ? 2 : 0, true);
        ui.rect(rc(ql.left, hy + 19, ql.right - ql.left, 1), cBorder, 0);
    }

    ui.clipPush(ql);
    if (hit(ql, ui.in.mx, ui.in.my) && ui.in.wheel != 0)
        a.queueScroll = std::clamp(a.queueScroll - ui.in.wheel * 3, 0.f,
                                   (std::max)(0.f, float(a.queue.size()) -
                                                       (ql.bottom - ql.top) / 26.f));
    for (size_t i = 0; i < a.queue.size(); ++i) {
        const float y = ql.top + (float(i) - a.queueScroll) * 26;
        if (y + 26 < ql.top || y > ql.bottom) continue;
        const D2D1_RECT_F row = rc(ql.left, y, ql.right - ql.left, 24);
        const bool over = hit(row, ui.in.mx, ui.in.my);
        if (int(i) == a.selQueue) ui.rect(row, cSel, 4);
        else if (over) ui.rect(row, cHover, 4);
        else if (i & 1) ui.rect(row, col(0x1A1B1F), 4); // zebra stripe
        if (ui.in.pressed && hit(row, ui.in.pressX, ui.in.pressY) &&
            ui.in.pressX < ql.right - 12) {
            a.selQueue = int(i);
            a.dragArmed = true; // reorder by drag, or drop on a deck to cue
            a.dragX0 = ui.in.pressX;
            a.dragY0 = ui.in.pressY;
            a.dragItem = a.queue[i];
            a.dragFromQueue = int(i);
            a.dragFromList = -1;
        }
        if (ui.in.rpressed && hit(row, ui.in.rX, ui.in.rY)) {
            a.selQueue = int(i);
            a.menu = {MenuReq::QueueRow, int(i), ui.in.rX, ui.in.rY};
        }
        if (ui.in.dblclick && hit(row, ui.in.dblX, ui.in.dblY)) {
            const Match m = a.queue[i];
            a.queue.erase(a.queue.begin() + i);
            playNow(a, m);
            break;
        }
        wchar_t num[8];
        swprintf(num, 8, L"%zu.", i + 1);
        const Match& qm = a.queue[i];
        const bool qPlayed = qm.id && a.playedTonight.count(qm.id);
        if (qPlayed) ui.circle(row.left + 3, y + 12, 3, cRed);
        ui.text(rc(row.left + 8, y, 26, 24), num, 12, cDim, 0, false);
        ui.text(rc(row.left + 34, y, 42, 24), typeTag(qm.type), 10,
                qPlayed ? cDim : typeColor(qm.type), 0, true);
        // Title falls back to the combined label for rows with no tags.
        ui.text(rc(qx[0], y, (std::max)(0.f, qcW[0] - 8), 24),
                qm.title.empty() ? qm.label : qm.title, 13, cText, 0, false);
        if (!qm.artist.empty())
            ui.text(rc(qx[1], y, (std::max)(0.f, qcW[1] - 8), 24), qm.artist, 12,
                    cDim, 0, false);
        if (qm.durMs > 0)
            ui.text(rc(qx[2], y, (std::max)(0.f, qcW[2] - 8), 24),
                    fmtTime(double(qm.durMs) / 1000), 12, cDim, 2, false);
    }
    // Drop-target line while a drag hovers the list.
    if (a.dragging && hit(ql, ui.in.mx, ui.in.my)) {
        const int idx = std::clamp(
            int((ui.in.my - ql.top) / 26 + a.queueScroll + 0.5f), 0,
            int(a.queue.size()));
        const float y = ql.top + (float(idx) - a.queueScroll) * 26 - 1;
        ui.rect(rc(ql.left, y, ql.right - ql.left, 2), cAccent, 1);
    }
    ui.clipPop();
    scrollbar(a, ui, 703, ql, a.queue.size(), 26, a.queueScroll);

    // Running total. ponytail: track length, not cue in/out -- Match does not
    // carry the markers, and the answer the operator wants is "how long until
    // the queue runs dry", which is close enough either way.
    double totalSec = 0;
    bool anyUnknown = false;
    for (const Match& m : a.queue) {
        if (m.durMs > 0) totalSec += double(m.durMs) / 1000;
        else anyUnknown = true;
    }
    const float fy = r.bottom - 25;
    ui.rect(rc(ql.left, fy - 1, ql.right - ql.left, 1), cBorder, 0);
    ui.text(rc(ql.left + 8, fy + 2, 120, 18), L"TOTAL", 10, cDim, 0, true);
    // "~" when something in there has no known length, so the number never
    // claims more precision than it has.
    ui.text(rc(ql.right - 128, fy + 2, 120, 18),
            (anyUnknown && totalSec > 0 ? L"~" : L"") + fmtTime(totalSec), 12,
            a.queue.empty() ? cDim : cText, 2, true);
}
