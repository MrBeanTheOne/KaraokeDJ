// Minimal self-checks for the lock-free ring, fade curves and CDG decoding.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "audio/ring_buffer.h"
#include "karaoke/cdg_renderer.h"
#include "media/mf_video_decoder.h" // VideoFrame
#include "playback/fade.h"

static std::vector<uint8_t> cdgPacket(uint8_t instruction,
                                      const uint8_t (&data)[16]) {
    std::vector<uint8_t> p(24, 0);
    p[0] = 9;
    p[1] = instruction;
    std::memcpy(&p[4], data, 16);
    return p;
}

int main() {
    // Ring: fill, drain, wraparound, bounded push.
    SpscRing<float> r(8);
    float in[6] = {1, 2, 3, 4, 5, 6}, outb[8];
    assert(r.push(in, 6) == 6);
    assert(r.size() == 6);
    assert(r.pop(outb, 4) == 4 && outb[0] == 1 && outb[3] == 4);
    assert(r.push(in, 6) == 6); // wraps; 2 + 6 = 8 = capacity
    assert(r.push(in, 1) == 0); // full: bounded, no overwrite
    assert(r.pop(outb, 8) == 8 && outb[0] == 5 && outb[2] == 1 && outb[7] == 6);
    assert(r.size() == 0);
    r.push(in, 3);
    r.clear();
    assert(r.size() == 0);

    // Fade curves: endpoints and equal-power midpoint.
    for (auto c : {FadeCurve::EqualPower, FadeCurve::Linear, FadeCurve::FastCut,
                   FadeCurve::SlowBlend}) {
        float gin, gout;
        fadeGains(c, 0.f, gin, gout);
        assert(gin == 0.f && std::fabs(gout - 1.f) < 1e-6f);
        fadeGains(c, 1.f, gin, gout);
        assert(std::fabs(gin - 1.f) < 1e-6f && gout < 1e-6f);
        fadeGains(c, 1.5f, gin, gout); // clamps
        assert(std::fabs(gin - 1.f) < 1e-6f);
    }
    float gin, gout;
    fadeGains(FadeCurve::EqualPower, 0.5f, gin, gout);
    assert(std::fabs(gin - 0.70710678f) < 1e-4f && std::fabs(gout - 0.70710678f) < 1e-4f);
    assert(std::fabs(gin * gin + gout * gout - 1.f) < 1e-4f); // constant power

    // Crossfader law: ends isolate one deck, center leaves both at unity.
    float gA, gB;
    xfadeGains(0.f, gA, gB);
    assert(gA == 1.f && gB == 0.f);
    xfadeGains(1.f, gA, gB);
    assert(gA == 0.f && gB == 1.f);
    xfadeGains(0.5f, gA, gB);
    assert(gA == 1.f && gB == 1.f);
    xfadeGains(0.25f, gA, gB);
    assert(gA == 1.f && std::fabs(gB - 0.5f) < 1e-6f);

    { // CDG: load palette, memory preset, one tile; verify blitted pixels.
        std::vector<uint8_t> stream;
        // Color table low: color0 = black, color1 = white (r=g=b=15).
        uint8_t pal[16] = {};
        pal[2] = 0x3F; // color1 byte0: 00rrrrgg = 111111
        pal[3] = 0x3F; // color1 byte1: 00ggbbbb = 111111
        for (auto b : cdgPacket(30, pal)) stream.push_back(b);
        // Memory preset to color 1 (white screen).
        uint8_t mp[16] = {1, 0};
        for (auto b : cdgPacket(1, mp)) stream.push_back(b);
        // Tile at row 2, col 3: color0=1, color1=0, top pixel row = 100000.
        uint8_t tl[16] = {1, 0, 2, 3, 0x20};
        for (auto b : cdgPacket(6, tl)) stream.push_back(b);

        CdgRenderer r;
        r.loadFromMemory(stream);
        VideoFrame f;
        assert(r.renderTo(f, 3.0 / 300.0)); // all three packets due
        assert(f.width == CdgRenderer::kW && f.height == CdgRenderer::kH);
        auto pxAt = [&](uint32_t x, uint32_t y) {
            return reinterpret_cast<const uint32_t*>(f.bgra.data())[y * f.width + x];
        };
        assert(pxAt(0, 0) == 0xFFFFFFFFu);        // white background
        assert(pxAt(3 * 6, 2 * 12) == 0xFF000000u); // tile bit set -> color1=black
        assert(pxAt(3 * 6 + 1, 2 * 12) == 0xFFFFFFFFu); // bit clear -> color0=white
        assert(!r.renderTo(f, 3.0 / 300.0)); // no new packets -> unchanged
        assert(r.renderTo(f, 1.0 / 300.0)); // rewind replays and redraws
        assert(pxAt(0, 0) == 0xFF000000u);  // only palette applied: still black
    }

    printf("test_core OK\n");
    return 0;
}
