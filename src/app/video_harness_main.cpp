// Two-deck A/V harness (plan §5.2): plays a playlist of media files through
// both decks; each deck's video follows its own audio clock and the master
// video crossfades in step with the audio transition. Audio-only tracks show
// black (the Waiting Screen state comes later).
#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio/wasapi_out.h"
#include "karaoke/cdg_renderer.h"
#include "media/mf_decoder.h"
#include "media/mf_video_decoder.h"
#include "playback/deck.h"
#include "playback/mixer.h"
#include "media/media_paths.h"
#include "video/video_window.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

static constexpr uint32_t kRate = 48000;
static constexpr uint32_t kCh = 2;

int wmain(int argc, wchar_t** argv) {
    std::vector<std::wstring> files;
    double fadeSec = 2.0, intervalSec = 0; // interval 0 = automix (end-of-track)
    int transitions = -1;                  // -1 = play each file once
    std::vector<int> monitors;             // empty = one windowed dev view
    int devWindows = 0;                    // extra dev windows (mirror testing)
    double seconds = 0;                    // safety cap, 0 = none
    double avOffsetMs = 0; // ponytail: constant A/V offset knob; measure per setup
    FadeCurve curve = FadeCurve::EqualPower;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--fade") fadeSec = _wtof(next().c_str());
        else if (a == L"--interval") intervalSec = _wtof(next().c_str());
        else if (a == L"--transitions") transitions = _wtoi(next().c_str());
        else if (a == L"--monitor") monitors.push_back(_wtoi(next().c_str()));
        else if (a == L"--windows") devWindows = _wtoi(next().c_str());
        else if (a == L"--seconds") seconds = _wtof(next().c_str());
        else if (a == L"--avoffset") avOffsetMs = _wtof(next().c_str());
        else if (a == L"--curve") {
            std::wstring c = next();
            if (c == L"linear") curve = FadeCurve::Linear;
            else if (c == L"fastcut") curve = FadeCurve::FastCut;
            else if (c == L"slowblend") curve = FadeCurve::SlowBlend;
        } else if (a[0] != L'-') files.push_back(a);
    }
    if (files.empty()) {
        wprintf(L"usage: video_harness <files...> [--fade s] [--interval s (timer "
                L"instead of end-of-track)] [--transitions n] [--monitor n (repeatable, "
                L"mirrors)] [--windows n] [--seconds cap] [--avoffset ms] [--curve name]\n"
                L"keys: Space pause/resume, Left/Right seek 10s, PgDn/PgUp next/prev "
                L"song, Esc quit\n");
        return 2;
    }
    if (transitions < 0) transitions = int(files.size()) - 1;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!MFDecoder::initMF()) { wprintf(L"MFStartup failed\n"); return 1; }

    Deck deckA(kRate, kCh, 1 << 19), deckB(kRate, kCh, 1 << 19);
    Deck* decks[2] = {&deckA, &deckB};
    Mixer mixer(deckA, deckB);
    WasapiOut out;
    if (!out.start(kRate, kCh, mixer)) { wprintf(L"WASAPI init failed\n"); return 1; }

    // ponytail: mirroring = same frame uploaded to each window's D2D target;
    // switch to one D3D11 shared texture if mirror count or resolution grows.
    std::vector<std::unique_ptr<VideoWindow>> wins;
    if (!monitors.empty()) {
        const int n = VideoWindow::monitorCount();
        for (int m = 0; m < n; ++m) {
            const RECT r = VideoWindow::monitorRect(m);
            wprintf(L"monitor %d: %ldx%ld at (%ld,%ld)\n", m, r.right - r.left,
                    r.bottom - r.top, r.left, r.top);
        }
    }
    for (int m : monitors) {
        auto w = std::make_unique<VideoWindow>();
        if (!w->create(m)) { wprintf(L"cannot open output on monitor %d\n", m); return 1; }
        const RECT r = VideoWindow::monitorRect(m);
        wprintf(L"output -> monitor %d (%ldx%ld at %ld,%ld)\n", m, r.right - r.left,
                r.bottom - r.top, r.left, r.top);
        wins.push_back(std::move(w));
    }
    const int devCount = wins.empty() ? (devWindows > 0 ? devWindows : 1) : devWindows;
    for (int i = 0; i < devCount; ++i) {
        auto w = std::make_unique<VideoWindow>();
        if (!w->create(-1, i)) { wprintf(L"window/D2D init failed\n"); return 1; }
        wins.push_back(std::move(w));
    }

    MFVideoDecoder vdec[2];
    CdgRenderer cdg[2];
    std::unique_ptr<VideoFrame> cur[2];
    bool hasVid[2] = {false, false};
    bool hasCdg[2] = {false, false};

    const auto start = Clock::now();
    bool running = true;

    // Operator keys: Space pause/resume, Left/Right seek 10 s,
    // PageDown/PageUp skip to next/previous song.
    bool skipNext = false, skipPrev = false;
    auto handleKey = [&](int k) {
        if (k == VK_NEXT) { skipNext = true; return; }
        if (k == VK_PRIOR) { skipPrev = true; return; }
        const int a = mixer.activeDeck.load();
        if (a < 0) return;
        if (k == VK_SPACE) {
            decks[a]->paused.store(!decks[a]->paused.load());
        } else if (k == VK_LEFT || k == VK_RIGHT) {
            double t = decks[a]->framesPlayed.load() / double(kRate) +
                       (k == VK_RIGHT ? 10.0 : -10.0);
            const double total = decks[a]->totalFrames.load() / double(kRate);
            if (t < 0) t = 0;
            if (total > 1 && t > total - 1) t = total - 1;
            decks[a]->seek(t);
            if (hasVid[a]) vdec[a].seek(int64_t(t * 10000000.0));
        }
    };

    // One iteration of the video/UI side; called from every wait loop.
    auto tick = [&]() {
        for (auto& w : wins)
            if (!w->pump()) { running = false; return; }
        if (seconds > 0 &&
            Clock::now() - start > std::chrono::duration<double>(seconds)) {
            running = false;
            return;
        }
        for (auto& w : wins)
            if (const int k = w->popKey()) handleKey(k);
        const int64_t off = int64_t(avOffsetMs * 10000.0);
        for (int d = 0; d < 2; ++d) {
            if (hasVid[d] && vdec[d].failed()) hasVid[d] = false; // async open
            const int64_t clk =
                int64_t(decks[d]->framesPlayed.load() * 10000000ull / kRate) + off;
            if (hasVid[d]) {
                if (auto f = vdec[d].popDue(clk)) {
                    cur[d] = std::move(f);
                    for (auto& w : wins) w->setFrame(d, *cur[d]);
                }
            } else if (hasCdg[d]) {
                if (!cur[d]) cur[d] = std::make_unique<VideoFrame>();
                if (cdg[d].renderTo(*cur[d], double(clk) / 10000000.0))
                    for (auto& w : wins) w->setFrame(d, *cur[d]);
            }
        }
        const int ft = mixer.fadeTo.load();
        const float t = mixer.fadeT.load();
        const int a = mixer.activeDeck.load();
        for (auto& w : wins) {
            if (w->bitmapsLost())
                for (int d = 0; d < 2; ++d)
                    if (cur[d]) w->setFrame(d, *cur[d]);
            if (ft >= 0 && t >= 0.f)
                w->draw(1 - ft, ft, t); // outgoing under, incoming fading in on top
            else
                w->draw(a >= 0 && (hasVid[a] || hasCdg[a]) ? a : -1, -1, 0.f);
        }
        std::this_thread::sleep_for(4ms);
    };

    size_t fileIdx = 0;
    int loadFailures = 0;
    auto loadDeck = [&](int d) -> bool {
        std::wstring path = files[fileIdx++ % files.size()];
        vdec[d].close();
        cur[d].reset();
        hasVid[d] = hasCdg[d] = false;

        // MP3+G pairing (shared with the operator app).
        std::wstring audio, cdgPath;
        if (!resolveMedia(path, audio, cdgPath)) {
            wprintf(L"NO AUDIO for %ls\n", path.c_str());
            ++loadFailures;
            return false;
        }
        path = audio;

        if (!decks[d]->load(path, false)) return false;
        while (running && decks[d]->state() == DeckState::Loading) tick();
        if (decks[d]->state() != DeckState::Ready) {
            wprintf(L"LOAD FAILED: %ls\n", path.c_str());
            ++loadFailures;
            decks[d]->stopAndUnload();
            return false;
        }
        if (!cdgPath.empty()) hasCdg[d] = cdg[d].load(cdgPath);
        if (!hasCdg[d]) hasVid[d] = vdec[d].open(path);
        wprintf(L"deck %c <- %ls%ls\n", d ? L'B' : L'A', path.c_str(),
                hasCdg[d] ? L" (CDG)" : hasVid[d] ? L"" : L" (no video)");
        return true;
    };
    auto postFade = [&](int to, double sec) {
        MixCommand c{to, uint64_t(sec * kRate), curve};
        mixer.cmds.push(&c, 1);
    };
    auto waitFadeDone = [&](uint64_t prev) {
        while (running && mixer.fadesCompleted.load() <= prev) tick();
    };

    if (!loadDeck(0)) return 1;
    uint64_t done = mixer.fadesCompleted.load();
    postFade(0, 0.05);
    waitFadeDone(done);
    int active = 0;

    for (int i = 1; i <= transitions && running; ++i) {
        const int idle = 1 - active;
        if (!loadDeck(idle)) continue; // current deck keeps playing (plan §9)
        skipNext = skipPrev = false;   // ponytail: one queued skip at a time
        bool proceed = true;
        const auto until = Clock::now() + std::chrono::duration<double>(intervalSec);
        while (running) { // wait for a transition trigger
            if (skipNext) { skipNext = false; break; }
            if (skipPrev) { // reload idle deck with the song before the current
                skipPrev = false;
                fileIdx = fileIdx >= 3 ? fileIdx - 3 : 0;
                proceed = loadDeck(idle);
                break;
            }
            if (intervalSec > 0) {
                if (Clock::now() >= until) break;
            } else { // automix: trigger when just enough is left to fade
                const double rem = decks[active]->remainingSec();
                if (rem >= 0 && rem <= fadeSec + 0.25) break;
                if (decks[active]->eos.load()) break;
            }
            tick();
        }
        if (!running) break;
        if (!proceed) continue;
        done = mixer.fadesCompleted.load();
        postFade(idle, fadeSec);
        waitFadeDone(done);
        decks[active]->stopAndUnload();
        vdec[active].close();
        cur[active].reset();
        hasVid[active] = hasCdg[active] = false;
        active = idle;
    }
    skipNext = skipPrev = false;
    while (running && !decks[active]->eos.load()) { // play last track out
        if (skipNext) break; // PageDown on the final song ends the set
        if (skipPrev) {      // PageUp on the final song restarts it
            skipPrev = false;
            decks[active]->seek(0);
            if (hasVid[active]) vdec[active].seek(0);
        }
        tick();
    }

    uint64_t shown = 0;
    for (auto& w : wins) shown += w->presented;
    wprintf(L"DONE: framesShown=%llu (x%zu windows) dropped=%llu underruns=%llu "
            L"clipped=%llu loadFailures=%d\n",
            shown, wins.size(), vdec[0].dropped() + vdec[1].dropped(),
            mixer.underruns.load(), mixer.clippedSamples.load(), loadFailures);
    vdec[0].close();
    vdec[1].close();
    out.stop();
    deckA.stopAndUnload();
    deckB.stopAndUnload();
    return (mixer.underruns.load() == 0 && loadFailures == 0) ? 0 : 1;
}
