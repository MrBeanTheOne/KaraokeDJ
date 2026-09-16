#include "playback/mixer.h"

#include <cstring>

#include "playback/deck.h"

void Mixer::prepare(size_t maxSamples) {
    scratch_[0].resize(maxSamples);
    scratch_[1].resize(maxSamples);
}

void Mixer::render(float* out, uint32_t frames, uint32_t ch) {
    const size_t n = size_t(frames) * ch;
    if (scratch_[0].size() < n) { // prepare() not called big enough
        std::memset(out, 0, n * sizeof(float));
        return;
    }

    MixCommand c;
    while (cmds.pop(&c, 1)) {
        fade_.active = true;
        fade_.pos = 0;
        fade_.dur = c.durSamples ? c.durSamples : 1;
        fade_.to = c.toDeck;
        fade_.curve = c.curve;
        decks_[c.toDeck]->playing.store(true, std::memory_order_release);
    }

    for (int d = 0; d < 2; ++d) {
        float* buf = scratch_[d].data();
        if (decks_[d]->playing.load(std::memory_order_relaxed) &&
            !decks_[d]->paused.load(std::memory_order_relaxed)) {
            const size_t got = decks_[d]->pull(buf, n);
            if (got < n) {
                std::memset(buf + got, 0, (n - got) * sizeof(float));
                // Draining the tail of a fully decoded track is not starvation.
                if (!decks_[d]->decodeDone.load(std::memory_order_relaxed))
                    underruns.fetch_add(1, std::memory_order_relaxed);
            }
            float pk = 0.f;
            for (size_t i = 0; i < n; ++i) {
                const float v = buf[i] < 0 ? -buf[i] : buf[i];
                if (v > pk) pk = v;
            }
            deckPeak[d].store(pk, std::memory_order_relaxed);
        } else {
            std::memset(buf, 0, n * sizeof(float));
            deckPeak[d].store(0.f, std::memory_order_relaxed);
        }
    }

    float xgA, xgB;
    xfadeGains(crossfader.load(std::memory_order_relaxed), xgA, xgB);
    const float ug[2] = {deckGain[0].load(std::memory_order_relaxed) * xgA *
                             autoGain[0].load(std::memory_order_relaxed),
                         deckGain[1].load(std::memory_order_relaxed) * xgB *
                             autoGain[1].load(std::memory_order_relaxed)};

    for (uint32_t f = 0; f < frames; ++f) {
        if (fade_.active) {
            const float t = float(fade_.pos) / float(fade_.dur);
            float gin, gout;
            fadeGains(fade_.curve, t, gin, gout);
            gain_[fade_.to] = gin;
            gain_[1 - fade_.to] = gout;
            if (++fade_.pos >= fade_.dur) {
                fade_.active = false;
                gain_[fade_.to] = 1.f;
                gain_[1 - fade_.to] = 0.f;
                decks_[1 - fade_.to]->playing.store(false, std::memory_order_release);
                activeDeck.store(fade_.to, std::memory_order_release);
                fadesCompleted.fetch_add(1, std::memory_order_release);
            }
        }
        if (f == frames - 1) { // publish fade state once per buffer
            if (fade_.active) {
                fadeTo.store(fade_.to, std::memory_order_relaxed);
                fadeT.store(float(fade_.pos) / float(fade_.dur), std::memory_order_relaxed);
            } else {
                fadeTo.store(-1, std::memory_order_relaxed);
                fadeT.store(-1.f, std::memory_order_relaxed);
            }
        }
        for (uint32_t k = 0; k < ch; ++k) {
            const size_t i = size_t(f) * ch + k;
            float s = scratch_[0][i] * gain_[0] * ug[0] + scratch_[1][i] * gain_[1] * ug[1];
            // ponytail: hard-clamp limiter; upgrade to lookahead limiter if clipping is ever audible
            if (s > 1.f) { s = 1.f; clippedSamples.fetch_add(1, std::memory_order_relaxed); }
            else if (s < -1.f) { s = -1.f; clippedSamples.fetch_add(1, std::memory_order_relaxed); }
            out[i] = s;
        }
    }
}
