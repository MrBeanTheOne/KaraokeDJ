#include "app/kdj.h"

// Top-level frame composition and every modal (confirm, columns, tag
// editor, requests, text prompts).
//
// The drawing used to be one file. It is split by screen area so no
// single one grows unreadable: kdj_ui_decks (decks + mixer + rhythm),
// kdj_ui_browser (sidebar + browser + queue), kdj_ui_settings (the
// settings window). They share only what kdj.h declares.

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
        // Say WHY the count is frozen while a deck is live, or it reads as a
        // hung analyzer for the length of the song.
        swprintf(perf, 96, L"CPU %.1f%%   RAM %d MB   %ls %d / %d", a.cpuPct,
                 a.ramMb,
                 a.bpmWaiting.load() ? L"ANALYSIS HELD (PLAYING)"
                                     : L"ANALYZING BPM + KEY",
                 a.bpmDone.load(), a.bpmTotal.load());
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
            const int m = a.chosenMonitor >= 0 && a.chosenMonitor < n
                              ? a.chosenMonitor
                              : VideoWindow::defaultMonitor();
            a.fullOut = std::make_unique<VideoWindow>();
            // Deliberately does NOT latch chosenMonitor: only the right-click
            // menu records a deliberate choice. Latching the auto-derived one
            // wrote video_monitor=0 during single-screen use, and that 0 then
            // beat the default once a TV was plugged in — putting the lyrics
            // on the operator's own screen.
            if (a.fullOut->create(m)) a.outMonitor = m;
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
                size_t qn = 0, qskip = 0;
                for (const Match& m : a.dragItems) {
                    if (pathOffline(m.path, a.driveMask)) { ++qskip; continue; }
                    a.queue.push_back(m);
                    ++qn;
                }
                a.status = qskip ? L"queued " + std::to_wstring(qn) +
                                       L", skipped " + std::to_wstring(qskip) +
                                       L" on a disconnected drive"
                                 : L"queued " + std::to_wstring(qn) + L" tracks";
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
                } else if (src < 0 && ensurePlayable(a, a.dragItem)) {
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
                         : columns ? 322.f
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
            static const wchar_t* names[App::kNumCols] = {
                L"Title", L"Artist", L"Genre", L"Year",
                L"BPM",   L"Time",   L"Key"};
            float cy2 = p.top + 34;
            for (int k = 0; k < App::kNumCols; ++k) {
                const int id = a.colSeq[k];
                if (ui.button(520 + k, rc(p.left + 16, cy2, 28, 24), L"▴",
                              cAccent) &&
                    k > 0)
                    std::swap(a.colSeq[k], a.colSeq[k - 1]);
                if (ui.button(530 + k, rc(p.left + 48, cy2, 28, 24), L"▾",
                              cAccent) &&
                    k < App::kNumCols - 1)
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
