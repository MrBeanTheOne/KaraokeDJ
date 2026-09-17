// Minimal self-checks for the lock-free ring, fade curves, the key-change
// pitch shifter and CDG decoding.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "audio/ring_buffer.h"
#include "karaoke/cdg_renderer.h"
#include "media/mf_video_decoder.h" // VideoFrame
#include "media/key_detect.h"
#include "playback/fade.h"
#include "playback/pitch_shifter.h"

// Dominant frequency of a stereo buffer, by zero crossings of the left channel.
static double zcFreq(const std::vector<float>& x, size_t rate) {
    size_t cross = 0;
    for (size_t f = 1; f < x.size() / 2; ++f)
        if ((x[(f - 1) * 2] < 0.f) != (x[f * 2] < 0.f)) ++cross;
    return double(cross) * double(rate) / (2.0 * double(x.size() / 2));
}

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

    { // key change: pitch moves, length does not
        constexpr size_t kRate = 48000, kBlock = 480, kBlocks = 200; // 2 s
        auto run = [&](int semi) {
            PitchShifter ps;
            ps.setSemitones(semi, 2);
            std::vector<float> tail;
            for (size_t b = 0; b < kBlocks; ++b) {
                std::vector<float> blk(kBlock * 2);
                for (size_t f = 0; f < kBlock; ++f) {
                    const double t = double(b * kBlock + f) / double(kRate);
                    const float v = float(std::sin(2.0 * 3.14159265 * 220.0 * t));
                    blk[f * 2] = blk[f * 2 + 1] = v;
                }
                ps.process(blk.data(), kBlock);
                if (b >= kBlocks / 2) // past the priming delay
                    tail.insert(tail.end(), blk.begin(), blk.end());
            }
            assert(tail.size() == kBlocks / 2 * kBlock * 2); // n in == n out
            return zcFreq(tail, kRate);
        };
        const double f0 = run(0), fUp = run(12), fDn = run(-12);
        assert(std::fabs(f0 - 220.0) < 4.0);   // key 0 is a pure bypass
        assert(std::fabs(fUp - 440.0) < 20.0); // +12 semitones = an octave up
        assert(std::fabs(fDn - 110.0) < 10.0); // -12 = an octave down
    }

    { // key detection: play a diatonic scale + triads, expect the right key
        constexpr size_t kRate = 48000, kBlock = 4800;
        // Feeds `midi` notes (one per second) and returns the detected key.
        auto detect = [&](const std::vector<int>& midi) {
            KeyDetector kd;
            std::vector<float> blk(kBlock * 2);
            size_t n = 0;
            for (int m : midi)
                for (int b = 0; b < 10; ++b) { // 10 blocks = 1 s per note
                    const double f = 440.0 * std::pow(2.0, (m - 69) / 12.0);
                    for (size_t i = 0; i < kBlock; ++i, ++n) {
                        // fundamental + two harmonics, like a real instrument
                        const double t = double(n) / double(kRate);
                        const double v =
                            std::sin(2 * 3.14159265 * f * t) +
                            0.5 * std::sin(4 * 3.14159265 * f * t) +
                            0.25 * std::sin(6 * 3.14159265 * f * t);
                        blk[i * 2] = blk[i * 2 + 1] = float(v * 0.25);
                    }
                    kd.feed(blk.data(), kBlock);
                }
            return kd.result();
        };
        // C major scale, resolving on C (MIDI 60 = C4).
        const int cMaj = detect({60, 62, 64, 65, 67, 69, 71, 72, 67, 64, 60,
                                 60, 64, 67, 72, 67, 64, 60, 65, 62, 60, 60});
        assert(cMaj == 0); // tonic C, major
        // Same notes centred on A: the natural minor of the same scale.
        const int aMin = detect({57, 59, 60, 62, 64, 65, 67, 69, 64, 60, 57,
                                 57, 60, 64, 69, 64, 60, 57, 62, 59, 57, 57});
        assert(aMin == 9 + 12); // tonic A, minor
        assert(keyName(keyToDb(cMaj), 0) == L"C");
        assert(keyName(keyToDb(aMin), 0) == L"Am");
        assert(keyName(keyToDb(aMin), 2) == L"Bm");   // +2 semitones
        assert(keyName(keyToDb(aMin), -1) == L"G#m"); // -1 semitone
        assert(keyName(keyToDb(cMaj), 12) == L"C");   // whole octave
        assert(keyName(0, 3).empty());                // never analysed
        assert(keyName(-1, 3).empty());               // no clear key
    }

    printf("test_core OK\n");
    return 0;
}
