#include "app/kdj.h"

// The settings window: behaviour, video/audio output, library scan,
// phone requests, YouTube, and the waiting-screen designer.
// Split out of kdj_ui.cpp.

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
    ui.text(rc(x, y, 90, 26), L"Language", 12, cText, 0, false);
    if (ui.toggle(607, rc(x + 100, y, 110, 26), L"ENGLISH", a.lang == 0, cGreen) &&
        a.lang != 0) {
        a.lang = 0;
        uiSetLanguage(0);
        a.web.setLanguage(0);
        setSetting(a.db, "lang", "0");
    }
    if (ui.toggle(608, rc(x + 216, y, 110, 26), L"FRANÇAIS", a.lang == 1,
                  cGreen) &&
        a.lang != 1) {
        a.lang = 1;
        uiSetLanguage(1);
        a.web.setLanguage(1);
        setSetting(a.db, "lang", "1");
    }
    y += 36;
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
        const bool dl = a.updDlBusy.load();
        const std::wstring lbl =
            dl ? uiTr(L"DOWNLOADING…") + L"  " +
                     std::to_wstring(a.updDlPct.load()) + L"%"
               : L"GET UPDATE  v" + a.updLatest;
        if (ui.button(604, rc(x, y, 300, 28), lbl, cGreen, !dl) && !dl) {
            if (!a.updAssetUrl.empty())
                startUpdateDownload(a); // installs in place, night restored
            else // release without an installer asset: browser fallback
                ShellExecuteW(nullptr, L"open",
                              L"https://github.com/MrBeanTheOne/KaraokeDJ/"
                              L"releases/latest",
                              nullptr, nullptr, SW_SHOWNORMAL);
        }
        ui.text(rc(x + 310, y, w - 310, 28),
                dl ? uiTr(L"the app restarts into the installer when ready")
                : a.updAssetUrl.empty()
                    ? uiTr(L"opens the download page — this is v") + KDJ_VERSION_W
                    : uiTr(L"downloads and installs — this is v") + KDJ_VERSION_W,
                10, cDim, 0, false);
    } else {
        if (ui.button(604, rc(x, y, 300, 28), L"CHECK FOR UPDATES", cDim))
            startUpdateCheck(a, true);
        ui.text(rc(x + 310, y, w - 310, 28),
                a.updBusy.load() ? uiTr(L"checking…")
                                 : uiTr(L"this is v") + KDJ_VERSION_W,
                10, cDim, 0, false);
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
            if (ui.toggle(613 + m, rc(fx, y, 92, 26), fitNames[m],
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
    if (ui.button(609, rc(x, y, 220, 28), L"EXPORT PROFILE…", cAccent))
        a.menu = {MenuReq::None, -5};
    if (ui.button(610, rc(x + 230, y, 220, 28), L"IMPORT PROFILE…", cAccent))
        a.menu = {MenuReq::None, -6};
    ui.text(rc(x + 460, y, w - 460, 28),
            L"settings, playlists, markers — move them to the laptop", 10,
            cDim, 0, false);
    y += 40;
    if (ui.button(602, rc(x, y, 220, 28), L"CLEAN MISSING FILES", cRed))
        a.menu = {MenuReq::CleanMissing, -1, 0, 0}; // confirm runs after the frame
    ui.text(rc(x + 230, y, w - 230, 28),
            L"drop entries whose file was deleted (unplugged drives are left alone)",
            10, cDim, 0, false);
    y += 44;

    ui.text(rc(x, y, w, 16), L"YOUTUBE", 11, cDim, 0, true);
    y += 24;
    ui.text(rc(x, y, 190, 26), L"Download folder", 12, cText, 0, false);
    if (ui.button(660, rc(x + 200, y, 130, 26), L"PICK FOLDER…", cAccent))
        a.menu = {MenuReq::None, -7}; // STA picker runs after this frame
    if (a.ytDir.empty()) {
        ui.text(rc(x + 340, y, w - 344, 26),
                L"default — %APPDATA%\\KaraokeDJ\\youtube (not in the library)",
                10, cDim, 0, false);
    } else {
        ui.text(rc(x + 340, y, w - 384, 26), a.ytDir, 11, cDim, 0, false);
        if (ui.button(661, rc(x + w - 34, y, 30, 26), L"✕", cRed)) {
            a.ytDir.clear();
            setSetting(a.db, "yt_dir", "");
        }
    }
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
