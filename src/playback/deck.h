#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "audio/ring_buffer.h"

enum class DeckState { Empty, Loading, Ready, Error };

// One playback deck: a decode worker thread fills a bounded ring buffer;
// the audio thread pulls from it via pull(). State machine per plan §5.1
// (Playing is tracked by the `playing` flag the mixer owns, not DeckState).
class Deck {
public:
    Deck(uint32_t rate, uint32_t channels, size_t ringCapacityPow2)
        : ring(ringCapacityPow2), rate_(rate), ch_(channels),
          preload_(size_t(rate) * channels / 2) {} // 0.5 s preload before Ready
    ~Deck() { stopAndUnload(); }

    // Async: state goes Loading -> Ready once preloaded (or Error).
    bool load(const std::wstring& path, bool loopFile);
    void stopAndUnload();
    // Reposition playback (control thread). Blocks briefly (<0.6 s worst case)
    // while the decode worker drops buffered audio and reseeks.
    void seek(double sec);

    DeckState state() const { return state_.load(std::memory_order_acquire); }

    // Audio thread only.
    size_t pull(float* dst, size_t n) {
        const size_t got = ring.pop(dst, n);
        framesPlayed.fetch_add(got / ch_, std::memory_order_relaxed);
        return got;
    }

    // Seconds of audio left to play; -1 if duration unknown (e.g. looping).
    double remainingSec() const {
        const uint64_t total = totalFrames.load(std::memory_order_relaxed);
        if (!total) return -1.0;
        const uint64_t played = framesPlayed.load(std::memory_order_relaxed);
        return played >= total ? 0.0 : double(total - played) / rate_;
    }

    std::atomic<uint64_t> framesPlayed{0};
    std::atomic<uint64_t> totalFrames{0}; // 0 = unknown or looping

    std::atomic<bool> playing{false};    // set/cleared by the mixer (audio thread)
    std::atomic<bool> paused{false};     // operator pause: mixer outputs silence
    std::atomic<bool> decodeDone{false}; // no more data coming (file fully decoded)
    std::atomic<bool> eos{false};        // decodeDone and ring drained: playback over
    SpscRing<float> ring;

private:
    void workerMain(std::wstring path, bool loopFile);

    uint32_t rate_, ch_;
    size_t preload_;
    std::atomic<DeckState> state_{DeckState::Empty};
    std::atomic<bool> quit_{false};
    std::atomic<int64_t> pendingSeekHns_{-1};
    std::thread worker_;
};
