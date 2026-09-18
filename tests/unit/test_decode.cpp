// Decode robustness: a sick file must never hang the analyser, and the cancel
// flag must be able to cut a run short.
//
// Why this exists: MFDecoder::readChunk used to return true forever when a
// source kept emitting stream ticks (S_OK, no sample, no ENDOFSTREAM). Every
// caller loops "until readChunk goes false", so one malformed file was an
// infinite loop. It stalled a 98k-track library pass at 97812/97813 and then
// hung the whole app on close, because shutdown joins the analyser from the UI
// thread. See decisions.md, 2026-09-18.
//
// Run from the repo root (it reads tests/media-fixtures/), or pass the fixture
// directory as argv[1].
#include <windows.h>
#include <objbase.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "media/mf_decoder.h"
#include "media/waveform.h"

static int failures = 0;

static double timeIt(const std::wstring& p, const std::atomic<bool>* cancel,
                     bool& ok, int& bpm, int& key) {
    const auto t0 = std::chrono::steady_clock::now();
    ok = analyzeTrack(p, bpm, key, cancel);
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// Runs analyzeTrack on its own thread so a genuine hang is reported instead of
// hanging the test too.
static void mustFinish(const std::wstring& p, const char* tag, double limitSec) {
    std::atomic<bool> done{false};
    bool ok = false; int bpm = 0, key = 0; double secs = 0;
    std::thread th([&] { secs = timeIt(p, nullptr, ok, bpm, key); done.store(true); });
    const auto t0 = std::chrono::steady_clock::now();
    while (!done.load() &&
           std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < limitSec)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!done.load()) {
        printf("  FAIL %-22s still running after %.0f s -- HUNG\n", tag, limitSec);
        ++failures;
        th.detach();
        return;
    }
    th.join();
    printf("  ok   %-22s returned %s in %.2f s (bpm=%d key=%d)\n",
           tag, ok ? "true " : "false", secs, bpm, key);
}

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFDecoder::initMF();

    std::wstring dir = L"tests/media-fixtures/";
    if (argc > 1) {
        const std::string a(argv[1]);
        dir.assign(a.begin(), a.end());
        if (!dir.empty() && dir.back() != L'/' && dir.back() != wchar_t(0x5C))
            dir += L'/';
    }
    // Damaged fixtures are generated into the system temp dir, not the repo.
    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring tmp = std::wstring(tempDir) + L"kdj_test_decode/";
    CreateDirectoryW(tmp.c_str(), nullptr);

    printf("1. a healthy file still analyses\n");
    mustFinish(dir + L"tone_a.wav", "tone_a.wav", 30.0);

    printf("\n2. damaged files must return, not spin\n");
    // garbage with a plausible extension
    struct { const wchar_t* name; int kind; } bad[] = {
        {L"empty.wav", 0}, {L"garbage.mp3", 1}, {L"truncated.wav", 2}, {L"header_only.mp4", 3},
    };
    for (auto& b : bad) {
        const std::wstring out = tmp + b.name;
        FILE* f = nullptr;
        _wfopen_s(&f, out.c_str(), L"wb");
        if (!f) continue;
        if (b.kind == 1) { for (int i = 0; i < 40000; ++i) fputc(rand() & 0xff, f); }
        else if (b.kind == 2) {
            FILE* src = nullptr;
            _wfopen_s(&src, (dir + L"tone_a.wav").c_str(), L"rb");
            if (src) { char buf[9000]; size_t n = fread(buf, 1, sizeof buf, src); fwrite(buf, 1, n, f); fclose(src); }
        } else if (b.kind == 3) { const unsigned char h[12] = {0,0,0,0x18,0x66,0x74,0x79,0x70,0x6d,0x70,0x34,0x32}; fwrite(h, 1, 12, f); }
        fclose(f);
        char tag[64]; sprintf_s(tag, "%S", b.name);
        mustFinish(out, tag, 30.0);
    }

    printf("\n3. the cancel flag aborts mid-file\n");
    {
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
        bool ok = true; int bpm = 0, key = 0; double secs = 0;
        std::thread th([&] { secs = timeIt(dir + L"test_video.mp4", &cancel, ok, bpm, key); done.store(true); });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        cancel.store(true);
        const auto t0 = std::chrono::steady_clock::now();
        while (!done.load() &&
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 10.0)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!done.load()) { printf("  FAIL cancel ignored -- still running\n"); ++failures; th.detach(); }
        else {
            th.join();
            const bool good = !ok; // cancelled must report "not analysed"
            printf("  %s cancel honoured: returned %s after %.2f s\n",
                   good ? "ok  " : "FAIL", ok ? "true" : "false", secs);
            if (!good) ++failures;
        }
    }

    for (auto& b : bad) DeleteFileW((tmp + b.name).c_str());
    RemoveDirectoryW(tmp.c_str());

    printf("\n%s\n", failures ? "FAILURES" : "ALL OK");
    return failures ? 1 : 0;
}
