#pragma once
#include <atomic>
#include <string>
#include <thread>

// One-shot tempo + musical key detection for the background library analyzer:
// decodes a ~100 s segment from a quarter of the way in, once, and measures
// both from it (the caller's thread must have COM initialized). bpm comes back
// 0 when the tempo can't be pinned down, key -1 when there is no clear key.
//
// Returns FALSE when the file could not be decoded at all — an unplugged
// drive, a missing codec. Callers must leave the stored values alone in that
// case: writing a "nothing found" result would permanently retire a track
// that was only temporarily unreachable.
// Pass a cancel flag to abort mid-file: the analyser is joined from the UI
// thread on shutdown, so a single slow track must not be able to hold the
// close. Cancelled counts as "not analysed" (returns false), which is what we
// want -- the row is left for the next launch.
bool analyzeTrack(const std::wstring& path, int& bpm, int& key,
                  const std::atomic<bool>* cancel = nullptr);

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
    // Detected musical key in KeyDetector terms, -1 until the scan finishes
    // or when the track has no clear key.
    int musicKey() const { return key_.load(std::memory_order_acquire); }

private:
    std::atomic<float> bins_[kBins]{};
    std::atomic<int> ready_{0};
    std::atomic<float> loud_{0.f};
    std::atomic<int> bpm_{0};
    std::atomic<int> key_{-1};
    std::atomic<bool> cancel_{false};
    std::thread th_;
};
