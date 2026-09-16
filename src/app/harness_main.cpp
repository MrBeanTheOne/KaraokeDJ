// Headless two-deck playback harness (plan §17): loads files round-robin,
// runs Play Now transitions with a sample-based crossfade, and reports
// underruns, clipping and memory so long endurance runs can be validated
// before any UI exists.
#include <windows.h>
#include <objbase.h>
#include <psapi.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "audio/wasapi_out.h"
#include "media/mf_decoder.h"
#include "playback/deck.h"
#include "playback/mixer.h"

using namespace std::chrono_literals;

static constexpr uint32_t kRate = 48000;
static constexpr uint32_t kCh = 2;

static double memMB() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return double(pmc.WorkingSetSize) / (1024.0 * 1024.0);
}

static bool waitReady(Deck& d, int timeoutMs = 10000) {
    for (int t = 0; t < timeoutMs; t += 10) {
        if (d.state() == DeckState::Ready) return true;
        if (d.state() == DeckState::Error) return false;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

int wmain(int argc, wchar_t** argv) {
    std::vector<std::wstring> files;
    double fadeSec = 2.0, intervalSec = 5.0;
    int transitions = 20;
    bool automix = false; // transition when the active track nears its end
    bool testPauseSeek = false; // scripted FR-001 transport check
    FadeCurve curve = FadeCurve::EqualPower;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--fade") fadeSec = _wtof(next().c_str());
        else if (a == L"--interval") intervalSec = _wtof(next().c_str());
        else if (a == L"--transitions") transitions = _wtoi(next().c_str());
        else if (a == L"--automix") automix = true;
        else if (a == L"--test-pause-seek") testPauseSeek = true;
        else if (a == L"--curve") {
            std::wstring c = next();
            if (c == L"linear") curve = FadeCurve::Linear;
            else if (c == L"fastcut") curve = FadeCurve::FastCut;
            else if (c == L"slowblend") curve = FadeCurve::SlowBlend;
        } else if (a[0] != L'-') files.push_back(a);
    }
    if (files.size() < 2) {
        wprintf(L"usage: harness <file1> <file2> [...] [--fade s] [--interval s] "
                L"[--transitions n] [--automix] [--curve equalpower|linear|fastcut|slowblend]\n");
        return 2;
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!MFDecoder::initMF()) { wprintf(L"MFStartup failed\n"); return 1; }

    Deck deckA(kRate, kCh, 1 << 19), deckB(kRate, kCh, 1 << 19); // ~5.5 s per ring
    Deck* decks[2] = {&deckA, &deckB};
    Mixer mixer(deckA, deckB);
    WasapiOut out;
    if (!out.start(kRate, kCh, mixer)) { wprintf(L"WASAPI init failed\n"); return 1; }

    size_t fileIdx = 0;
    int loadFailures = 0;
    auto loadNext = [&](int d) -> bool {
        const std::wstring& path = files[fileIdx++ % files.size()];
        if (!decks[d]->load(path, /*loopFile=*/!automix) || !waitReady(*decks[d])) {
            wprintf(L"LOAD FAILED: %ls\n", path.c_str());
            ++loadFailures;
            decks[d]->stopAndUnload();
            return false;
        }
        return true;
    };
    auto postFade = [&](int to, double sec) {
        MixCommand c{to, uint64_t(sec * kRate), curve};
        mixer.cmds.push(&c, 1);
    };
    auto waitFadeDone = [&](uint64_t prev, double sec) -> bool {
        const int timeoutMs = int(sec * 1000) + 5000;
        for (int t = 0; t < timeoutMs; t += 10) {
            if (mixer.fadesCompleted.load() > prev) return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    };

    if (testPauseSeek) { // scripted FR-001 transport check
        if (!loadNext(0)) return 1;
        const uint64_t prev = mixer.fadesCompleted.load();
        postFade(0, 0.05);
        waitFadeDone(prev, 0.05);
        std::this_thread::sleep_for(1000ms);
        Deck& d = *decks[0];
        d.paused.store(true);
        std::this_thread::sleep_for(100ms);
        const uint64_t f1 = d.framesPlayed.load();
        std::this_thread::sleep_for(300ms);
        const uint64_t f2 = d.framesPlayed.load();
        const bool pauseOk = f1 > 0 && f1 == f2;
        d.paused.store(false);
        std::this_thread::sleep_for(300ms);
        const bool resumeOk = d.framesPlayed.load() > f2;
        d.seek(8.0);
        std::this_thread::sleep_for(200ms);
        const double pos = d.framesPlayed.load() / double(kRate);
        const bool seekOk = pos >= 8.0 && pos < 9.5;
        wprintf(L"pause=%ls resume=%ls seek=%ls (pos=%.2fs) underruns=%llu\n",
                pauseOk ? L"OK" : L"FAIL", resumeOk ? L"OK" : L"FAIL",
                seekOk ? L"OK" : L"FAIL", pos, mixer.underruns.load());
        out.stop();
        deckA.stopAndUnload();
        deckB.stopAndUnload();
        return (pauseOk && resumeOk && seekOk && mixer.underruns.load() == 0) ? 0 : 1;
    }

    // Start deck 0 with a near-instant fade-in from silence.
    if (!loadNext(0)) return 1;
    uint64_t done = mixer.fadesCompleted.load();
    postFade(0, 0.05);
    waitFadeDone(done, 0.05);
    int active = 0;

    wprintf(L"Running %d transitions, fade %.2fs, interval %.2fs\n",
            transitions, fadeSec, intervalSec);
    for (int i = 1; i <= transitions; ++i) {
        const int idle = 1 - active;
        if (!loadNext(idle)) continue; // failed target leaves current deck playing (plan §9)
        if (automix) {
            // Wait until the active track has just enough left for the fade.
            while (true) {
                const double rem = decks[active]->remainingSec();
                if (rem >= 0 && rem <= fadeSec + 0.25) break;
                if (decks[active]->eos.load()) break;
                std::this_thread::sleep_for(20ms);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(int(intervalSec * 1000)));
        }
        done = mixer.fadesCompleted.load();
        postFade(idle, fadeSec);
        if (!waitFadeDone(done, fadeSec)) {
            wprintf(L"FADE TIMEOUT at transition %d\n", i);
            break;
        }
        decks[active]->stopAndUnload();
        active = idle;
        if (i % 10 == 0 || i == transitions) {
            wprintf(L"[%4d/%d] underruns=%llu clipped=%llu mem=%.1fMB\n", i, transitions,
                    mixer.underruns.load(), mixer.clippedSamples.load(), memMB());
            fflush(stdout); // stats visible live when redirected to a log
        }
    }

    const uint64_t under = mixer.underruns.load();
    wprintf(L"DONE: transitions=%llu underruns=%llu clipped=%llu loadFailures=%d mem=%.1fMB\n",
            mixer.fadesCompleted.load() - 1, under, mixer.clippedSamples.load(),
            loadFailures, memMB());
    out.stop();
    deckA.stopAndUnload();
    deckB.stopAndUnload();
    return (under == 0 && loadFailures == 0) ? 0 : 1;
}
