#pragma once
// Shared state + cross-file API for the Karaoke DJ operator app.

// KARAOKE DJ — the operator app (VirtualDJ-5-style layout, modern dark skin).
// Decks + mixer on top; left sidebar (library / playlists / folder tree /
// singers), browser + queue below. Import scans folders into the library on a
// background thread. Drag & drop: Explorer files or browser rows onto a deck
// (cues without playing), the queue, or a playlist view.
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <psapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "audio/wasapi_out.h"
#include "karaoke/cdg_renderer.h"
#include "library/db.h"
#include "library/media_query.h"
#include "library/scanner.h"
#include "media/media_paths.h"
#include "media/mf_decoder.h"
#include "media/mf_video_decoder.h"
#include "media/waveform.h"
#include "playback/deck.h"
#include "playback/mixer.h"
#include "ui/im.h"
#include "video/video_window.h"
#include "web/request_server.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

static constexpr uint32_t kRate = 48000;
static constexpr uint32_t kCh = 2;

// Palette — VDJ-classic dark: near-black window, neutral graphite panels,
// crimson accent; deck A blue / deck B red like the reference waveforms.
static const D2D1_COLOR_F cPanel = col(0x141416);
static const D2D1_COLOR_F cInset = col(0x060608);
static const D2D1_COLOR_F cBorder = col(0x2A2A2F);
static const D2D1_COLOR_F cText = col(0xECECEF);
static const D2D1_COLOR_F cDim = col(0x94949D);
static const D2D1_COLOR_F cA = col(0x58AAE8);
static const D2D1_COLOR_F cB = col(0xE0554C);
static const D2D1_COLOR_F cAccent = col(0xD22C36);
static const D2D1_COLOR_F cGreen = col(0x4ADE80);
static const D2D1_COLOR_F cRed = col(0xFF5C5C);
static const D2D1_COLOR_F cSel = col(0x3A2026);
static const D2D1_COLOR_F cHover = col(0x1E1E23);

enum class MixMode { Fade, Smart };
enum class NavMode { Library, Playlist, Folder, Singers, Settings, History };
enum class Focus { None, Search, SingerName, IdleTitle };

// itemId >= 0: rotation entry; -1: section divider; -2: play-history entry.
struct SingerRow {
    int64_t itemId = 0, pos = 0;
    std::wstring singer, label, when;
    std::string status;
    Match song;
};

struct MenuReq {
    enum Kind {
        None, VideoOut, Automix, BrowserRow, QueueRow, SingerRow,
        PlaylistRow, FolderRow, CleanMissing, DeckWave, RotationRow, HistoryRow
    } kind = None;
    int index = -1;
    float x = 0, y = 0;
    int64_t plId = -1;  // PlaylistRow
    std::wstring path;  // FolderRow
};

struct App {
    // Engine
    Deck deckA{kRate, kCh, 1 << 19}, deckB{kRate, kCh, 1 << 19};
    Deck* decks[2] = {&deckA, &deckB};
    Mixer mixer{deckA, deckB};
    WasapiOut out;
    MFVideoDecoder vdec[2];
    CdgRenderer cdg[2];
    WaveformScanner wave[2];
    std::unique_ptr<VideoFrame> cur[2];
    bool hasVid[2] = {false, false}, hasCdg[2] = {false, false};
    std::wstring label[2];
    Match deckMatch[2];             // what each deck holds
    int64_t cueIn[2] = {0, 0}, cueOut[2] = {0, 0}; // song start/end markers (ms)
    bool cueFromDb[2] = {false, false};  // saved markers beat smart's guesses
    bool smartCueSet[2] = {false, false};
    int markerDeck = -1, markerWhich = -1; // handle being dragged (0 in, 1 out)
    D2D1_RECT_F rcWave[2]{};        // waveform strips (marker right-click)
    bool autoCued[2] = {false, false}; // cue was pulled from the queue head
    int pendingFade = -1;
    double pendingDur = 3.0;
    int retireAfterFade = -1; // deck the running fade came from (-1 = silence)
    int lastFreed = -1;       // deck most recently stopped: next track prefers
                              // the OTHER deck (VDJ handover feel)
    double fadeSec = 3.0;
    bool automixOn = true; // off = hard cut at end of track (queue continues)
    bool repeatOn = false; // finished tracks rejoin the queue tail
    MixMode mixMode = MixMode::Fade;
    uint64_t lastFades = 0;
    Clock::time_point lastTick = Clock::now();

    // Library / navigation
    Db db;
    std::wstring dbPath;
    std::wstring search, singerName = L"Guest";
    std::wstring singerFilter; // singers view: filter to one singer
    Focus focus = Focus::None; // None = no box lit; typing goes to the
                               // view's natural box and focuses it
    bool searchDirty = true, navDirty = true;
    NavMode nav = NavMode::Library;
    int64_t navPlaylist = -1;
    std::wstring navPlaylistName, navFolder;
    std::vector<std::pair<std::wstring, int64_t>> playlists;
    std::vector<std::wstring> roots;       // imported scan roots
    std::vector<std::wstring> allDirs;     // every folder containing media
    std::set<std::wstring> expanded;       // folder tree expand state
    std::vector<std::pair<std::wstring, int>> flatFolders; // fallback, no roots
    std::vector<Match> results;
    std::vector<SingerRow> singers;
    int64_t singingItemId = -1;
    std::deque<Match> queue;
    int selLib = -1, selQueue = -1;
    std::set<int> selRows;    // multi-selection (shift/ctrl click) in the browser
    // Browser columns: kColNames order = TITLE ARTIST GENRE YEAR BPM TIME.
    // colSeq = display order (values are column ids), colShow = visibility,
    // colFrac = width shares (normalized over the visible set).
    int colSeq[6] = {0, 1, 2, 3, 4, 5};
    bool colShow[6] = {true, true, true, true, true, true};
    float colFrac[6] = {0.34f, 0.24f, 0.13f, 0.09f, 0.09f, 0.11f};
    int colDrag = -1; // index into the VISIBLE sequence being resized
    float libScroll = 0, queueScroll = 0, sideScroll = 0;
    bool sidebarOpen = true, queueOpen = true;
    float sideW = 220.f; // resizable via the divider next to the sidebar
    bool resizingSide = false;
    float queueW = 330.f; // resizable via the divider left of the queue
    bool resizingQueue = false;
    int scrollGrab = 0;       // scrollbar being dragged (by widget id)
    float scrollGrabOff = 0;  // press offset inside the thumb
    bool hoverResize = false; // cursor is over any resize divider this frame

    // CPU/RAM self-monitor (sampled once a second, shown under the title)
    float cpuPct = 0.f;
    int ramMb = 0;
    uint64_t cpuPrev100ns = 0;
    Clock::time_point cpuAt{};
    int scrubDeck = -1; // deck being waveform-scrubbed
    Clock::time_point lastScrub{};
    int sortCol = 0; // 0 artist 1 title 2 genre 3 year
    bool sortAsc = true;
    std::wstring status = L"ready";

    // Tonight's history (rows recorded when a track goes on air)
    struct HistRow {
        std::wstring when, singer;
        Match song;
    };
    std::vector<HistRow> history;
    std::set<int64_t> playedTonight; // green dot in the browser

    // Auto gain: per-deck trim from measured loudness (setting auto_gain)
    bool autoGainOn = true;
    bool gainSet[2] = {false, false};

    // Settings window (separate top-level window; main.cpp owns it)
    HWND settingsWnd = nullptr;
    bool settingsOpenReq = false; // header button toggles open/close
    bool setFocusTitle = false;   // the waiting-title box has keyboard focus

    // Settings view state
    int videoFit = 0;         // 0 fit (letterbox), 1 fill (crop), 2 stretch
    std::wstring idleTitle;   // waiting-screen headline (idle_title)
    bool scanTags = true;     // read file tags during import (scan_tags)
    std::string audioDevice;  // pinned output endpoint id, "" = default
    std::vector<std::pair<std::wstring, std::wstring>> audioDevs; // id, name

    // Session snapshot (FR-010): last blob written + write throttle.
    std::wstring lastSnap;
    Clock::time_point lastSnapAt{};

    // Import (background scan)
    std::thread scanThread;
    std::atomic<bool> scanning{false};
    std::atomic<bool> scanFinished{false};
    ScanProgress scanProg; // polled into the status line while scanning
    // Folder picker runs on its own STA thread: FileOpenDialog crashes on
    // this process's MTA main thread, and a separate thread keeps the UI and
    // video output alive while the dialog is up.
    std::thread pickThread;
    std::atomic<bool> pickDone{false};
    std::wstring pickResult;

    // Background BPM analysis: fills media_item.bpm for rows the import's
    // tag read couldn't (most karaoke files carry no BPM tag). Runs at OS
    // background priority on its own db connection; restarted after imports.
    std::thread bpmThread;
    std::atomic<bool> bpmStop{false};
    std::atomic<bool> bpmBusy{false};
    std::atomic<bool> bpmFinished{false};
    std::atomic<int> bpmDone{0}, bpmTotal{0};

    // Video outputs
    std::unique_ptr<VideoWindow> fullOut;
    int outMonitor = -1;
    int chosenMonitor = -1;

    // In-app modal: text input (new singer / new playlist) or a themed
    // CONFIRM box (all destructive actions — no native MessageBox).
    enum class Prompt { None, NewSinger, NewPlaylist, Confirm, Columns, Requests };
    enum class ConfirmAction {
        None, RemoveFolder, DeletePlaylist, CleanMissing, ClearRotation,
        ClearHistory
    };
    Prompt prompt = Prompt::None;
    Match rotAddPending; // NewSinger: the track being added
    std::wstring promptText;
    ConfirmAction confirmAction = ConfirmAction::None;
    std::wstring confirmTitle, confirmL1, confirmL2; // two body lines
    std::wstring confirmPath;      // RemoveFolder payload
    int64_t confirmId = 0;         // DeletePlaylist payload
    std::vector<int64_t> confirmIds; // CleanMissing payload

    // Drag & drop
    bool dragArmed = false, dragging = false;
    float dragX0 = 0, dragY0 = 0;
    Match dragItem;
    std::vector<Match> dragItems; // >1 = multi-selection drag (queue-only drop)
    int dragFromQueue = -1; // source row when the drag started in the queue
    int dragFromList = -1;  // source row when dragging inside a playlist view
    int dragSinger = -1;    // source row when reordering the rotation
    D2D1_RECT_F rcDeck[2]{}, rcQueue{}, rcQueueList{}, rcBrowser{}, rcBrowserList{};

    // In-app video previews: presentVideo bumps frameSerial on fresh frames,
    // the mixer panel uploads to the Ui bitmap cache when it lags behind.
    uint64_t frameSerial[2] = {0, 0}, shownSerial[2] = {0, 0};

    // Phone requests (optional embedded LAN server, off by default). When
    // webOn is false the server object is inert — no thread, no socket.
    RequestServer web;
    bool webOn = false;
    std::vector<PhoneRequest> reqInbox; // pending, shown in the REQUESTS modal

    // YouTube download (yt-dlp.exe beside the app or on PATH)
    std::thread ytThread;
    std::atomic<bool> ytBusy{false}, ytDone{false};
    std::wstring ytPath, ytError;

    MenuReq menu;
    float uiScale = 1.f; // physical px per DIP, set from Ui each frame
};

void movePlaylistItem(App& a, int64_t playlistId, int from, int to);
void moveSingerRow(App& a, int from, int to);
void saveSnapshot(App& a);
void performRemoveFolder(App& a, const std::wstring& folder);
void performCleanMissing(App& a);

// Open the themed confirm modal (replaces native MessageBox everywhere).
inline void askConfirm(App& a, App::ConfirmAction action,
                       const std::wstring& title, const std::wstring& l1,
                       const std::wstring& l2) {
    a.prompt = App::Prompt::Confirm;
    a.confirmAction = action;
    a.confirmTitle = title;
    a.confirmL1 = l1;
    a.confirmL2 = l2;
}

inline std::wstring fmtTime(double sec) {
    if (sec < 0) sec = 0;
    wchar_t b[16];
    swprintf(b, 16, L"%d:%02d", int(sec) / 60, int(sec) % 60);
    return b;
}

inline std::wstring leafName(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

// Row tag + color for a media type ("karaoke_zip" shows as green "zip").
inline std::wstring typeTag(const std::string& t) {
    return t == "karaoke_zip" ? L"zip" : wide(t);
}
inline D2D1_COLOR_F typeColor(const std::string& t) {
    return t == "mp3g" || t == "karaoke_zip" ? cGreen : t == "video" ? cB : cA;
}

// ---------------------------------------------------------------------------
// Cross-file API. Definitions: kdj_data (settings/db/import/snapshot/youtube),
// kdj_engine (deck+mixer control, engineTick), kdj_ui (all drawing),
// kdj_menus (context menus + external drops); karaoke_dj_main owns the window.
std::string getSetting(Db& db, const char* key, const std::string& def);
void setSetting(Db& db, const char* key, const std::string& v);
void loadSettings(App& a, UINT& winW, UINT& winH);
void saveSettings(App& a, UINT winW, UINT winH);

bool loadTo(App& a, int d, const Match& m);
void rescueAutoCue(App& a, int d);
void queueSelected(App& a);
void playNow(App& a, const Match& m);
void dropOnDeck(App& a, int d, const Match& m);
void clearDeckSlot(App& a, int d);
void stopDeck(App& a, int d);
void seekFrac(App& a, int d, float frac);
void setSingerStatus(App& a, int64_t itemId, const char* status);
void singNow(App& a, const SingerRow& row);
void addToRotationAs(App& a, const Match& m, const std::wstring& singer);
std::vector<std::wstring> rotationSingers(App& a);
void commitPrompt(App& a);
void presentVideo(App& a);
void engineTick(App& a);

Match readMatch(Db::Stmt& q, int base = 0);
void reloadNav(App& a);
void reloadBrowser(App& a);
Match matchFromPath(App& a, const std::wstring& path);
std::wstring snapshotBlob(App& a);
void restoreSnapshot(App& a);
void startImport(App& a, const std::wstring& folder);
void startBpmAnalysis(App& a);
std::wstring pickFolder(HWND owner);
std::wstring runCapture(const std::wstring& cmd, DWORD& exitCode);
std::wstring youtubeCacheDir();
void startYoutube(App& a, std::wstring url);

void scrollbar(App& a, Ui& ui, int id, const D2D1_RECT_F& list, size_t count,
               float rowH, float& scroll);
void drawRhythm(App& a, Ui& ui, const D2D1_RECT_F& r);
std::vector<std::wstring> folderChildren(const App& a, const std::wstring& dir);
void drawDeck(App& a, Ui& ui, int d, const D2D1_RECT_F& r);
void drawMixer(App& a, Ui& ui, const D2D1_RECT_F& r);
void drawSidebar(App& a, Ui& ui, const D2D1_RECT_F& r);
void cleanMissingFiles(App& a);
void drawSettings(App& a, Ui& ui, const D2D1_RECT_F& r);
void drawBrowser(App& a, Ui& ui, const D2D1_RECT_F& r);
void drawQueue(App& a, Ui& ui, const D2D1_RECT_F& r);
void drawUi(App& a, Ui& ui, float W, float H);

int showMenu(HWND hwnd, float scale, float cx, float cy,
             const std::vector<std::wstring>& items, int checked = -1);
int showTrackMenu(App& a, HWND hwnd, float cx, float cy,
                  const std::vector<std::wstring>& items, size_t subAt,
                  const std::vector<std::wstring>& singers);
void addToPlaylistDb(App& a, int64_t playlistId, const Match& m);
void queueAllPlaylist(App& a, int64_t playlistId);
void removeFolder(App& a, const std::wstring& folder);
void handleMenu(App& a, HWND hwnd);
void dropExternal(App& a, float x, float y, const std::vector<std::wstring>& files);
