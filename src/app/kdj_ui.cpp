#include "app/kdj.h"

// ---------------------------------------------------------------- UI drawing

void drawDeck(App& a, Ui& ui, int d, const D2D1_RECT_F& r) {
    a.rcDeck[d] = r;
    const D2D1_COLOR_F ac = d == 0 ? cA : cB;
    const int act = a.mixer.activeDeck.load();
    ui.rect(r, cPanel, 10);
    if (act == d) { // soft glow around the live deck
        ui.frameRect(rc(r.left - 2, r.top - 2, r.right - r.left + 4,
                        r.bottom - r.top + 4),
                     D2D1_COLOR_F{ac.r, ac.g, ac.b, 0.28f}, 12, 3.f);
        ui.frameRect(r, ac, 10, 1.5f);
    } else {
        ui.frameRect(r, cBorder, 10);
    }
    const float x = r.left + 14, w = r.right - r.left - 28;

    // VDJ-style deck header: accent-tinted title bar with deck id/track/state.
    ui.rect(rc(r.left + 1.5f, r.top + 1.5f, r.right - r.left - 3, 48),
            mix(cPanel, ac, a.label[d].empty() ? 0.05f : 0.16f), 9);
    ui.text(rc(x, r.top + 6, w, 14), d == 0 ? L"DECK A" : L"DECK B", 10, ac, 0, true);
    const DeckState st = a.decks[d]->state();
    const bool cued = !a.label[d].empty() && act != d && st == DeckState::Ready &&
                      a.pendingFade != d;
    std::wstring stateTxt = a.label[d].empty() ? L"" :
        st == DeckState::Loading ? L"LOADING" :
        st == DeckState::Error   ? L"ERROR" :
        a.decks[d]->paused.load() ? L"PAUSED" :
        act == d ? L"PLAYING" : cued ? L"CUED — NEXT" : L"READY";
    ui.text(rc(x, r.top + 6, w, 14), stateTxt, 10,
            st == DeckState::Error ? cRed
            : stateTxt == L"PLAYING" ? cGreen
            : cued ? cText : cDim, 2, true);

    ui.text(rc(x, r.top + 22, w, 24),
            a.label[d].empty() ? L"— drop a track —" : a.label[d], 15,
            a.label[d].empty() ? cDim : cText, 0, true);
    std::wstring media = a.hasCdg[d] ? L"MP3+G KARAOKE" : a.hasVid[d] ? L"VIDEO" :
                         a.label[d].empty() ? L"" : L"AUDIO";
    const float ag = a.mixer.autoGain[d].load();
    if (ag != 1.f) { // auto-level trim in effect
        wchar_t agb[32];
        swprintf(agb, 32, L"   ·  auto %+.1f dB", 20.0 * log10(double(ag)));
        media += agb;
    }
    ui.text(rc(x, r.top + 56, w, 18), media, 11, cDim, 0, false);

    const float pk = a.mixer.deckPeak[d].load(std::memory_order_relaxed);
    ui.rect(rc(r.right - 90, r.top + 60, 76, 6), col(0x2A2A30), 3);
    if (pk > 0.001f)
        ui.rect(rc(r.right - 90, r.top + 60, 76 * (std::min)(pk * 1.2f, 1.f), 6),
                pk > 0.9f ? cRed : ac, 3);

    const double played = double(a.decks[d]->framesPlayed.load()) / kRate;
    const double total = double(a.decks[d]->totalFrames.load()) / kRate;
    const float tw = w - 150; // right side belongs to the jog platter
    ui.text(rc(x, r.top + 78, tw / 2, 34), fmtTime(played), 26, cText, 0, true);
    ui.text(rc(x + tw / 2, r.top + 78, tw / 2, 34),
            total > 0 ? L"-" + fmtTime(total - played) : L"", 26, cDim, 2, true);

    // Jog platter (VDJ look): spins with the track, accent ring when live.
    // ponytail: visual only — scrubbing lives on the waveform strip.
    {
        const float jx = r.right - 86, jy = r.top + 146, jr = 56;
        ui.circle(jx, jy, jr, cInset);
        ui.circle(jx, jy, jr, act == d ? ac : cBorder, false, act == d ? 2.f : 1.f);
        ui.circle(jx, jy, jr * 0.42f, col(0x18181C));
        // VDJ signature: crimson center cap on both platters.
        ui.circle(jx, jy, jr * 0.30f,
                  a.label[d].empty() ? col(0x3A2026) : cAccent);
        if (!a.label[d].empty()) {
            const float ang = float(played * 6.2831853 / 1.8) - 1.5707963f;
            const float mx = jx + cosf(ang) * jr * 0.72f;
            const float my = jy + sinf(ang) * jr * 0.72f;
            ui.line(jx + cosf(ang) * jr * 0.46f, jy + sinf(ang) * jr * 0.46f,
                    jx + cosf(ang) * jr * 0.94f, jy + sinf(ang) * jr * 0.94f,
                    col(0x323238), 3);
            ui.circle(mx, my, 4, act == d ? cText : cDim);
        }
    }

    // Waveform strip with click-to-scrub (fills in as the background scan runs).
    const D2D1_RECT_F wf = rc(x, r.top + 124, w - 160, 46); // handles live above
    ui.rect(wf, cInset, 6);
    const float frac = total > 0 ? float(played / total) : 0.f;
    const int readyN = a.wave[d].readyBins();
    const float bw = (wf.right - wf.left) / float(WaveformScanner::kBins);
    const float midY = (wf.top + wf.bottom) / 2;
    for (int i = 0; i < readyN; ++i) {
        float v = a.wave[d].bin(i);
        v = (std::min)(1.f, sqrtf(v) * 1.15f);
        const float bh = (std::max)(1.5f, v * 40.f);
        const bool done = float(i) / WaveformScanner::kBins <= frac;
        D2D1_COLOR_F bc = done ? ac : col(0x3A3A40);
        if (done) bc.a = 0.9f;
        ui.rect(rc(wf.left + i * bw, midY - bh / 2, (std::max)(1.f, bw - 0.5f), bh), bc,
                1);
    }
    a.rcWave[d] = wf;
    // Start/end markers: ALWAYS drawn (defaults: 0 and track end). Smart mode
    // moves them from the scan; drag the handles above the strip to correct.
    if (total > 0) {
        const double durMs = total * 1000.0;
        const double shown[2] = {double(a.cueIn[d]),
                                 a.cueOut[d] > 0 ? double(a.cueOut[d]) : durMs};
        const D2D1_COLOR_F mc[2] = {cGreen, cRed};
        for (int k = 0; k < 2; ++k) {
            const float fx = wf.left + float(shown[k] / durMs) *
                                           (wf.right - wf.left);
            ui.rect(rc(fx - 1, wf.top + 1, 2, wf.bottom - wf.top - 2), mc[k], 1);
            // Grab handle above the strip: markers move ONLY from here, so a
            // stray press on the waveform body can't drag them.
            const D2D1_RECT_F hnd = rc(fx - 6, wf.top - 10, 12, 12);
            ui.rect(hnd, mc[k], 3);
            ui.frameRect(hnd, cText, 3);
            if (ui.in.pressed && hit(hnd, ui.in.pressX, ui.in.pressY) &&
                !a.label[d].empty()) {
                a.markerDeck = d;
                a.markerWhich = k;
            }
        }
        if (a.markerDeck == d) { // dragging a handle
            const int k = a.markerWhich;
            const double ms = std::clamp(
                double((ui.in.mx - wf.left) / (wf.right - wf.left)), 0.0, 1.0) *
                durMs;
            if (k == 0) a.cueIn[d] = int64_t((std::min)(ms, shown[1] - 1000.0));
            else a.cueOut[d] = int64_t(std::clamp(ms, double(a.cueIn[d]) + 1000.0,
                                                  durMs));
            if (a.cueIn[d] < 0) a.cueIn[d] = 0;
            ui.text(rc(ui.in.mx + 8, wf.top - 12, 70, 14),
                    fmtTime((k == 0 ? a.cueIn[d] : a.cueOut[d]) / 1000.0), 11,
                    mc[k], 0, true);
            if (!ui.in.down) { // release: persist (library tracks)
                if (a.deckMatch[d].id) {
                    a.cueFromDb[d] = true; // manual correction wins over smart
                    Db::Stmt q;
                    a.db.prepare(q, "UPDATE media_item SET cue_in_ms=?2, "
                                    "cue_out_ms=?3 WHERE id=?1");
                    const int64_t outSave = // end marker at track end = unset
                        a.cueOut[d] >= int64_t(durMs) - 200 ? 0 : a.cueOut[d];
                    q.bind(1, a.deckMatch[d].id).bind(2, a.cueIn[d]).bind(3, outSave);
                    q.step();
                }
                a.markerDeck = a.markerWhich = -1;
            }
        }
    }
    if (ui.in.rpressed && hit(wf, ui.in.rX, ui.in.rY) && !a.label[d].empty())
        a.menu = {MenuReq::DeckWave, d, ui.in.rX, ui.in.rY};
    if (total > 0)
        ui.rect(rc(wf.left + frac * (wf.right - wf.left) - 1, wf.top + 2, 2,
                   wf.bottom - wf.top - 4),
                cText, 1);
    // Scrub: press to jump, drag to sweep. The top 12 px belong to the marker
    // handles, so a press there never moves the playhead.
    if (ui.in.pressed && hit(wf, ui.in.pressX, ui.in.pressY) &&
        ui.in.pressY > wf.top + 12 && a.markerDeck < 0 && total > 0)
        a.scrubDeck = d;
    if (a.scrubDeck == d) {
        // Ghost playhead tracks the mouse at full frame rate; the actual seek
        // (which briefly blocks) is committed once, on release.
        const float sf =
            std::clamp((ui.in.mx - wf.left) / (wf.right - wf.left), 0.f, 1.f);
        ui.rect(rc(wf.left + sf * (wf.right - wf.left) - 1, wf.top + 2, 2,
                   wf.bottom - wf.top - 4),
                cAccent, 1);
        ui.text(rc(wf.left + sf * (wf.right - wf.left) + 6, wf.top, 60, 16),
                fmtTime(sf * total), 11, cAccent, 0, true);
        if (!ui.in.down) {
            seekFrac(a, d, sf);
            a.scrubDeck = -1;
        }
    }

    const float by = r.bottom - 44;
    if (cued) {
        if (ui.button(110 + d, rc(x, by, 92, 32), L"PLAY ▸", ac, true)) {
            a.pendingFade = d;
            a.pendingDur = act < 0 ? 0.3 : (a.automixOn ? a.fadeSec : 0.08);
        }
    } else {
        const bool paused = a.decks[d]->paused.load();
        if (ui.button(110 + d, rc(x, by, 92, 32), paused ? L"RESUME" : L"PAUSE", ac))
            a.decks[d]->paused.store(!paused);
    }
    // Labeled CLEAR: it empties the deck (and hands over when it was live),
    // it doesn't halt-in-place — that's PAUSE.
    if (ui.button(120 + d, rc(x + 100, by, 72, 32), L"CLEAR", cRed)) stopDeck(a, d);
}

void drawMixer(App& a, Ui& ui, const D2D1_RECT_F& r) {
    ui.rect(r, cPanel, 10);
    ui.frameRect(r, cBorder, 10);
    const float cx = (r.left + r.right) / 2;
    const int act = a.mixer.activeDeck.load();

    // Live per-deck video preview panes (VDJ center): the operator sees both
    // feeds without opening a preview window.
    const float pw = (r.right - r.left - 28 - 4) / 2;
    for (int d = 0; d < 2; ++d) {
        const D2D1_RECT_F pane = rc(r.left + 14 + d * (pw + 4), r.top + 8, pw, 82);
        ui.rect(pane, col(0x000000), 4);
        if (a.cur[d]) { // cur[] resets on load/stop, so no stale-track frames
            if (a.shownSerial[d] != a.frameSerial[d]) {
                ui.setImage(d, a.cur[d]->bgra.data(), a.cur[d]->width,
                            a.cur[d]->height);
                a.shownSerial[d] = a.frameSerial[d];
            }
            ui.clipPush(pane);
            if (!ui.image(d, pane)) { // device loss dropped the cache: re-upload
                ui.setImage(d, a.cur[d]->bgra.data(), a.cur[d]->width,
                            a.cur[d]->height);
                ui.image(d, pane);
            }
            ui.clipPop();
        } else if (!a.label[d].empty() && !a.hasVid[d] && !a.hasCdg[d]) {
            ui.text(pane, L"AUDIO", 10, cDim, 1, true);
        }
        ui.frameRect(pane, act == d ? (d ? cB : cA) : cBorder, 4);
    }

    float gA = a.mixer.deckGain[0].load(), gB = a.mixer.deckGain[1].load();
    if (ui.sliderV(200, rc(cx - 70, r.top + 96, 40, 64), gA, cA))
        a.mixer.deckGain[0].store(gA);
    if (ui.sliderV(201, rc(cx + 30, r.top + 96, 40, 64), gB, cB))
        a.mixer.deckGain[1].store(gB);
    ui.text(rc(cx - 70, r.top + 160, 40, 14), L"A", 11, cA, 1, true);
    ui.text(rc(cx + 30, r.top + 160, 40, 14), L"B", 11, cB, 1, true);

    // Crossfader: deck-colored center fill (A blue left, B red right). During
    // a transition the thumb ANIMATES across at the fade's own pace — display
    // only, so the audio fade gains are never doubled up.
    float xf = a.mixer.crossfader.load();
    const int ftd = a.mixer.fadeTo.load();
    const float ftt = a.mixer.fadeT.load();
    const bool fading = ftd >= 0 && ftt >= 0.f;
    float shown = fading ? (ftd == 1 ? ftt : 1.f - ftt) : xf;
    if (ui.sliderH(202, rc(r.left + 16, r.top + 176, r.right - r.left - 32, 26),
                   shown, cB, true, &cA) &&
        !fading)
        a.mixer.crossfader.store(shown);

    const std::wstring amLabel =
        !a.automixOn ? L"AUTOMIX: CUT"
        : a.mixMode == MixMode::Smart ? L"AUTOMIX: SMART" : L"AUTOMIX: FADE";
    const D2D1_RECT_F amr = rc(r.left + 16, r.bottom - 38, 136, 28);
    if (ui.toggle(203, amr, amLabel, a.automixOn, cGreen)) a.automixOn = !a.automixOn;
    if (ui.in.rpressed && hit(amr, ui.in.rX, ui.in.rY))
        a.menu = {MenuReq::Automix, -1, ui.in.rX, ui.in.rY};

    if (ui.button(204, rc(r.right - 116, r.bottom - 38, 28, 28), L"−", cAccent))
        a.fadeSec = (std::max)(0.5, a.fadeSec - 0.5);
    wchar_t fb[24];
    swprintf(fb, 24, L"%.1fs", a.fadeSec);
    ui.text(rc(r.right - 86, r.bottom - 38, 42, 28), fb, 13, cText, 1, true);
    if (ui.button(205, rc(r.right - 42, r.bottom - 38, 28, 28), L"+", cAccent))
        a.fadeSec = (std::min)(10.0, a.fadeSec + 0.5);
}

// VDJ-style rhythm strip: both decks' waveforms scroll past a fixed center
// "now" line — deck A drawn upward, deck B downward, ±20 s window.
// ponytail: reuses the 400-bin full-track scan, so blocks are chunky on long
// tracks; a zoomed rescan around the playhead is the upgrade if it matters.
void drawRhythm(App& a, Ui& ui, const D2D1_RECT_F& r) {
    ui.rect(r, cInset, 8); // black well, VDJ-style
    ui.frameRect(r, cBorder, 8);
    const float midY = (r.top + r.bottom) / 2;
    ui.line(r.left + 6, midY, r.right - 6, midY, col(0x1B1B1F), 1);
    constexpr double win = 20.0; // seconds visible on each side of "now"
    const float cx = (r.left + r.right) / 2;
    const float pxPerSec = (cx - r.left - 6) / float(win);
    ui.clipPush(rc(r.left + 4, r.top + 3, r.right - r.left - 8, r.bottom - r.top - 6));
    for (int d = 0; d < 2; ++d) {
        const uint64_t total = a.decks[d]->totalFrames.load();
        const int readyN = a.wave[d].readyBins();
        if (!total || !readyN) continue;
        const double dur = double(total) / kRate;
        const double pos = double(a.decks[d]->framesPlayed.load()) / kRate;
        const D2D1_COLOR_F ac = d == 0 ? cA : cB;
        const float half = midY - r.top - 8;
        const double binSec = dur / WaveformScanner::kBins;
        const float bw = (std::max)(1.f, float(binSec * pxPerSec) - 0.5f);
        for (int i = 0; i < readyN; ++i) {
            const double t = (i + 0.5) * binSec;
            const float x = cx + float((t - pos) * pxPerSec) - bw / 2;
            if (x + bw < r.left + 4 || x > r.right - 4) continue;
            const float v = (std::min)(1.f, sqrtf(a.wave[d].bin(i)) * 1.15f);
            const float bh = (std::max)(1.5f, v * half);
            const D2D1_COLOR_F c = t <= pos ? ac : mix(ac, col(0x3A3A40), 0.6f);
            ui.rect(d == 0 ? rc(x, midY - bh, bw, bh) : rc(x, midY + 1, bw, bh), c,
                    0.5f);
        }
    }
    ui.clipPop();
    ui.rect(rc(cx - 1, r.top + 4, 2, r.bottom - r.top - 8), cText, 1); // now line
}

// Immediate children of dir among folders that contain media.
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
            (row.mode != NavMode::Folder || row.folder == a.navFolder);
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
                a.focus = Focus::None;
                a.searchDirty = true;
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
void cleanMissingFiles(App& a) {
    std::vector<int64_t> gone;
    {
        Db::Stmt q;
        a.db.prepare(q, "SELECT id, path FROM media_item");
        while (q.step()) {
            const std::wstring p = wide(q.colText(1));
            if (p.size() >= 3 && p[1] == L':' &&
                GetFileAttributesW(p.substr(0, 3).c_str()) == INVALID_FILE_ATTRIBUTES)
                continue; // whole drive absent: skip
            if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES)
                gone.push_back(q.colInt(0));
        }
    }
    if (gone.empty()) {
        a.status = L"no missing files in the library";
        return;
    }
    a.confirmIds = std::move(gone);
    askConfirm(a, App::ConfirmAction::CleanMissing, L"CLEAN MISSING FILES",
               std::to_wstring(a.confirmIds.size()) +
                   L" entries point at files that no longer exist.",
               L"Their playlist / rotation entries go too. Files on disk are "
               L"not touched.");
}

void performCleanMissing(App& a) {
    const std::vector<int64_t> gone = std::move(a.confirmIds);
    a.confirmIds.clear();
    a.db.exec("BEGIN");
    for (const int64_t id : gone) {
        Db::Stmt d1;
        a.db.prepare(d1, "DELETE FROM playlist_item WHERE media_id=?1");
        d1.bind(1, id);
        d1.step();
        Db::Stmt d2;
        a.db.prepare(d2, "DELETE FROM singer_queue_item WHERE media_id=?1");
        d2.bind(1, id);
        d2.step();
        Db::Stmt d3;
        a.db.prepare(d3, "DELETE FROM media_item WHERE id=?1");
        d3.bind(1, id);
        d3.step();
    }
    a.db.exec("COMMIT");
    a.navDirty = a.searchDirty = true;
    a.status = L"removed " + std::to_wstring(gone.size()) + L" missing entries";
}

// Text-box caret blink (only while a box is focused).
static bool caretOn() { return (GetTickCount64() / 530) & 1; }

void drawSettings(App& a, Ui& ui, const D2D1_RECT_F& r) {
    // The section list has outgrown small screens: the wheel scrolls it.
    static float scroll = 0, contentH = 1100;
    scroll = std::clamp(scroll - ui.in.wheel * 48.f, 0.f,
                        (std::max)(0.f, contentH - (r.bottom - r.top)));
    const float x = r.left + 20;
    const float w = (std::min)(620.f, r.right - r.left - 40);
    float y = r.top + 14 - scroll;
    if (ui.in.pressed) a.setFocusBox = 0; // boxes re-claim when hit

    ui.text(rc(x, y, w, 16), L"BEHAVIOUR", 11, cDim, 0, true);
    y += 24;
    if (ui.toggle(603, rc(x, y, 300, 28), L"AUTO GAIN (LEVEL TRACKS)", a.autoGainOn,
                  cGreen)) {
        a.autoGainOn = !a.autoGainOn;
        for (int d = 0; d < 2; ++d) a.gainSet[d] = false; // re-apply live decks
        if (!a.autoGainOn)
            for (int d = 0; d < 2; ++d) a.mixer.autoGain[d].store(1.f);
    }
    ui.text(rc(x + 310, y, w - 310, 28),
            L"trims every track toward the same loudness", 10, cDim, 0, false);
    y += 40;
    if (!a.updLatest.empty()) { // newer release on GitHub
        if (ui.button(604, rc(x, y, 300, 28),
                      L"GET UPDATE  v" + a.updLatest, cGreen, true))
            ShellExecuteW(nullptr, L"open",
                          L"https://github.com/MrBeanTheOne/KaraokeDJ/"
                          L"releases/latest",
                          nullptr, nullptr, SW_SHOWNORMAL);
        ui.text(rc(x + 310, y, w - 310, 28),
                L"opens the download page — this is v" KDJ_VERSION_W, 10,
                cDim, 0, false);
    } else {
        if (ui.button(604, rc(x, y, 300, 28), L"CHECK FOR UPDATES", cDim))
            startUpdateCheck(a, true);
        ui.text(rc(x + 310, y, w - 310, 28),
                a.updBusy.load() ? L"checking…"
                                 : L"this is v" KDJ_VERSION_W, 10, cDim, 0,
                false);
    }
    y += 44;

    ui.text(rc(x, y, w, 16), L"VIDEO OUTPUT", 11, cDim, 0, true);
    y += 24;
    auto monRow = [&](const std::wstring& label, int mon) {
        const D2D1_RECT_F rr = rc(x, y, w, 26);
        const bool cur = a.outMonitor == mon;
        if (cur) ui.rect(rr, cSel, 5);
        else if (hit(rr, ui.in.mx, ui.in.my)) ui.rect(rr, cHover, 5);
        ui.circle(x + 13, y + 13, 6, cur ? cGreen : cBorder, cur, 1.5f);
        if (!cur) ui.circle(x + 13, y + 13, 6, cBorder, false, 1.5f);
        ui.text(rc(x + 28, y, w - 34, 26), label, 12, cur ? cText : cDim, 0, cur);
        if (ui.in.pressed && hit(rr, ui.in.pressX, ui.in.pressY) && !cur) {
            a.fullOut.reset();
            a.outMonitor = -1;
            if (mon >= 0) {
                a.fullOut = std::make_unique<VideoWindow>();
                if (a.fullOut->create(mon)) {
                    a.outMonitor = a.chosenMonitor = mon;
                    setSetting(a.db, "video_monitor", std::to_string(mon));
                    for (int d = 0; d < 2; ++d) // fresh window needs frames
                        if (a.cur[d]) a.fullOut->setFrame(d, *a.cur[d]);
                } else {
                    a.fullOut.reset();
                }
            }
            a.status = mon >= 0 ? L"video output: monitor " + std::to_wstring(mon)
                                : L"video output off";
        }
        y += 28;
    };
    monRow(L"Off  (no fullscreen output)", -1);
    for (int i = 0; i < VideoWindow::monitorCount(); ++i) {
        const RECT mr2 = VideoWindow::monitorRect(i);
        wchar_t ml[80];
        swprintf(ml, 80, L"Monitor %d  (%ldx%ld)%ls", i, mr2.right - mr2.left,
                 mr2.bottom - mr2.top, i == 0 ? L" — main display" : L"");
        monRow(ml, i);
    }
    y += 6;
    ui.text(rc(x, y, 90, 26), L"Video fit", 12, cText, 0, false);
    { // fit / fill / stretch — applied live to the output window
        static const wchar_t* fitNames[3] = {L"FIT", L"FILL", L"STRETCH"};
        static const wchar_t* fitHints[3] = {
            L"keep ratio, black bars", L"keep ratio, crop to fill",
            L"ignore ratio, fill screen"};
        float fx = x + 100;
        for (int m = 0; m < 3; ++m) {
            if (ui.toggle(610 + m, rc(fx, y, 92, 26), fitNames[m],
                          a.videoFit == m, cGreen) &&
                a.videoFit != m) {
                a.videoFit = m;
                setSetting(a.db, "video_fit", std::to_string(m));
            }
            fx += 98;
        }
        ui.text(rc(fx + 6, y, w - (fx - x) - 6, 26), fitHints[a.videoFit], 10,
                cDim, 0, false);
    }
    y += 40;

    ui.text(rc(x, y, w, 16), L"AUDIO OUTPUT", 11, cDim, 0, true);
    y += 24;
    auto devRow = [&](const std::wstring& label, const std::string& idU8) {
        const D2D1_RECT_F rr = rc(x, y, w, 26);
        const bool cur = a.audioDevice == idU8;
        if (cur) ui.rect(rr, cSel, 5);
        else if (hit(rr, ui.in.mx, ui.in.my)) ui.rect(rr, cHover, 5);
        ui.circle(x + 13, y + 13, 6, cur ? cGreen : cBorder, cur, 1.5f);
        if (!cur) ui.circle(x + 13, y + 13, 6, cBorder, false, 1.5f);
        ui.text(rc(x + 28, y, w - 34, 26), label, 12, cur ? cText : cDim, 0, cur);
        if (ui.in.pressed && hit(rr, ui.in.pressX, ui.in.pressY) && !cur) {
            a.audioDevice = idU8;
            setSetting(a.db, "audio_device", idU8);
            // Reopen on the new endpoint; decks keep decoding, the ring
            // carries straight on after the brief re-init.
            a.out.start(kRate, kCh, a.mixer, wide(idU8));
            a.status = L"audio output: " + label;
        }
        y += 28;
    };
    devRow(L"System default  (follows Windows)", "");
    for (const auto& [id, name] : a.audioDevs) devRow(name, utf8(id));
    y += 14;

    ui.text(rc(x, y, w, 16), L"LIBRARY SCAN", 11, cDim, 0, true);
    y += 24;
    if (ui.toggle(601, rc(x, y, 300, 28), L"READ FILE TAGS ON IMPORT", a.scanTags,
                  cGreen))
        a.scanTags = !a.scanTags;
    ui.text(rc(x + 310, y, w - 310, 28),
            a.scanTags ? L"slower — real titles, artists, durations"
                       : L"fastest — filenames only; tags fill in on a later import",
            10, cDim, 0, false);
    y += 40;
    if (ui.button(605, rc(x, y, 220, 28), L"UPDATE ALL FOLDERS", cAccent))
        rescanAll(a);
    ui.text(rc(x + 230, y, w - 230, 28),
            L"rescan every imported folder — unchanged files skip fast", 10,
            cDim, 0, false);
    y += 40;
    if (ui.toggle(606, rc(x, y, 300, 28), L"WATCH FOLDERS (AUTO-UPDATE)",
                  a.watchOn, cGreen)) {
        a.watchOn = !a.watchOn;
        setSetting(a.db, "watch_folders", a.watchOn ? "1" : "0");
        startWatcher(a); // start or stop immediately
        a.status = a.watchOn ? L"watching library folders for changes"
                             : L"folder watching off";
    }
    ui.text(rc(x + 310, y, w - 310, 28),
            L"new/changed files import themselves shortly after they appear",
            10, cDim, 0, false);
    y += 40;
    if (ui.button(602, rc(x, y, 220, 28), L"CLEAN MISSING FILES", cRed))
        a.menu = {MenuReq::CleanMissing, -1, 0, 0}; // confirm runs after the frame
    ui.text(rc(x + 230, y, w - 230, 28),
            L"drop entries whose file was deleted (unplugged drives are left alone)",
            10, cDim, 0, false);
    y += 44;

    ui.text(rc(x, y, w, 16), L"PHONE REQUESTS", 11, cDim, 0, true);
    y += 24;
    if (ui.toggle(620, rc(x, y, 300, 28), L"ALLOW PHONE REQUESTS", a.webOn,
                  cGreen)) {
        if (a.webOn) { // turning it off tears everything down immediately
            a.web.stop();
            a.webOn = false;
            if (a.setFocusBox == 2) a.setFocusBox = 0;
            a.status = L"phone requests off";
        } else if (a.web.setPassword(a.webPass), a.web.start(a.dbPath)) {
            a.webOn = true;
            a.status = L"phone requests on";
        } else {
            a.status = L"phone requests: server failed to start";
        }
        setSetting(a.db, "web_on", a.webOn ? "1" : "0");
    }
    ui.text(rc(x + 310, y, w - 310, 28),
            L"singers search and request from their phones (same Wi-Fi / hotspot)",
            10, cDim, 0, false);
    y += 34;
    if (a.webOn) { // optional page password, applied live as it's typed
        ui.text(rc(x, y, 190, 26), L"Password (optional)", 12, cText, 0, false);
        const D2D1_RECT_F pbox = rc(x + 200, y, 240, 26);
        ui.rect(pbox, cInset, 7);
        ui.frameRect(pbox, a.setFocusBox == 2 ? cAccent : cBorder, 5);
        ui.text(rc(pbox.left + 8, pbox.top, 224, 26),
                a.webPass + (a.setFocusBox == 2 && caretOn() ? L"▏" : L""),
                12, cText, 0, false);
        if (ui.in.pressed && hit(pbox, ui.in.pressX, ui.in.pressY))
            a.setFocusBox = 2;
        ui.text(rc(pbox.right + 10, y, w - 200 - 250, 26),
                L"phones enter it once; blank = open", 10, cDim, 0, false);
        y += 34;
    }
    if (a.webOn && a.web.running()) {
        // The LAN address walks the adapter list — cache it a few seconds.
        static std::wstring cachedUrl;
        static ULONGLONG urlAt = 0;
        if (GetTickCount64() - urlAt > 3000 || urlAt == 0) {
            cachedUrl = a.web.url();
            urlAt = GetTickCount64();
        }
        ui.text(rc(x, y, w, 18),
                cachedUrl.empty()
                    ? L"No network found — join Wi-Fi or start a mobile hotspot"
                    : L"Phones open:  " + cachedUrl +
                          L"   (QR shows on the video output between songs)",
                11, cachedUrl.empty() ? cRed : cText, 0, false);
        y += 24;
    }
    y += 20;

    { // collapsible: a gig-night settings page stays one screen tall
        const D2D1_RECT_F hd = rc(x, y, w, 18);
        ui.text(hd,
                a.idleEditorOpen ? L"WAITING SCREEN  ▾"
                                 : L"WAITING SCREEN  ▸   (click to design)",
                11, a.idleEditorOpen ? cAccent : cDim, 0, true);
        if (ui.in.pressed && hit(hd, ui.in.pressX, ui.in.pressY))
            a.idleEditorOpen = !a.idleEditorOpen;
    }
    y += 24;
    if (!a.idleEditorOpen) {
        contentH = (y + scroll) - r.top + 16;
        return;
    }
    const auto textRow = [&](const wchar_t* label, std::wstring& val,
                             int focusId, const wchar_t* hint) {
        ui.text(rc(x, y, 190, 26), label, 12, cText, 0, false);
        const D2D1_RECT_F tb = rc(x + 200, y, w - 200, 26);
        ui.rect(tb, cInset, 7);
        ui.frameRect(tb, a.setFocusBox == focusId ? cAccent : cBorder, 5);
        ui.text(rc(tb.left + 8, tb.top, tb.right - tb.left - 12, 26),
                val + (a.setFocusBox == focusId && caretOn() ? L"▏" : L""),
                12, cText, 0, false);
        if (ui.in.pressed && hit(tb, ui.in.pressX, ui.in.pressY))
            a.setFocusBox = focusId;
        ui.text(rc(x + 200, y + 27, w - 200, 14), hint, 10, cDim, 0, false);
        y += 50;
    };
    textRow(L"Title", a.idleTitle, 1, L"the big headline between songs");
    textRow(L"Message", a.idleSub, 3,
            L"free text — venue name, drink specials, anything");
    ui.text(rc(x, y, 190, 26), L"Logo", 12, cText, 0, false);
    if (ui.button(630, rc(x + 200, y, 130, 26), L"PICK IMAGE…", cAccent))
        a.menu = {MenuReq::None, -3}; // STA picker runs after this frame
    if (a.idleLogoLoaded) {
        ui.text(rc(x + 340, y, w - 384, 26), leafName(a.idleLogoPath), 11, cDim,
                0, false);
        if (ui.button(631, rc(x + w - 34, y, 30, 26), L"✕", cRed)) {
            a.idleLogoPath.clear();
            a.idleLogoLoaded = false;
            setSetting(a.db, "idle_logo", "");
        }
    } else {
        ui.text(rc(x + 340, y, w - 344, 26),
                L"none — png/jpg, transparency kept", 10, cDim, 0, false);
    }
    y += 34;
    ui.text(rc(x, y, 190, 26), L"Background", 12, cText, 0, false);
    if (ui.button(632, rc(x + 200, y, 130, 26), L"PICK IMAGE…", cAccent))
        a.menu = {MenuReq::None, -4}; // STA picker runs after this frame
    if (a.idleBgLoaded) {
        ui.text(rc(x + 340, y, w - 384, 26), leafName(a.idleBgPath), 11, cDim,
                0, false);
        if (ui.button(633, rc(x + w - 34, y, 30, 26), L"✕", cRed)) {
            a.idleBgPath.clear();
            a.idleBgLoaded = false;
            setSetting(a.db, "idle_bg", "");
        }
    } else {
        ui.text(rc(x + 340, y, w - 344, 26),
                L"none — fills the screen, dimmed so text stays readable", 10,
                cDim, 0, false);
    }
    y += 38;
    ui.text(rc(x, y, 300, 14),
            L"show · position (3×3) · size", 10, cDim, 0, false);
    y += 22;
    const float elemTop = y;
    static const wchar_t* kElemNames[6] = {L"Logo",    L"Title",
                                           L"Message", L"Next up",
                                           L"Singer list", L"QR code"};
    static const wchar_t* kSizeNames[3] = {L"S", L"M", L"L"};
    for (int k = 0; k < 6; ++k) {
        IdleElem& e = a.idleElems[k];
        ui.text(rc(x, y + 8, 106, 26), kElemNames[k], 12, cText, 0, false);
        if (ui.toggle(640 + k, rc(x + 112, y + 6, 76, 26),
                      e.on ? L"SHOWN" : L"HIDDEN", e.on, cGreen))
            e.on = !e.on;
        for (int c9 = 0; c9 < 9; ++c9) { // anchor grid
            const D2D1_RECT_F cell =
                rc(x + 206 + float(c9 % 3) * 15.f, y + float(c9 / 3) * 13.f,
                   13, 11);
            if (ui.in.pressed && hit(cell, ui.in.pressX, ui.in.pressY))
                e.pos = c9;
            ui.rect(cell, e.pos == c9 ? cGreen : col(0x2E2E34), 2);
        }
        if (ui.button(650 + k, rc(x + 262, y + 6, 34, 26), kSizeNames[e.size],
                      cAccent))
            e.size = (e.size + 1) % 3;
        y += 44;
    }

    { // Live preview in the space right of the rows: same proportions as
      // drawIdle, scaled to a 16:9 card (everything there is % of height,
      // which is also why bar TVs at 720p/1080p render the same layout).
        const float pw2 = w - 310;
        const float ph2 = pw2 * 9.f / 16.f;
        const D2D1_RECT_F P = rc(x + 310, elemTop, pw2, ph2);
        ui.rect(P, col(0x000000), 4);
        // slots on the settings Ui: 0 = logo, 1 = background
        static std::wstring upLogo, upBg; // last uploaded (per settings Ui)
        if (a.idleBgLoaded) {
            if (upBg != a.idleBgPath || !ui.image(1, P)) {
                ui.setImage(1, a.idleBg.bgra.data(), a.idleBg.width,
                            a.idleBg.height);
                upBg = a.idleBgPath;
                ui.image(1, P);
            }
            ui.rect(P, col(0x000000, 0.45f), 4);
        }
        const float H = ph2, MG = H * 0.05f;
        const auto mul = [](int sz) {
            return sz == 0 ? 0.65f : sz == 2 ? 1.5f : 1.f;
        };
        const auto rowY = [&](int pos, float hgt) {
            const int rr = pos / 3;
            return rr == 0   ? P.top + MG
                   : rr == 2 ? P.bottom - MG - hgt
                             : (P.top + P.bottom - hgt) / 2;
        };
        const auto pText = [&](const std::wstring& t, const IdleElem& e,
                               float base, D2D1_COLOR_F c2, bool bold) {
            if (!e.on || t.empty()) return;
            const float sz = (std::max)(6.f, H * base * mul(e.size));
            ui.text(rc(P.left + MG, rowY(e.pos, sz * 1.3f), pw2 - 2 * MG,
                       sz * 1.3f),
                    t, sz, c2, e.pos % 3 == 0 ? 0 : e.pos % 3 == 2 ? 2 : 1,
                    bold);
        };
        const IdleElem& lg = a.idleElems[0];
        if (lg.on && a.idleLogoLoaded && a.idleLogo.height) {
            float bh2 = H * 0.22f * mul(lg.size);
            float bw3 = bh2 * float(a.idleLogo.width) / float(a.idleLogo.height);
            const float mx = pw2 * 0.6f;
            if (bw3 > mx) { bh2 *= mx / bw3; bw3 = mx; }
            const int c2 = lg.pos % 3;
            const float lx = c2 == 0   ? P.left + MG
                             : c2 == 2 ? P.right - MG - bw3
                                       : (P.left + P.right - bw3) / 2;
            const D2D1_RECT_F lr = rc(lx, rowY(lg.pos, bh2), bw3, bh2);
            if (upLogo != a.idleLogoPath || !ui.image(0, lr)) {
                ui.setImage(0, a.idleLogo.bgra.data(), a.idleLogo.width,
                            a.idleLogo.height);
                upLogo = a.idleLogoPath;
                ui.image(0, lr);
            }
        }
        pText(a.idleTitle, a.idleElems[1], 0.075f, cText, true);
        pText(a.idleSub, a.idleElems[2], 0.038f, col(0xC9CDD3), false);
        pText(L"NEXT UP:  …", a.idleElems[3], 0.034f, col(0x4FC3F7), false);
        if (a.idleElems[4].on) { // singer list: a few stand-in lines
            const IdleElem& e = a.idleElems[4];
            const float sz = (std::max)(6.f, H * 0.030f * mul(e.size));
            const std::wstring lines[3] = {
                L"UP NEXT",
                a.idleSingerLines.size() > 0 ? a.idleSingerLines[0]
                                             : L"1.  —",
                a.idleSingerLines.size() > 1 ? a.idleSingerLines[1]
                                             : L"2.  —"};
            float ly = rowY(e.pos, sz * 4.f);
            for (const std::wstring& ln : lines) {
                ui.text(rc(P.left + MG, ly, pw2 - 2 * MG, sz * 1.25f), ln, sz,
                        cText, e.pos % 3 == 0 ? 0 : e.pos % 3 == 2 ? 2 : 1,
                        false);
                ly += sz * 1.3f;
            }
        }
        const IdleElem& eq = a.idleElems[5];
        if (eq.on && a.webOn) { // stylized QR stand-in
            const float qs = H * 0.26f * mul(eq.size);
            const int c2 = eq.pos % 3;
            const float qx = c2 == 0   ? P.left + MG
                             : c2 == 2 ? P.right - MG - qs
                                       : (P.left + P.right - qs) / 2;
            const float qy = rowY(eq.pos, qs + 10);
            ui.rect(rc(qx, qy, qs, qs), col(0xFFFFFF), 3);
            const float fp = qs * 0.28f;
            ui.rect(rc(qx + qs * 0.10f, qy + qs * 0.10f, fp, fp), col(0x000000), 1);
            ui.rect(rc(qx + qs * 0.62f, qy + qs * 0.10f, fp, fp), col(0x000000), 1);
            ui.rect(rc(qx + qs * 0.10f, qy + qs * 0.62f, fp, fp), col(0x000000), 1);
            ui.rect(rc(qx + qs * 0.45f, qy + qs * 0.45f, fp * 0.6f, fp * 0.6f),
                    col(0x000000), 1);
        }
        ui.frameRect(P, cBorder, 4);
        ui.text(rc(P.left, P.bottom + 4, pw2, 14), L"PREVIEW", 9, cDim, 1, true);
    }
    contentH = (y + scroll) - r.top + 16; // for next frame's scroll clamp
}

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
    // 2 GENRE 3 YEAR 4 BPM 5 TIME (sort ids map via kColSort).
    static const wchar_t* kColName[6] = {L"TITLE", L"ARTIST", L"GENRE",
                                         L"YEAR",  L"BPM",    L"TIME"};
    static const int kColSort[6] = {1, 0, 2, 3, 4, 5};
    const float listLeft = r.left + 6, listRight = r.right - r.left - 12 + r.left + 6;
    const float cw = (listRight - listLeft) - 66 - 8;
    int visIds[6];
    int nVis = 0;
    float fracSum = 0.f;
    for (int k = 0; k < 6; ++k) {
        const int id = a.colSeq[k];
        if (!a.colShow[id]) continue;
        visIds[nVis++] = id;
        fracSum += a.colFrac[id];
    }
    float colX[6]{}, colW[6]{};
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
            ui.text(hr, t, 10, active ? cAccent : cDim, id >= 4 ? 2 : 0, true);
            if (a.nav != NavMode::Playlist && a.colDrag < 0 && ui.in.pressed &&
                hit(hr, ui.in.pressX, ui.in.pressY)) {
                if (a.sortCol == kColSort[id]) a.sortAsc = !a.sortAsc;
                else { a.sortCol = kColSort[id]; a.sortAsc = true; }
                a.searchDirty = true;
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
                    a.selRows.clear();          // collapses it; inside keeps it
                    a.selRows.insert(int(i));   // so the group can be dragged
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
                        a.queue.push_back(a.results[j]);
                }
            }
            const bool played = m.id && a.playedTonight.count(m.id);
            if (played) ui.circle(row.left + 3, y + 12, 3, cRed);
            ui.text(rc(row.left + 8, y, 50, rowH), typeTag(m.type), 11,
                    played ? cDim : typeColor(m.type), 0, true);
            for (int v = 0; v < nVis; ++v) {
                const int id = visIds[v];
                const D2D1_RECT_F cell =
                    rc(colX[v], y, colW[v] - (id >= 4 ? 18.f : 8.f), rowH);
                switch (id) {
                case 0:
                    ui.text(cell, m.title.empty() ? m.label : m.title, 13, cText,
                            0, false);
                    break;
                case 1: ui.text(cell, m.artist, 13, cDim, 0, false); break;
                case 2: ui.text(cell, m.genre, 12, cDim, 0, false); break;
                case 3:
                    ui.text(cell, m.year > 0 ? std::to_wstring(m.year) : L"", 12,
                            cDim, 0, false);
                    break;
                case 4:
                    ui.text(cell, m.bpm > 0 ? std::to_wstring(m.bpm) : L"", 12,
                            cDim, 2, false);
                    break;
                case 5:
                    ui.text(cell, fmtTime(double(m.durMs) / 1000), 12, cDim, 2,
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
    if (ui.toggle(305, rc(r.right - 218, r.top + 8, 48, 26), L"RPT", a.repeatOn,
                  cGreen))
        a.repeatOn = !a.repeatOn; // repeat: finished tracks rejoin the tail
    if (ui.button(304, rc(r.right - 164, r.top + 8, 52, 26), L"SHUF", cAccent)) {
        static std::mt19937 rng{std::random_device{}()};
        std::shuffle(a.queue.begin(), a.queue.end(), rng);
    }
    if (ui.button(302, rc(r.right - 106, r.top + 8, 56, 26), L"CLEAR", cRed)) {
        a.queue.clear();
        a.selQueue = -1;
    }
    if (ui.button(406, rc(r.right - 44, r.top + 8, 32, 26), L"»", cAccent))
        a.queueOpen = false;
    const D2D1_RECT_F ql = rc(r.left + 6, r.top + 42, r.right - r.left - 12,
                              r.bottom - r.top - 48);
    a.rcQueueList = ql;
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
        ui.text(rc(row.left + 80, y, row.right - row.left - 86, 24), qm.label, 13,
                cText, 0, false);
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
}

void drawUi(App& a, Ui& ui, float W, float H) {
    const UiInput savedIn = ui.in;
    const bool promptWasOpen = a.prompt != App::Prompt::None;
    if (promptWasOpen) { // modal: the UI below sees no pointer
        ui.in.pressed = ui.in.released = ui.in.dblclick = ui.in.rpressed = false;
        ui.in.down = false;
        ui.in.mx = ui.in.my = -10000;
    }
    if (ui.in.pressed) a.focus = Focus::None; // boxes re-claim when hit
    ui.text(rc(16, 8, 300, 32), L"KARAOKE DJ", 20, cAccent, 0, true);
    wchar_t perf[96];
    if (a.bpmBusy.load() && a.bpmTotal.load() > 0)
        swprintf(perf, 96, L"CPU %.1f%%   RAM %d MB   ANALYZING BPM %d / %d",
                 a.cpuPct, a.ramMb, a.bpmDone.load(), a.bpmTotal.load());
    else
        swprintf(perf, 96, L"CPU %.1f%%   RAM %d MB", a.cpuPct, a.ramMb);
    ui.text(rc(16, 40, 420, 14), perf, 10, cDim, 0, false);
    ui.text(rc(16, 8, W - 32, 32), a.status, 12, cDim, 2, false);

    std::wstring vlabel = a.outMonitor < 0
                              ? L"VIDEO OUT: OFF"
                              : L"VIDEO OUT: MON " + std::to_wstring(a.outMonitor);
    if (a.webOn || !a.reqInbox.empty()) { // phone-request inbox badge
        wchar_t rl[32];
        swprintf(rl, 32, L"REQUESTS (%d)", int(a.reqInbox.size()));
        if (ui.toggle(402, rc(W - 446, 44, 132, 30), rl, !a.reqInbox.empty(),
                      cGreen))
            a.prompt = App::Prompt::Requests;
    }
    // SETTINGS opens its own window (main loop services the request).
    if (ui.toggle(401, rc(W - 306, 44, 110, 30), L"SETTINGS",
                  a.settingsWnd != nullptr, cAccent))
        a.settingsOpenReq = true;
    const D2D1_RECT_F vr = rc(W - 186, 44, 170, 30);
    if (ui.toggle(400, vr, vlabel, a.outMonitor >= 0, cGreen)) {
        if (a.outMonitor >= 0) {
            a.fullOut.reset();
            a.outMonitor = -1;
        } else {
            const int n = VideoWindow::monitorCount();
            const int m = a.chosenMonitor >= 0 && a.chosenMonitor < n ? a.chosenMonitor
                          : n > 1 ? n - 1 : 0;
            a.fullOut = std::make_unique<VideoWindow>();
            if (a.fullOut->create(m)) a.outMonitor = a.chosenMonitor = m;
            else a.fullOut.reset();
        }
    }
    if (ui.in.rpressed && hit(vr, ui.in.rX, ui.in.rY))
        a.menu = {MenuReq::VideoOut, -1, ui.in.rX, ui.in.rY};


    drawRhythm(a, ui, rc(16, 84, W - 32, 56));

    const float mixW = 330, top = 148, deckH = 252;
    const float deckW = (W - mixW - 48) / 2;
    drawDeck(a, ui, 0, rc(16, top, deckW, deckH));
    drawMixer(a, ui, rc(16 + deckW + 8, top, mixW, deckH));
    drawDeck(a, ui, 1, rc(16 + deckW + mixW + 16, top, deckW, deckH));

    const float by = top + deckH + 10, bh = H - by - 12;
    a.hoverResize = false; // rebuilt every frame by each divider below
    if (a.queueOpen) { // queue divider drag (resize) before layout uses it
        const D2D1_RECT_F qdiv = rc(W - a.queueW - 16 - 9, by, 12, bh);
        if (hit(qdiv, ui.in.mx, ui.in.my) || a.resizingQueue) a.hoverResize = true;
        if (ui.in.pressed && hit(qdiv, ui.in.pressX, ui.in.pressY))
            a.resizingQueue = true;
        if (!ui.in.down) a.resizingQueue = false;
        if (a.resizingQueue)
            a.queueW = std::clamp(W - 16.f - ui.in.mx, 300.f,
                                  (std::min)(560.f, W * 0.4f));
    }
    const float qw = a.queueOpen ? a.queueW : 30.f;
    // Sidebar divider drag (resize) before layout uses the width.
    if (a.sidebarOpen) {
        const D2D1_RECT_F divider = rc(16 + a.sideW - 3, by, 12, bh);
        if (hit(divider, ui.in.mx, ui.in.my) || a.resizingSide) a.hoverResize = true;
        if (ui.in.pressed && hit(divider, ui.in.pressX, ui.in.pressY))
            a.resizingSide = true;
        if (!ui.in.down) a.resizingSide = false;
        if (a.resizingSide)
            a.sideW = std::clamp(ui.in.mx - 16.f, 160.f, (std::min)(420.f, W * 0.4f));
    }
    const float sw = a.sidebarOpen ? a.sideW : 30.f;
    drawSidebar(a, ui, rc(16, by, sw, bh));
    if (a.sidebarOpen)
        ui.rect(rc(16 + sw + 2, by + 8, 3, bh - 16),
                a.resizingSide || hit(rc(16 + a.sideW - 3, by, 12, bh), ui.in.mx,
                                      ui.in.my)
                    ? cAccent
                    : cBorder,
                1);
    drawBrowser(a, ui, rc(16 + sw + 8, by, W - sw - qw - 48, bh));
    if (a.queueOpen)
        ui.rect(rc(W - qw - 16 - 6, by + 8, 3, bh - 16),
                a.resizingQueue || hit(rc(W - a.queueW - 16 - 9, by, 12, bh),
                                       ui.in.mx, ui.in.my)
                    ? cAccent
                    : cBorder,
                1);
    drawQueue(a, ui, rc(W - qw - 16, by, qw, bh));

    // Internal drag: promote to dragging after 8 px, drop on release.
    if (a.dragArmed && ui.in.down &&
        (fabsf(ui.in.mx - a.dragX0) > 8 || fabsf(ui.in.my - a.dragY0) > 8))
        a.dragging = true;
    if (a.dragging) {
        const D2D1_RECT_F chip = rc(ui.in.mx + 12, ui.in.my + 8, 260, 24);
        ui.rect(chip, col(0x2A2A31, 0.95f), 5);
        ui.frameRect(chip, cAccent, 5);
        const std::wstring lbl =
            a.dragItems.size() > 1
                ? std::to_wstring(a.dragItems.size()) + L" tracks"
                : a.dragItem.label;
        ui.text(rc(chip.left + 8, chip.top, 244, 24), lbl, 12, cText, 0, false);
    }
    if (ui.in.released) {
        if (a.dragging && a.dragSinger >= 0) {
            if (a.nav == NavMode::Singers &&
                hit(a.rcBrowser, ui.in.mx, ui.in.my)) {
                int nAct = 0;
                for (const auto& sr : a.singers)
                    if (sr.itemId >= 0 &&
                        (sr.status == "waiting" || sr.status == "singing"))
                        ++nAct;
                int idx = std::clamp(
                    int((ui.in.my - a.rcBrowserList.top) / 26 + a.libScroll + 0.5f),
                    0, nAct);
                if (idx > a.dragSinger) --idx;
                moveSingerRow(a, a.dragSinger, (std::min)(idx, nAct - 1));
            }
        } else if (a.dragging && a.dragItems.size() > 1) {
            // Multi-drag: dropping on the queue OR a deck queues them all.
            if (hit(a.rcDeck[0], ui.in.mx, ui.in.my) ||
                hit(a.rcDeck[1], ui.in.mx, ui.in.my) ||
                hit(a.rcQueue, ui.in.mx, ui.in.my)) {
                for (const Match& m : a.dragItems) a.queue.push_back(m);
                a.status = L"queued " + std::to_wstring(a.dragItems.size()) +
                           L" tracks";
            }
        } else if (a.dragging) {
            const int src = a.dragFromQueue; // remove first: a busy-deck drop
            if (src >= 0 && src < int(a.queue.size()) && // re-queues at the front
                (hit(a.rcDeck[0], ui.in.mx, ui.in.my) ||
                 hit(a.rcDeck[1], ui.in.mx, ui.in.my)))
                a.queue.erase(a.queue.begin() + src);
            if (hit(a.rcDeck[0], ui.in.mx, ui.in.my)) dropOnDeck(a, 0, a.dragItem);
            else if (hit(a.rcDeck[1], ui.in.mx, ui.in.my)) dropOnDeck(a, 1, a.dragItem);
            else if (hit(a.rcQueue, ui.in.mx, ui.in.my)) {
                if (src >= 0 && src < int(a.queue.size()) && a.queueOpen) {
                    // Reorder: drop the row at the indicator position.
                    int idx = std::clamp(
                        int((ui.in.my - a.rcQueueList.top) / 26 + a.queueScroll + 0.5f),
                        0, int(a.queue.size()));
                    a.queue.erase(a.queue.begin() + src);
                    if (idx > src) --idx;
                    a.queue.insert(a.queue.begin() + idx, a.dragItem);
                    a.selQueue = idx;
                } else if (src < 0) {
                    a.queue.push_back(a.dragItem);
                    a.status = L"queued: " + a.dragItem.label;
                }
            } else if (a.dragFromList >= 0 && a.nav == NavMode::Playlist &&
                       hit(a.rcBrowser, ui.in.mx, ui.in.my)) {
                // Reorder inside the playlist view; persisted immediately.
                int idx = std::clamp(
                    int((ui.in.my - a.rcBrowserList.top) / 26 + a.libScroll + 0.5f),
                    0, int(a.results.size()));
                const int from = a.dragFromList;
                if (idx > from) --idx;
                idx = (std::min)(idx, int(a.results.size()) - 1);
                if (idx >= 0 && idx != from) {
                    movePlaylistItem(a, a.navPlaylist, from, idx);
                    a.searchDirty = true; // reload the view in the new order
                    a.selLib = idx;
                }
            }
        }
        a.dragArmed = a.dragging = false;
        a.dragItems.clear();
        a.dragFromQueue = a.dragFromList = a.dragSinger = -1;
    }

    if (a.prompt != App::Prompt::None) { // in-app modal, drawn over everything
        if (promptWasOpen) {
            ui.in = savedIn;
        } else { // opened THIS frame: swallow the opening click, or the
            ui.in.pressed = false; // click-outside-cancels below fires instantly
            ui.in.down = false;
        }
        const bool confirm = a.prompt == App::Prompt::Confirm;
        const bool singer = a.prompt == App::Prompt::NewSinger;
        const bool columns = a.prompt == App::Prompt::Columns;
        const bool reqs = a.prompt == App::Prompt::Requests;
        const bool tags = a.prompt == App::Prompt::TagEdit;
        const int nReq = (std::min)(9, int(a.reqInbox.size()));
        ui.rect(rc(0, 0, W, H), col(0x000000, 0.55f), 0);
        const float pw = tags      ? 560.f
                         : reqs    ? 640.f
                         : columns ? 380.f
                         : confirm ? 560.f
                                   : 440.f,
                    ph = tags      ? 232.f
                         : reqs    ? 96.f + (std::max)(nReq, 1) * 32.f
                         : columns ? 292.f
                                   : 132.f;
        const D2D1_RECT_F p = rc((W - pw) / 2, (H - ph) / 2, pw, ph);
        ui.rect(p, cPanel, 10);
        ui.frameRect(p, confirm ? cRed : cAccent, 10);
        ui.text(rc(p.left + 16, p.top + 10, pw - 32, 16),
                tags      ? L"EDIT TAGS — " + leafName(a.tagEditItem.path)
                : reqs    ? L"PHONE REQUESTS"
                : columns ? L"BROWSER COLUMNS"
                : confirm ? a.confirmTitle
                : singer  ? L"ADD TO ROTATION — NEW SINGER"
                          : L"NEW PLAYLIST",
                11, confirm ? cRed : cAccent, 0, true);
        if (tags) {
            static const wchar_t* fl[4] = {L"Artist", L"Title", L"Genre",
                                           L"Year"};
            float cy2 = p.top + 32;
            for (int k = 0; k < 4; ++k) {
                ui.text(rc(p.left + 16, cy2, 70, 26), fl[k], 12, cDim, 0, false);
                const D2D1_RECT_F fb = rc(p.left + 92, cy2, pw - 108, 26);
                ui.rect(fb, cInset, 6);
                ui.frameRect(fb, a.tagFocus == k ? cAccent : cBorder, 5);
                ui.text(rc(fb.left + 8, fb.top, fb.right - fb.left - 16, 26),
                        a.tagField[k] +
                            (a.tagFocus == k && caretOn() ? L"▏" : L""),
                        12, cText, 0, false);
                if (ui.in.pressed && hit(fb, ui.in.pressX, ui.in.pressY))
                    a.tagFocus = k;
                cy2 += 34;
            }
            if (ui.button(502, rc(p.left + 16, p.bottom - 40, 130, 28),
                          L"SAVE", cGreen, true))
                applyTagEdit(a, false);
            if (ui.button(503, rc(p.left + 154, p.bottom - 40, 190, 28),
                          L"SAVE + WRITE FILE", cAccent))
                applyTagEdit(a, true);
        } else if (reqs) {
            float cy2 = p.top + 34;
            if (a.reqInbox.empty())
                ui.text(rc(p.left + 16, cy2, pw - 32, 26),
                        L"No pending requests.", 12, cDim, 0, false);
            int addK = -1, killK = -1;
            for (int k = 0; k < nReq; ++k) {
                const PhoneRequest& pr = a.reqInbox[k];
                ui.text(rc(p.left + 16, cy2, 140, 26), pr.singer, 12, cAccent, 0,
                        true);
                ui.text(rc(p.left + 162, cy2, pw - 162 - 190, 26), pr.song.label,
                        12, cText, 0, false);
                if (ui.button(560 + k, rc(p.right - 176, cy2 + 1, 84, 24), L"ADD",
                              cGreen, true))
                    addK = k;
                if (ui.button(575 + k, rc(p.right - 86, cy2 + 1, 70, 24),
                              L"REJECT", cRed))
                    killK = k;
                cy2 += 32;
            }
            if (int(a.reqInbox.size()) > nReq)
                ui.text(rc(p.left + 16, cy2 + 2, pw - 32, 16),
                        L"+ " + std::to_wstring(a.reqInbox.size() - nReq) +
                            L" more waiting",
                        10, cDim, 0, false);
            if (addK >= 0) { // into the rotation under that singer
                addToRotationAs(a, a.reqInbox[addK].song,
                                a.reqInbox[addK].singer);
                a.status = L"rotation: " + a.reqInbox[addK].singer + L" — " +
                           a.reqInbox[addK].song.label;
                a.reqInbox.erase(a.reqInbox.begin() + addK);
            } else if (killK >= 0) {
                a.reqInbox.erase(a.reqInbox.begin() + killK);
            }
            if (ui.button(500, rc(p.right - 96, p.bottom - 38, 80, 26), L"DONE",
                          cGreen, true))
                a.prompt = App::Prompt::None;
        } else if (columns) {
            static const wchar_t* names[6] = {L"Title", L"Artist", L"Genre",
                                              L"Year",  L"BPM",    L"Time"};
            float cy2 = p.top + 34;
            for (int k = 0; k < 6; ++k) {
                const int id = a.colSeq[k];
                if (ui.button(520 + k, rc(p.left + 16, cy2, 28, 24), L"▴",
                              cAccent) &&
                    k > 0)
                    std::swap(a.colSeq[k], a.colSeq[k - 1]);
                if (ui.button(530 + k, rc(p.left + 48, cy2, 28, 24), L"▾",
                              cAccent) &&
                    k < 5)
                    std::swap(a.colSeq[k], a.colSeq[k + 1]);
                ui.text(rc(p.left + 88, cy2, 120, 24), names[id], 13,
                        a.colShow[id] ? cText : cDim, 0, a.colShow[id]);
                if (ui.toggle(540 + k, rc(p.right - 100, cy2, 84, 24),
                              a.colShow[id] ? L"SHOWN" : L"HIDDEN",
                              a.colShow[id], cGreen)) {
                    int vis = 0; // never hide the last visible column
                    for (bool b : a.colShow) vis += b ? 1 : 0;
                    if (!a.colShow[id] || vis > 1) a.colShow[id] = !a.colShow[id];
                }
                cy2 += 30;
            }
            if (ui.button(500, rc(p.right - 96, p.bottom - 38, 80, 26), L"DONE",
                          cGreen, true))
                a.prompt = App::Prompt::None;
        } else if (confirm) {
            ui.text(rc(p.left + 16, p.top + 34, pw - 32, 18), a.confirmL1, 12,
                    cText, 0, false);
            ui.text(rc(p.left + 16, p.top + 54, pw - 32, 18), a.confirmL2, 11,
                    cDim, 0, false);
            if (ui.button(500, rc(p.right - 176, p.bottom - 40, 72, 28), L"YES",
                          cRed, true))
                commitPrompt(a);
        } else {
            ui.text(rc(p.left + 16, p.top + 27, pw - 32, 16),
                    singer ? a.rotAddPending.label
                           : L"fill it by right-click → Add to playlist, or drag rows in",
                    12, cDim, 0, false);
            const D2D1_RECT_F box = rc(p.left + 16, p.top + 50, pw - 32, 28);
            ui.rect(box, cInset, 7);
            ui.frameRect(box, cAccent, 5);
            ui.text(rc(box.left + 8, box.top, box.right - box.left - 16, 28),
                    a.promptText + (caretOn() ? L"▏" : L""), 13, cText, 0,
                    false);
            if (ui.button(500, rc(p.right - 176, p.bottom - 40, 72, 28),
                          singer ? L"ADD" : L"CREATE", cGreen, true))
                commitPrompt(a);
        }
        if ((!columns && !reqs &&
             ui.button(501, rc(p.right - 96, p.bottom - 40, 80, 28), L"CANCEL",
                       confirm ? cDim : cRed)) ||
            (ui.in.pressed && !hit(p, ui.in.pressX, ui.in.pressY)))
            a.prompt = App::Prompt::None; // click outside also cancels
    }
}
