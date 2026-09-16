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
    bool readChunk(std::vector<float>& out);
    uint64_t durationFrames(uint32_t rate) const; // 0 if unknown
    void seekTo(int64_t hns);
    void seekStart() { seekTo(0); }
    void close();
    ~MFDecoder() { close(); }

private:
    IMFSourceReader* reader_ = nullptr;
};
