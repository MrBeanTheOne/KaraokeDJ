#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct VideoFrame;

// CD+G graphics decoder: 300x216 indexed screen, 16-color palette, 24-byte
// packets at 300/sec. renderTo() replays packets up to the given song time
// and blits into a BGRA VideoFrame. Rewind = replay from zero (a full song
// is ~60k packets touching <=72 px each — microseconds, so no snapshots).
class CdgRenderer {
public:
    static constexpr uint32_t kW = 300, kH = 216;

    bool load(const std::wstring& path);
    void loadFromMemory(std::vector<uint8_t> data); // also used by tests

    // Advance to timeSec; fills frame and returns true if content changed
    // (or force). The frame is left untouched when nothing changed.
    bool renderTo(VideoFrame& frame, double timeSec, bool force = false);

private:
    void reset();
    void applyPacket(const uint8_t* p);
    void tile(const uint8_t* d, bool xorMode);
    void scroll(const uint8_t* d, bool preset);
    void loadColors(const uint8_t* d, int base);

    std::vector<uint8_t> data_;
    size_t pos_ = 0; // next packet index
    uint8_t px_[kH][kW] = {};
    uint32_t pal_[16] = {};
    bool dirty_ = false;
};
