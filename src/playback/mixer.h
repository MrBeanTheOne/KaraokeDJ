#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

#include "audio/ring_buffer.h"
#include "playback/fade.h"

class Deck;

// Posted by the control thread; consumed by the audio thread. The fade is
// computed sample-based inside render() (plan §5.3), never from UI timers.
struct MixCommand {
    int toDeck = 0;
    uint64_t durSamples = 1; // in frames
    FadeCurve curve = FadeCurve::EqualPower;
};

class Mixer {
public:
    Mixer(Deck& a, Deck& b) : decks_{&a, &b} {}

    void prepare(size_t maxSamples); // call before render() with max buffer size
    void render(float* out, uint32_t frames, uint32_t channels); // audio thread only

    // Operator controls (FR-002), set from any thread, applied per buffer.
    std::atomic<float> deckGain[2]{1.f, 1.f};
    std::atomic<float> crossfader{0.5f}; // 0 = deck A, 1 = deck B, center = both unity
    // Auto-level trim per deck (from the track's measured loudness), applied
    // under the operator's gain so the sliders keep their meaning.
    std::atomic<float> autoGain[2]{1.f, 1.f};

    // Per-buffer peak of each deck's decoded content (pre-gain), for silence
    // detection ("smart" automix) and level meters.
    std::atomic<float> deckPeak[2]{0.f, 0.f};

    SpscRing<MixCommand> cmds{16};
    std::atomic<uint64_t> fadesCompleted{0};
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> clippedSamples{0};
    std::atomic<int> activeDeck{-1};
    // Published fade state so the video path can mirror the audio transition.
    std::atomic<int> fadeTo{-1};    // incoming deck, -1 when no fade running
    std::atomic<float> fadeT{-1.f}; // progress 0..1, -1 when no fade running

private:
    Deck* decks_[2];
    std::vector<float> scratch_[2];
    float gain_[2] = {0.f, 0.f};
    struct {
        bool active = false;
        uint64_t pos = 0, dur = 1;
        int to = 0;
        FadeCurve curve = FadeCurve::EqualPower;
    } fade_;
};
