#include "app/kdj.h"

// Window class, message loop, and app lifetime. Everything else
// lives in kdj_data / kdj_engine / kdj_ui / kdj_menus (see kdj.h).

static App* g_app = nullptr;

// ---------------------------------------------------------- settings window
static UiInput g_setIn;
static Ui g_setUi;
static bool g_setClosed = false;

static LRESULT CALLBACK settingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE:
        g_setIn.mx = float(GET_X_LPARAM(lp));
        g_setIn.my = float(GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(h);
        g_setIn.down = true;
        g_setIn.pressed = true;
        g_setIn.mx = g_setIn.pressX = float(GET_X_LPARAM(lp));
        g_setIn.my = g_setIn.pressY = float(GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        g_setIn.down = false;
        g_setIn.released = true;
        return 0;
    case WM_MOUSEWHEEL:
        g_setIn.wheel += float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        return 0;
    case WM_CHAR:
        if (wp >= 32 || wp == 8) g_setIn.typed.push_back(wchar_t(wp));
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_setClosed = true;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Showtime mode. rcMonitor (not rcWork) is what covers the taskbar, and
// dropping WS_OVERLAPPEDWINDOW is what removes the title bar with its
// minimize/close. HWND_TOP rather than TOPMOST so dialogs and the video
// output can still come forward.
static void setAppFullscreen(App& a, HWND h, bool on) {
    if (on == a.appFull) return;
    if (on) {
        a.preFull.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(h, &a.preFull);
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        SetWindowLongPtrW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(h, &a.preFull);
        SetWindowPos(h, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    a.appFull = on;
}

static void applyDarkTitlebar(HWND hwnd) {
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20; 19 pre-20H1.
    using DwmSetFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        if (auto set = reinterpret_cast<DwmSetFn>(
                GetProcAddress(dwm, "DwmSetWindowAttribute"))) {
            const BOOL dark = TRUE;
            if (FAILED(set(hwnd, 20, &dark, sizeof(dark))))
                set(hwnd, 19, &dark, sizeof(dark));
        }
    }
}

// Open (or focus) the settings window, centered on the owner.
static void openSettingsWindow(App& a, HWND owner, HINSTANCE hi) {
    if (a.settingsWnd) {
        ShowWindow(a.settingsWnd, SW_RESTORE);
        SetForegroundWindow(a.settingsWnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = settingsProc;
        wc.hInstance = hi;
        wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"KaraokeDjSettings";
        RegisterClassW(&wc);
        registered = true;
    }
    UINT dpi = 96;
    using GetDpiFn = UINT(WINAPI*)(HWND);
    if (auto getDpi = reinterpret_cast<GetDpiFn>(GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")))
        dpi = getDpi(owner);
    const int w = MulDiv(690, dpi, 96), h = MulDiv(760, dpi, 96);
    RECT orc{};
    GetWindowRect(owner, &orc);
    const int cx = (orc.left + orc.right - w) / 2, cy = (orc.top + orc.bottom - h) / 2;
    a.settingsWnd = CreateWindowExW(0, L"KaraokeDjSettings", L"Settings — Karaoke DJ",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                    cx, cy, w, h, owner, nullptr, hi, nullptr);
    if (!a.settingsWnd) return;
    applyDarkTitlebar(a.settingsWnd);
    g_setIn = UiInput{};
    g_setClosed = false;
    g_setUi.init(a.settingsWnd);
    a.audioDevs = listAudioOutputs(); // fresh device list on open
    ShowWindow(a.settingsWnd, SW_SHOW);
}

// ------------------------------------------------------------------- window

static UiInput g_pending;
static UINT g_w = 1380, g_h = 860;
static bool g_sized = false;
static bool g_closed = false;
static bool g_dpiChanged = false;
static bool g_displayChanged = false;

static LRESULT CALLBACK mainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE:
        g_pending.mx = float(GET_X_LPARAM(lp));
        g_pending.my = float(GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(h);
        g_pending.down = true;
        g_pending.pressed = true;
        g_pending.shift = (wp & MK_SHIFT) != 0;
        g_pending.ctrl = (wp & MK_CONTROL) != 0;
        g_pending.mx = g_pending.pressX = float(GET_X_LPARAM(lp));
        g_pending.my = g_pending.pressY = float(GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONDBLCLK:
        g_pending.down = true;
        g_pending.dblclick = true;
        g_pending.mx = g_pending.dblX = float(GET_X_LPARAM(lp));
        g_pending.my = g_pending.dblY = float(GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        g_pending.down = false;
        g_pending.released = true;
        return 0;
    case WM_RBUTTONDOWN:
        g_pending.rpressed = true;
        g_pending.rX = float(GET_X_LPARAM(lp));
        g_pending.rY = float(GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEWHEEL:
        g_pending.wheel += float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        return 0;
    case WM_CHAR:
        if (wp == 22) { // Ctrl+V: paste clipboard text as typed input (URLs)
            if (OpenClipboard(h)) {
                if (HANDLE hd = GetClipboardData(CF_UNICODETEXT)) {
                    if (auto* p = static_cast<const wchar_t*>(GlobalLock(hd))) {
                        for (; *p; ++p)
                            if (*p >= 32) g_pending.typed.push_back(*p);
                        GlobalUnlock(hd);
                    }
                }
                CloseClipboard();
            }
        } else if (wp == L'\r') g_pending.enter = true;
        else if (wp == L' ' && g_app && g_app->prompt == App::Prompt::None &&
                 g_app->focus == Focus::None)
            g_pending.pauseKey = true; // space only types inside a box
        else if (wp >= 32 || wp == 8) g_pending.typed.push_back(wchar_t(wp));
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_DELETE) g_pending.del = true;
        else if (wp == VK_NEXT) g_pending.pgdn = true;
        else if (wp == VK_PRIOR) g_pending.pgup = true;
        else if (wp == VK_UP) g_pending.navKey = -1;
        else if (wp == VK_DOWN) g_pending.navKey = 1;
        else if (wp == VK_ESCAPE) g_pending.esc = true;
        else if (wp == 'N' && (GetKeyState(VK_CONTROL) & 0x8000) && g_app &&
                 g_app->prompt == App::Prompt::None) {
            // Ctrl+N: next singer (the char handler sees ^N as code 14)
            for (const auto& s : g_app->singers)
                if (s.status == "waiting") { singNow(*g_app, s); break; }
        }
        return 0;
    case WM_GETMINMAXINFO: {
        // The deck layout packs PLAY/CLEAR and the key group into one row and
        // they overlap below ~1050 DIP wide, where a single click would hit
        // both. Nothing else enforced a floor — the saved-size clamp only
        // applies at startup — so pin it here.
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        UINT dpi = 96;
        using GetDpiFn = UINT(WINAPI*)(HWND);
        if (auto getDpi = reinterpret_cast<GetDpiFn>(GetProcAddress(
                GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")))
            if (const UINT d = getDpi(h)) dpi = d;
        // Width is the one that matters (the deck row collides below ~1050).
        // Height stays at the 600 the settings loader has always used: a 720p
        // screen cannot fit a 720-tall window plus its frame, and pinning it
        // there would push the bottom of the UI off a small display.
        mmi->ptMinTrackSize.x = MulDiv(1140, dpi, 96);
        mmi->ptMinTrackSize.y = MulDiv(600, dpi, 96);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP hd = reinterpret_cast<HDROP>(wp);
        POINT pt{};
        DragQueryPoint(hd, &pt);
        const UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> files;
        for (UINT i = 0; i < n; ++i) {
            const UINT len = DragQueryFileW(hd, i, nullptr, 0);
            std::wstring f(len, 0);
            DragQueryFileW(hd, i, f.data(), len + 1);
            files.push_back(std::move(f));
        }
        DragFinish(hd);
        if (g_app) // physical client coords -> DIPs for hit testing
            dropExternal(*g_app, float(pt.x) / g_app->uiScale,
                         float(pt.y) / g_app->uiScale, files);
        return 0;
    }
    case WM_TIMER: // keeps audio-follow video presenting during modal menus
        if (g_app) engineTick(*g_app);
        return 0;
    case 0x02E0: { // WM_DPICHANGED: move to the suggested rect, re-read DPI
        const RECT* pr = reinterpret_cast<RECT*>(lp);
        SetWindowPos(h, nullptr, pr->left, pr->top, pr->right - pr->left,
                     pr->bottom - pr->top, SWP_NOZORDER | SWP_NOACTIVATE);
        g_dpiChanged = true;
        return 0;
    }
    case WM_SIZE:
        g_w = LOWORD(lp);
        g_h = HIWORD(lp);
        g_sized = true;
        return 0;
    case WM_DISPLAYCHANGE: // monitor added/removed/re-arranged (FR-011)
        g_displayChanged = true;
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_app && g_app->hoverResize) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        break;
    case WM_CLOSE:
        // Closing ends the night: warn first, because it clears the decks and
        // whatever is still queued. WM_DESTROY (and a confirmed quit) still
        // goes straight through.
        if (g_app && !g_app->quitConfirmed) {
            // The prompt is drawn into THIS window, so closing from the
            // taskbar while minimized would leave it invisible and the app
            // looking hung. Surface the window before asking.
            if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
            SetForegroundWindow(h);
            askConfirm(*g_app, App::ConfirmAction::QuitApp, L"CLOSE KARAOKE DJ?",
                       L"Decks, queue, singer rotation and tonight's history "
                       L"will be cleared.",
                       L"Your library, playlists, markers and settings are kept.");
            return 0;
        }
        g_closed = true;
        return 0;
    case WM_DESTROY:
        g_closed = true;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    // Single instance: a second launch just focuses the existing window.
    // (Two instances share one library DB and fight over the audio device.)
    CreateMutexW(nullptr, TRUE, L"KaraokeDJ.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND prev = FindWindowW(L"KaraokeDjMain", nullptr)) {
            ShowWindow(prev, SW_RESTORE);
            SetForegroundWindow(prev);
        }
        return 0;
    }
    // Per-monitor DPI awareness: never let DWM scale (blur) or re-map input.
    using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (auto setCtx = reinterpret_cast<SetCtxFn>(GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
        setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    } else {
        SetProcessDPIAware();
    }
    // Dark native context menus + file dialogs (undocumented uxtheme,
    // Win10 1809+; harmless no-op elsewhere). Confirms are the app's own
    // themed modal now — no native MessageBox left in normal operation.
    if (HMODULE ux = LoadLibraryW(L"uxtheme.dll")) {
        using SetModeFn = int(WINAPI*)(int);
        if (auto setMode = reinterpret_cast<SetModeFn>(
                GetProcAddress(ux, MAKEINTRESOURCEA(135))))
            setMode(2); // ForceDark
        using VoidFn = void(WINAPI*)();
        if (auto flush = reinterpret_cast<VoidFn>(
                GetProcAddress(ux, MAKEINTRESOURCEA(136))))
            flush();
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!MFDecoder::initMF()) return 1;

    App a;
    g_app = &a;
    a.dbPath = defaultDbPath();
    a.db.open(a.dbPath);
    UINT winW = 0, winH = 0;
    loadSettings(a, winW, winH);
    {
        // First run — or a restore size an older build saved off a maximized
        // window, which would make "restore" do nothing. Either way: ~80% of
        // the primary work area.
        const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        if (winW <= 900 || int(winW) >= sw || int(winH) >= sh) {
            winW = UINT(sw * 0.78);
            winH = UINT(sh * 0.82);
        }
    }

    WNDCLASSW wc{};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = mainProc;
    wc.hInstance = hi;
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)); // assets/app.rc
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"KaraokeDjMain";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Karaoke DJ", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, int(winW), int(winH),
                                nullptr, nullptr, hi, nullptr);
    if (!hwnd) return 1;
    applyDarkTitlebar(hwnd);
    DragAcceptFiles(hwnd, TRUE);
    // Opens filling the screen unless the operator left it un-maximized last
    // time; win_w/win_h are then the size it restores to.
    ShowWindow(hwnd, a.winMax ? SW_SHOWMAXIMIZED : SW_SHOW);
    if (a.appFull) { // restore showtime mode; the flag is set by loadSettings
        a.appFull = false; // the window is not fullscreen yet
        setAppFullscreen(a, hwnd, true);
    }

    if (!a.out.start(kRate, kCh, a.mixer, wide(a.audioDevice))) {
        MessageBoxW(hwnd, L"WASAPI audio init failed", L"Karaoke DJ", MB_ICONERROR);
        return 1;
    }
    a.web.setPassword(a.webPass); // set before listen: no unprotected window
    a.web.setLanguage(a.lang);    // phone page follows the app language
    if (a.webOn && !a.web.start(a.dbPath)) a.webOn = false; // opt-in feature
    startUpdateCheck(a, false); // silent: only speaks up when newer exists
    startWatcher(a); // no-op unless the watch-folders setting is on

    Ui ui;
    if (!ui.init(hwnd)) return 1;
    SetTimer(hwnd, 1, 30, nullptr);

    // Crash recovery: the clean flag is written on graceful exit only, so a
    // missing flag here means the last session died — restore its snapshot.
    const bool crashed = getSetting(a.db, "snap_clean", "1") == "0";
    setSetting(a.db, "snap_clean", "0"); // this session is now in progress
    if (crashed) restoreSnapshot(a);
    startBpmAnalysis(a); // fill in BPMs the last session didn't get to

    auto lastDraw = Clock::now();
    while (!g_closed) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_dpiChanged) { ui.refreshDpi(); g_dpiChanged = false; }
        if (g_sized) { ui.resize(g_w, g_h); g_sized = false; }
        a.uiScale = ui.dpiScale();

        // Keyboard input is consumed immediately — the loop runs several times
        // per drawn frame, and reprocessing pending characters duplicates them.
        for (wchar_t c : g_pending.typed) {
            Focus tgt = a.focus; // typing with no focus goes to the view's box
            if (a.prompt == App::Prompt::None && tgt == Focus::None)
                tgt = a.nav == NavMode::Singers ? Focus::SingerName
                                                : Focus::Search;
            std::wstring& target =
                a.prompt == App::Prompt::TagEdit
                    ? a.tagField[std::clamp(a.tagFocus, 0, 3)]
                : a.prompt != App::Prompt::None ? a.promptText
                : tgt == Focus::SingerName      ? a.singerFilter
                : tgt == Focus::IdleTitle       ? a.idleTitle
                                                : a.search;
            if (c == 8) { if (!target.empty()) target.pop_back(); }
            else target += c;
            if (a.prompt == App::Prompt::None) {
                a.focus = tgt; // the box lights up once you type
                if (tgt == Focus::Search) {
                    a.searchDirty = true;
                    a.searchEditAt = Clock::now(); // debounced reload
                    a.libScroll = 0;               // new search: back to top
                    a.selLib = -1;
                }
                if (tgt == Focus::SingerName) a.navDirty = true;
            }
        }
        g_pending.typed.clear();
        if (g_pending.enter) {
            if (a.prompt != App::Prompt::None) {
                commitPrompt(a);
            } else if (a.focus == Focus::Search && a.search.rfind(L"http", 0) == 0) {
                startYoutube(a, a.search); // pasted URL: download, then queue
                a.search.clear();
                a.searchDirty = true;
            } else if (a.nav != NavMode::Singers && a.nav != NavMode::Settings &&
                       a.nav != NavMode::History) {
                queueSelected(a);
            }
        }
        if (a.prompt == App::Prompt::None && g_pending.del && a.selQueue >= 0 &&
            a.selQueue < int(a.queue.size())) {
            a.queue.erase(a.queue.begin() + a.selQueue);
            a.selQueue = (std::min)(a.selQueue, int(a.queue.size()) - 1);
        }
        if (a.prompt == App::Prompt::None && g_pending.pgdn) { // skip: cued deck, else queue
            const int act = a.mixer.activeDeck.load();
            const int idle = act < 0 ? 0 : 1 - act;
            if (!a.label[idle].empty() && a.decks[idle]->state() == DeckState::Ready &&
                a.pendingFade < 0 && a.mixer.fadeTo.load() < 0) {
                a.pendingFade = idle;
                a.pendingDur = act < 0 ? 0.3 : (a.automixOn ? a.fadeSec : 0.08);
            } else if (!a.queue.empty()) {
                const Match m = a.queue.front();
                a.queue.pop_front();
                playNow(a, m);
            }
        }
        if (a.prompt == App::Prompt::None && g_pending.pgup) {
            const int act = a.mixer.activeDeck.load();
            if (act >= 0) seekFrac(a, act, 0.f);
        }
        if (g_pending.pauseKey && a.prompt == App::Prompt::None) {
            const int act = a.mixer.activeDeck.load(); // space = pause/resume
            if (act >= 0)
                a.decks[act]->paused.store(!a.decks[act]->paused.load());
        }
        if (g_pending.navKey && a.prompt == App::Prompt::None &&
            a.nav != NavMode::Singers && a.nav != NavMode::History &&
            !a.results.empty()) { // arrows walk the browser selection
            a.selLib = std::clamp(a.selLib + g_pending.navKey, 0,
                                  int(a.results.size()) - 1);
            a.selRows.clear();
            a.selRows.insert(a.selLib);
            const float vis = // keep the selection on screen
                (a.rcBrowserList.bottom - a.rcBrowserList.top) / 26.f;
            if (float(a.selLib) < a.libScroll) a.libScroll = float(a.selLib);
            else if (float(a.selLib) > a.libScroll + vis - 1)
                a.libScroll = float(a.selLib) - vis + 1;
        }
        if (g_pending.esc) { // Esc: close modal, else unfocus, else clear search
            if (a.prompt != App::Prompt::None) a.prompt = App::Prompt::None;
            else if (a.focus != Focus::None) a.focus = Focus::None;
            else if (!a.search.empty()) {
                a.search.clear();
                a.searchDirty = true;
                a.libScroll = 0;
            }
            // Nothing left to back out of: Esc is the way out of showtime
            // mode, which has no title bar to close from.
            else if (a.appFull) setAppFullscreen(a, hwnd, false);
        }
        g_pending.enter = g_pending.del = g_pending.pgdn = g_pending.pgup = false;
        g_pending.pauseKey = g_pending.esc = false;
        g_pending.navKey = 0;
        if (a.pickDone.exchange(false)) {
            if (a.pickThread.joinable()) a.pickThread.join();
            if (!a.pickResult.empty()) {
                if (a.pickKind == 1) { // waiting-screen logo
                    a.idleLogoPath = a.pickResult;
                    a.idleLogoLoaded = loadImageFile(a.idleLogoPath, a.idleLogo);
                    if (!a.idleLogoLoaded) {
                        a.idleLogoPath.clear();
                        a.status = L"couldn't read that image";
                    } else {
                        a.status = L"waiting-screen logo set";
                    }
                    setSetting(a.db, "idle_logo", utf8(a.idleLogoPath));
                } else if (a.pickKind == 3) { // export profile
                    a.status = exportProfile(a, a.pickResult)
                                   ? L"profile exported: " +
                                         leafName(a.pickResult)
                                   : L"profile export failed";
                } else if (a.pickKind == 4) { // import profile
                    a.status = importProfile(a, a.pickResult)
                                   ? L"profile imported — settings and "
                                     L"playlists applied"
                                   : L"that file is not a Karaoke DJ profile";
                } else if (a.pickKind == 2) { // waiting-screen background
                    a.idleBgPath = a.pickResult;
                    a.idleBgLoaded = loadImageFile(a.idleBgPath, a.idleBg, 1920);
                    if (!a.idleBgLoaded) {
                        a.idleBgPath.clear();
                        a.status = L"couldn't read that image";
                    } else {
                        a.status = L"waiting-screen background set";
                    }
                    setSetting(a.db, "idle_bg", utf8(a.idleBgPath));
                } else if (a.pickKind == 6) { // a folder's media moved
                    const int n = relocateFolder(a, a.relocFrom, a.pickResult);
                    a.status =
                        n == -1 ? L"none of those tracks are in that folder — "
                                  L"nothing changed"
                        : n == -2 ? L"that folder is already in the library — "
                                    L"remove one of the two first"
                        : n == 0 ? L"relocate failed — library unchanged"
                                 : std::to_wstring(n) + L" tracks now point at " +
                                       a.pickResult;
                    a.relocFrom.clear();
                } else if (a.pickKind == 5) { // YouTube download folder
                    a.ytDir = a.pickResult;
                    setSetting(a.db, "yt_dir", utf8(a.ytDir));
                    a.status = L"YouTube downloads → " + a.ytDir;
                } else {
                    startImport(a, a.pickResult);
                }
            }
            a.pickResult.clear();
            a.pickKind = 0;
        }
        // A mid-gig import yields to playback (scan threads follow this).
        a.scanProg.gentle.store(a.mixer.activeDeck.load() >= 0,
                                std::memory_order_relaxed);
        if (a.scanning.load()) { // live import progress in the status line
            wchar_t b[160];
            if (a.scanProg.phase.load() == 0) {
                swprintf(b, 160, L"importing…  discovering files: %d",
                         a.scanProg.walked.load());
            } else {
                const int t = a.scanProg.total.load(), d = a.scanProg.done.load();
                swprintf(b, 160, L"importing…  reading tags %d / %d  (%d%%)", d, t,
                         t ? d * 100 / t : 100);
            }
            a.status = b;
        }
        if (a.scanFinished.exchange(false)) {
            a.navDirty = a.searchDirty = true;
            a.status = L"import complete";
            if (!a.rescanQueue.empty()) { // next folder in the update line
                const std::wstring next = a.rescanQueue.front();
                a.rescanQueue.pop_front();
                startImport(a, next);
            } else {
                startBpmAnalysis(a); // detect BPM once the line is empty
                startWatcher(a);     // roots may have changed
            }
        }
        // Folder watcher: rescan a changed root once it has been quiet for
        // 5 s (a burst of copied files collapses into one import).
        if (a.watchOn && !a.scanning.load()) {
            const uint64_t last = a.watchLastEvent.load();
            if (last && GetTickCount64() - last > 5000) {
                std::wstring root;
                {
                    std::lock_guard<std::mutex> lk(a.watchMx);
                    if (!a.watchDirtyRoots.empty()) {
                        root = *a.watchDirtyRoots.begin();
                        a.watchDirtyRoots.erase(a.watchDirtyRoots.begin());
                    }
                    if (a.watchDirtyRoots.empty()) a.watchLastEvent.store(0);
                }
                if (!root.empty()) {
                    a.status = L"folder changed — updating library";
                    queueRescan(a, root);
                }
            }
        }
        if (a.bpmBusy.load()) { // refresh the BPM column as results land
            static int bpmShown = 0;
            const int d = a.bpmDone.load();
            if (d - bpmShown >= 20 || d < bpmShown) {
                bpmShown = d;
                a.searchDirty = true;
            }
        }
        if (a.bpmFinished.exchange(false)) {
            a.searchDirty = true;
            a.status = L"BPM analysis complete";
        }
        if (a.updDone.exchange(false)) {
            if (a.updThread.joinable()) a.updThread.join();
            if (!a.updLatest.empty())
                a.status = L"update available: v" + a.updLatest +
                           L"  — get it in Settings";
            else if (a.updManual)
                a.status = a.updError.empty() ? L"you're on the latest version"
                                              : L"update check: " + a.updError;
        }
        if (a.web.hasPending()) { // phone requests → DJ inbox + status blip
            for (auto& r : a.web.take()) a.reqInbox.push_back(std::move(r));
            if (!a.reqInbox.empty())
                a.status = L"phone request: " + a.reqInbox.back().singer +
                           L" — " + a.reqInbox.back().song.label;
        }
        if (a.ytDone.exchange(false)) {
            if (a.ytThread.joinable()) a.ytThread.join();
            if (!a.ytPath.empty()) {
                a.queue.push_back(matchFromPath(a, a.ytPath));
                a.status = L"YouTube ready → queued: " + leafName(a.ytPath);
            } else {
                a.status = L"YouTube: " + a.ytError;
            }
        }
        if (g_displayChanged) { // re-place the fullscreen output (FR-011)
            g_displayChanged = false;
            if (a.outMonitor >= 0) {
                const int n = VideoWindow::monitorCount();
                int m = a.chosenMonitor >= 0 ? a.chosenMonitor : n - 1;
                if (m >= n) m = n - 1;
                a.fullOut.reset();
                if (m >= 0) {
                    a.fullOut = std::make_unique<VideoWindow>();
                    if (a.fullOut->create(m)) {
                        a.outMonitor = m;
                        for (int d = 0; d < 2; ++d) // fresh window needs frames
                            if (a.cur[d]) a.fullOut->setFrame(d, *a.cur[d]);
                    } else {
                        a.fullOut.reset();
                        a.outMonitor = -1;
                    }
                } else {
                    a.outMonitor = -1;
                }
            }
        }

        { // CPU/RAM sample for the header readout, once a second
            const auto pnow = Clock::now();
            if (pnow - a.cpuAt >= 1s) {
                FILETIME c2, e2, k2, u2;
                if (GetProcessTimes(GetCurrentProcess(), &c2, &e2, &k2, &u2)) {
                    const uint64_t busy =
                        (uint64_t(k2.dwHighDateTime) << 32 | k2.dwLowDateTime) +
                        (uint64_t(u2.dwHighDateTime) << 32 | u2.dwLowDateTime);
                    const double dtms =
                        std::chrono::duration<double, std::milli>(pnow - a.cpuAt)
                            .count();
                    if (a.cpuPrev100ns && dtms > 0) {
                        static const double nCores =
                            (std::max)(1u, std::thread::hardware_concurrency());
                        a.cpuPct = float((busy - a.cpuPrev100ns) / 10000.0 / dtms *
                                         100.0 / nCores);
                    }
                    a.cpuPrev100ns = busy;
                }
                PROCESS_MEMORY_COUNTERS pmc{};
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
                    a.ramMb = int(pmc.WorkingSetSize >> 20);
                a.cpuAt = pnow;
            }
        }
        if (a.quitConfirmed) break; // close warning accepted: decks already wiped
        if (a.fullscreenReq) { // header button
            a.fullscreenReq = false;
            setAppFullscreen(a, hwnd, !a.appFull);
        }
        if (a.settingsOpenReq) { // header button: toggle the settings window
            a.settingsOpenReq = false;
            if (a.settingsWnd) PostMessageW(a.settingsWnd, WM_CLOSE, 0, 0);
            else openSettingsWindow(a, hwnd, hi);
        }
        if (g_setClosed) {
            g_setClosed = false;
            g_setUi.shutdown();
            a.settingsWnd = nullptr;
            a.setFocusBox = 0;
        }
        if (a.settingsWnd) { // typed input goes to the focused settings box
            bool passEdited = false;
            for (wchar_t c : g_setIn.typed) {
                std::wstring* t = a.setFocusBox == 1   ? &a.idleTitle
                                  : a.setFocusBox == 2 ? &a.webPass
                                  : a.setFocusBox == 3 ? &a.idleSub
                                                       : nullptr;
                if (!t) continue;
                if (c == 8) { if (!t->empty()) t->pop_back(); }
                else *t += c;
                passEdited |= a.setFocusBox == 2;
            }
            g_setIn.typed.clear();
            if (passEdited) a.web.setPassword(a.webPass); // applies live
        }

        if (a.navDirty) reloadNav(a);
        if (a.searchDirty && Clock::now() - a.searchEditAt > 180ms)
            reloadBrowser(a);

        engineTick(a);

        // Activity-driven pacing: 60 fps while the mouse is engaged (drags,
        // scrubs, slider rides feel continuous), ~30 fps when idle.
        const bool engaged = g_pending.down || ui.dragging() || a.scrubDeck >= 0 ||
                             a.resizingSide || a.resizingQueue || a.dragging ||
                             a.colDrag >= 0 || a.markerDeck >= 0 ||
                             a.scrollGrab != 0;
        if (Clock::now() - lastDraw >= (engaged ? 15ms : 33ms)) {
            lastDraw = Clock::now();
            ui.beginFrame(g_pending);
            drawUi(a, ui, float(g_w) / ui.dpiScale(), float(g_h) / ui.dpiScale());
            ui.endFrame();
            if (a.settingsWnd) {
                RECT scr{};
                GetClientRect(a.settingsWnd, &scr);
                g_setUi.beginFrame(g_setIn);
                drawSettings(a, g_setUi,
                             rc(0, 0, scr.right / g_setUi.dpiScale(),
                                scr.bottom / g_setUi.dpiScale()));
                g_setUi.endFrame();
                g_setIn.pressed = g_setIn.released = false;
                g_setIn.pressX = g_setIn.pressY = -1;
                g_setIn.wheel = 0;
            }
            g_pending.pressed = g_pending.released = g_pending.dblclick = false;
            g_pending.pressX = g_pending.pressY = g_pending.dblX = g_pending.dblY = -1;
            g_pending.rpressed = false;
            g_pending.rX = g_pending.rY = -1;
            g_pending.wheel = 0;
            handleMenu(a, hwnd); // after EndDraw: menus/dialogs run modal loops
        }
        std::this_thread::sleep_for(engaged ? 1ms : 4ms);
    }

    KillTimer(hwnd, 1);
    if (a.settingsWnd) {
        DestroyWindow(a.settingsWnd);
        g_setUi.shutdown();
        a.settingsWnd = nullptr;
    }
    g_app = nullptr;
    // Placement, not the window rect: on a maximized window GetWindowRect
    // returns the maximized size, which would then become the restore size.
    WINDOWPLACEMENT wp{sizeof(wp)};
    GetWindowPlacement(hwnd, &wp);
    // In showtime mode the window IS the whole monitor, so the size worth
    // remembering is the one from before the switch.
    if (a.appFull) wp = a.preFull;
    a.winMax = wp.showCmd == SW_SHOWMAXIMIZED ||
               (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
    const RECT& wr = wp.rcNormalPosition;
    saveSettings(a, UINT(wr.right - wr.left), UINT(wr.bottom - wr.top));
    saveSnapshot(a); // final state, then mark the exit graceful
    setSetting(a.db, "snap_clean", "1");
    if (a.pickThread.joinable()) a.pickThread.join();
    a.scanProg.cancel.store(true); // closing cancels a running import promptly;
    if (a.scanThread.joinable()) a.scanThread.join(); // committed rows survive
    a.bpmStop.store(true); // finished BPMs are already committed row-by-row
    if (a.bpmThread.joinable()) a.bpmThread.join();
    if (a.updThread.joinable()) a.updThread.join();
    stopWatcher(a);
    a.web.stop();
    if (a.ytThread.joinable()) a.ytThread.join();
    a.fullOut.reset();
    a.out.stop();
    a.deckA.stopAndUnload();
    a.deckB.stopAndUnload();
    ui.shutdown();
    return 0;
}
