#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct IMFSourceReader;

// Decodes any Media Foundation supported audio (WAV/MP3/AAC/M4A/FLAC/WMA)
// to interleaved 32-bit float at the requested rate/channel count.
class MFDecoder {
public:
    static bool initMF(); // once per process, after CoInitializeEx

    bool open(const std::wstring& path, uint32_t sampleRate, uint32_t channels);
    // Appends decoded samples to out. Returns false at end of stream or error.
    // A source may legitimately return "no data this call" (a stream tick);
    // that still returns true, but only kMaxTicks times in a row — see the
    // comment in the .cpp. Every caller loops until this goes false, so the
    // bound is what stops a sick file becoming an infinite loop.
    bool readChunk(std::vector<float>& out);
    uint64_t durationFrames(uint32_t rate) const; // 0 if unknown
    void seekTo(int64_t hns);
    void seekStart() { seekTo(0); }
    void close();
    ~MFDecoder() { close(); }

private:
    static constexpr int kMaxTicks = 1000; // consecutive empty reads before we call it dead
    IMFSourceReader* reader_ = nullptr;
    int ticks_ = 0; // consecutive ReadSample calls that returned no sample
};
