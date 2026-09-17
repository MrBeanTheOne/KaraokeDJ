#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// Key change: shifts pitch by whole semitones while the TEMPO is untouched —
// the same number of frames go out as came in, so the deck's frame clock (and
// therefore the CDG / video sync built on it) keeps working unchanged.
//
// WSOLA time-stretch by 2^(n/12) followed by a linear-interpolating resample
// by the same factor: the two cancel in length and leave only the pitch.
//
// Weighs nothing until it is used. At key 0 no buffer is allocated and
// process() returns after one relaxed load. The first non-zero key allocates
// on the CONTROL thread (setSemitones); the audio thread never allocates,
// locks or touches the filesystem.
class PitchShifter {
public:
    static constexpr int kMaxSemitones = 12;

    // Control thread. Clamped to +-kMaxSemitones. `channels` is taken on the
    // first arming call only (the whole engine is fixed at float/48k/stereo,
    // see decisions.md). Safe to call while the audio thread is running.
    void setSemitones(int semi, uint32_t channels);
    int semitones() const { return semi_.load(std::memory_order_relaxed); }

    // Control thread: drop the internal delay line after a load or a seek, so
    // the next buffers aren't stitched onto audio from before the jump.
    void reset() { resetReq_.store(true, std::memory_order_relaxed); }

    // Audio thread. In place, `frames` in -> `frames` out. No-op at key 0.
    void process(float* io, size_t frames);

private:
    // 1024-frame Hann window at 50 % overlap (sums to unity); the +-512-frame
    // WSOLA search covers one period down to ~94 Hz. Latency is the history
    // the search needs, 2*kSeek + kWin ~= 43 ms — inaudible against lyrics,
    // and only paid while a key is actually dialled in.
    static constexpr size_t kWin = 1024;    // analysis/synthesis window
    static constexpr size_t kHop = kWin / 2; // synthesis hop
    static constexpr size_t kCorr = 256;    // frames compared in the search
    static constexpr size_t kSeek = 512;    // +- search range
    static constexpr size_t kInCap = 8192;  // input FIFO (frames)
    static constexpr size_t kOutCap = 8192; // stretched FIFO (frames)
    static constexpr size_t kMaxBlock = 2048; // biggest render buffer handled

    bool hop(float ratio); // one synthesis hop; false if it needs more input
    void clearState();

    std::atomic<int> semi_{0};
    std::atomic<float> ratio_{1.f};
    std::atomic<bool> ready_{false}; // release-stores the buffers below
    std::atomic<bool> resetReq_{false};

    uint32_t ch_ = 2;
    std::vector<float> in_;   // input FIFO, interleaved
    std::vector<float> inM_;  // ...mono downmix, for the correlation search
    size_t inFill_ = 0;       // frames held in in_
    uint64_t inBase_ = 0;     // absolute frame index of in_[0]
    std::vector<float> ola_;  // overlap-add accumulator, kWin frames
    std::vector<float> outF_; // stretched output FIFO
    size_t outFill_ = 0;
    std::vector<float> win_;  // Hann
    double anaPos_ = 0;       // nominal analysis position (absolute frames)
    uint64_t tmplPos_ = 0;    // where the previous segment naturally continues
    double resPos_ = 0;       // fractional read position in outF_
};
