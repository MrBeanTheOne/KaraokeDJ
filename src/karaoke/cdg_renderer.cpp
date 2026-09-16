#include "karaoke/cdg_renderer.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "media/mf_video_decoder.h" // VideoFrame

bool CdgRenderer::load(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    if (d.size() < 24) return false;
    loadFromMemory(std::move(d));
    return true;
}

void CdgRenderer::loadFromMemory(std::vector<uint8_t> data) {
    data_ = std::move(data);
    reset();
}

void CdgRenderer::reset() {
    pos_ = 0;
    std::memset(px_, 0, sizeof px_);
    std::memset(pal_, 0, sizeof pal_);
    dirty_ = true;
}

bool CdgRenderer::renderTo(VideoFrame& f, double timeSec, bool force) {
    const size_t total = data_.size() / 24;
    size_t target = timeSec <= 0 ? 0 : size_t(timeSec * 300.0);
    target = std::min(target, total);
    if (target < pos_) reset(); // rewind: replay from the start
    while (pos_ < target) {
        applyPacket(&data_[pos_ * 24]);
        ++pos_;
    }
    if (!dirty_ && !force) return false;
    f.width = kW;
    f.height = kH;
    f.ptsHns = int64_t(timeSec * 10000000.0);
    f.bgra.resize(size_t(kW) * kH * 4);
    uint32_t* dst = reinterpret_cast<uint32_t*>(f.bgra.data());
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x) *dst++ = pal_[px_[y][x]];
    dirty_ = false;
    return true;
}

void CdgRenderer::applyPacket(const uint8_t* p) {
    if ((p[0] & 0x3F) != 9) return; // not a CDG graphics packet
    const uint8_t* d = p + 4;       // 16 data bytes, low 6 bits significant
    switch (p[1] & 0x3F) {
    case 1: // Memory Preset (repeat>0 are redundancy copies)
        if ((d[1] & 0x0F) == 0) {
            std::memset(px_, d[0] & 0x0F, sizeof px_);
            dirty_ = true;
        }
        break;
    case 2: { // Border Preset: everything outside the 6..293 x 12..203 area
        const uint8_t c = d[0] & 0x0F;
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x)
                if (y < 12 || y >= 204 || x < 6 || x >= 294) px_[y][x] = c;
        dirty_ = true;
        break;
    }
    case 6:  tile(d, false); break;
    case 38: tile(d, true); break;
    case 20: scroll(d, true); break;
    case 24: scroll(d, false); break;
    case 28: break; // transparent color: only relevant for video overlay
    case 30: loadColors(d, 0); break;
    case 31: loadColors(d, 8); break;
    default: break;
    }
}

void CdgRenderer::tile(const uint8_t* d, bool xorMode) {
    const uint8_t c0 = d[0] & 0x0F, c1 = d[1] & 0x0F;
    const uint32_t row = (d[2] & 0x1F) * 12, col = (d[3] & 0x3F) * 6;
    if (row + 12 > kH || col + 6 > kW) return;
    for (int i = 0; i < 12; ++i) {
        const uint8_t bits = d[4 + i] & 0x3F;
        for (int j = 0; j < 6; ++j) {
            const uint8_t c = (bits >> (5 - j)) & 1 ? c1 : c0;
            uint8_t& q = px_[row + i][col + j];
            q = xorMode ? (q ^ c) : c;
        }
    }
    dirty_ = true;
}

void CdgRenderer::scroll(const uint8_t* d, bool preset) {
    // ponytail: block scroll only (6/12 px steps); the smooth pixel offsets
    // some discs use for scrolling text are ignored until one visibly needs it.
    const uint8_t color = d[0] & 0x0F;
    const int hc = (d[1] & 0x30) >> 4, vc = (d[2] & 0x30) >> 4;
    const int dx = hc == 1 ? 6 : hc == 2 ? -6 : 0;
    const int dy = vc == 1 ? 12 : vc == 2 ? -12 : 0;
    if (!dx && !dy) return;
    uint8_t old[kH][kW];
    std::memcpy(old, px_, sizeof old);
    for (int y = 0; y < int(kH); ++y) {
        for (int x = 0; x < int(kW); ++x) {
            const int sx = x - dx, sy = y - dy;
            if (sx >= 0 && sx < int(kW) && sy >= 0 && sy < int(kH))
                px_[y][x] = old[sy][sx];
            else if (preset)
                px_[y][x] = color;
            else // copy: wrap around
                px_[y][x] = old[(sy + kH) % kH][(sx + kW) % kW];
        }
    }
    dirty_ = true;
}

void CdgRenderer::loadColors(const uint8_t* d, int base) {
    for (int i = 0; i < 8; ++i) {
        const uint8_t b0 = d[i * 2] & 0x3F, b1 = d[i * 2 + 1] & 0x3F;
        const uint32_t r = (b0 >> 2) & 0x0F;
        const uint32_t g = ((b0 & 0x03) << 2) | ((b1 >> 4) & 0x03);
        const uint32_t b = b1 & 0x0F;
        pal_[base + i] = 0xFF000000u | (r * 17) << 16 | (g * 17) << 8 | (b * 17);
    }
    dirty_ = true;
}
