#pragma once
#include <atomic>
#include <string>
#include <thread>

// One-shot BPM detection for the background library analyzer: decodes a
// ~100 s segment from a quarter of the way in (the caller's thread must have
// COM initialized). Returns 0 when the tempo can't be pinned down.
int analyzeBpm(const std::wstring& path);

// Decodes a whole track on a background thread into kBins peak values for the
// deck waveform strip. Bins become valid left-to-right while scanning; the UI
// polls readyBins() and just draws what exists.
class WaveformScanner {
public:
    static constexpr int kBins = 400;

    ~WaveformScanner() { cancel(); }
    void start(const std::wstring& audioPath); // cancels any previous scan
    void cancel();

    float bin(int i) const { return bins_[i].load(std::memory_order_relaxed); }
    int readyBins() const { return ready_.load(std::memory_order_acquire); }
    // Track loudness: mean RMS of the loudest 20% of bins (the choruses), so
    // quiet intros/outros don't skew it. 0 until the scan finishes.
    float loudness() const { return loud_.load(std::memory_order_acquire); }
    // Detected tempo (rounded BPM), 0 until the scan finishes or if unsure.
    int bpm() const { return bpm_.load(std::memory_order_acquire); }

private:
    std::atomic<float> bins_[kBins]{};
    std::atomic<int> ready_{0};
    std::atomic<float> loud_{0.f};
    std::atomic<int> bpm_{0};
    std::atomic<bool> cancel_{false};
    std::thread th_;
};
