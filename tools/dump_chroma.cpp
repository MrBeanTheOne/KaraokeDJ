// dump_chroma <files...> — prints one TSV line per track:
//   path <tab> windows <tab> chroma[0..11]
//
// The tuning rig for the key detector. Key profiles must never be swapped by
// feel: dump a few hundred real tracks from the library, then score candidate
// profiles offline against the dump (share of major keys detected, plus the
// handful of tracks whose key you actually know). That is how the shipping
// profiles were chosen — see the comment in src/media/key_detect.cpp.
#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <vector>

#include "media/key_detect.h"
#include "media/mf_decoder.h"

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: dump_chroma <audio files...>  > chroma.tsv\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFDecoder::initMF();
    for (int i = 1; i < argc; ++i) {
        MFDecoder dec;
        if (!dec.open(argv[i], 48000, 2)) {
            fwprintf(stderr, L"open failed: %ls\n", argv[i]);
            continue;
        }
        // Same segment analyzeTrack() uses, so the dump matches what ships.
        const uint64_t total = dec.durationFrames(48000);
        if (total > 48000ull * 130)
            dec.seekTo(int64_t(total / 4 / 48000) * 10000000ll);
        KeyDetector kd;
        std::vector<float> chunk;
        uint64_t got = 0;
        while (got < 48000ull * 100) {
            chunk.clear();
            if (!dec.readChunk(chunk)) break;
            kd.feed(chunk.data(), chunk.size() / 2);
            got += chunk.size() / 2;
        }
        wprintf(L"%ls\t%d", argv[i], kd.windows());
        for (int k = 0; k < 12; ++k) wprintf(L"\t%.6f", kd.chroma()[k]);
        wprintf(L"\n");
        fflush(stdout);
    }
    CoUninitialize();
    return 0;
}
