#include "playback/pitch_shifter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

void PitchShifter::setSemitones(int semi, uint32_t channels) {
    semi = std::clamp(semi, -kMaxSemitones, kMaxSemitones);
    if (semi != 0 && in_.empty()) {
        // First use. Everything is allocated here, on the control thread; the
        // audio thread is still bypassing on !ready_ while this runs.
        ch_ = channels ? channels : 2;
        in_.assign(kInCap * ch_, 0.f);
        inM_.assign(kInCap, 0.f);
        ola_.assign(kWin * ch_, 0.f);
        outF_.assign(kOutCap * ch_, 0.f);
        win_.resize(kWin);
        for (size_t i = 0; i < kWin; ++i)
            win_[i] = 0.5f - 0.5f * std::cos(6.2831853f * float(i) / float(kWin));
        clearState();
        ratio_.store(std::pow(2.f, float(semi) / 12.f), std::memory_order_relaxed);
        semi_.store(semi, std::memory_order_relaxed);
        ready_.store(true, std::memory_order_release); // publishes the buffers
        return;
    }
    // Coming back from 0, the mixer stopped calling process() entirely, so
    // the delay line holds audio from before the gap — resuming the overlap-add
    // against it clicks. Start clean instead.
    if (semi != 0 && semi_.load(std::memory_order_relaxed) == 0) reset();
    ratio_.store(std::pow(2.f, float(semi) / 12.f), std::memory_order_relaxed);
    semi_.store(semi, std::memory_order_relaxed);
}

void PitchShifter::clearState() {
    inFill_ = 0;
    inBase_ = 0;
    outFill_ = 0;
    resPos_ = 0;
    anaPos_ = double(kSeek); // leave room to search backwards from frame 0
    tmplPos_ = kSeek;
    std::fill(ola_.begin(), ola_.end(), 0.f);
}

void PitchShifter::process(float* io, size_t frames) {
    if (!ready_.load(std::memory_order_acquire)) return;
    if (ratio_.load(std::memory_order_relaxed) == 1.f || frames == 0) return;
    // A render buffer bigger than one working block gets split rather than
    // skipped: on an endpoint with a long period, skipping would leave the
    // key silently doing nothing (or jumping, if only some buffers were big).
    while (frames > kMaxBlock) {
        block(io, kMaxBlock);
        io += kMaxBlock * ch_;
        frames -= kMaxBlock;
    }
    block(io, frames);
}

void PitchShifter::block(float* io, size_t frames) {
    const float r = ratio_.load(std::memory_order_relaxed);
    if (resetReq_.exchange(false, std::memory_order_relaxed)) clearState();

    // --- input FIFO: drop what the search can no longer reach, then append.
    const uint64_t aPos = uint64_t(anaPos_);
    const uint64_t keep =
        (std::min)(aPos >= kSeek ? aPos - kSeek : 0, tmplPos_);
    if (keep > inBase_) {
        const size_t drop = (std::min)(size_t(keep - inBase_), inFill_);
        std::memmove(in_.data(), in_.data() + drop * ch_,
                     (inFill_ - drop) * ch_ * sizeof(float));
        std::memmove(inM_.data(), inM_.data() + drop,
                     (inFill_ - drop) * sizeof(float));
        inFill_ -= drop;
        inBase_ += drop;
    }
    if (inFill_ + frames > kInCap) return; // unreachable in steady state
    std::memcpy(in_.data() + inFill_ * ch_, io, frames * ch_ * sizeof(float));
    for (size_t f = 0; f < frames; ++f) {
        float s = 0.f;
        for (uint32_t k = 0; k < ch_; ++k) s += io[f * ch_ + k];
        inM_[inFill_ + f] = s / float(ch_);
    }
    inFill_ += frames;

    // --- stretch until the resampler has something to read for every frame.
    const size_t needOut = size_t(resPos_ + double(frames - 1) * double(r)) + 2;
    while (outFill_ < needOut && hop(r)) {}

    // --- resample the stretched stream back to the original length.
    if (outFill_ < 2) { // still filling the delay line (first ~43 ms)
        std::memset(io, 0, frames * ch_ * sizeof(float));
        return;
    }
    for (size_t f = 0; f < frames; ++f) {
        const double p = resPos_ + double(f) * double(r);
        size_t i0 = size_t(p);
        float fr = float(p - double(i0));
        if (i0 + 1 >= outFill_) { i0 = outFill_ - 2; fr = 1.f; } // priming only
        for (uint32_t k = 0; k < ch_; ++k) {
            const float a = outF_[i0 * ch_ + k], b = outF_[(i0 + 1) * ch_ + k];
            io[f * ch_ + k] = a + (b - a) * fr;
        }
    }
    resPos_ = (std::min)(resPos_ + double(frames) * double(r),
                         double(outFill_ - 1));

    const size_t drop = size_t(resPos_);
    if (drop) {
        std::memmove(outF_.data(), outF_.data() + drop * ch_,
                     (outFill_ - drop) * ch_ * sizeof(float));
        outFill_ -= drop;
        resPos_ -= double(drop);
    }
}

bool PitchShifter::hop(float r) {
    const uint64_t aPos = uint64_t(anaPos_);
    const uint64_t lo = aPos >= kSeek ? aPos - kSeek : 0;
    const uint64_t hi = aPos + kSeek + kWin; // last frame a candidate can need
    if (inBase_ > lo) return false;                      // history dropped
    if (inBase_ + inFill_ < hi) return false;            // not enough input yet
    if (tmplPos_ < inBase_ || tmplPos_ + kCorr > inBase_ + inFill_) return false;
    if (outFill_ + kHop > kOutCap) return false;

    // WSOLA: of the candidate segments around the nominal analysis position,
    // take the one that continues the previous segment most smoothly. Matching
    // on the mono downmix keeps the two channels phase-locked.
    const float* m = inM_.data();
    const size_t tOff = size_t(tmplPos_ - inBase_);
    size_t bestOff = size_t(aPos - inBase_);
    float bestScore = -1e30f;
    for (uint64_t c = lo; c <= aPos + kSeek; c += 2) { // 2-frame step is plenty
        const size_t co = size_t(c - inBase_);
        float num = 0.f, den = 1e-9f;
        for (size_t i = 0; i < kCorr; ++i) {
            const float v = m[co + i];
            num += v * m[tOff + i];
            den += v * v;
        }
        const float s = num / std::sqrt(den); // normalised: loud != similar
        if (s > bestScore) { bestScore = s; bestOff = co; }
    }

    // Window the chosen segment into the accumulator; with 50 % Hann overlap
    // the leading kHop frames are then final.
    for (size_t f = 0; f < kWin; ++f) {
        const float w = win_[f];
        for (uint32_t k = 0; k < ch_; ++k)
            ola_[f * ch_ + k] += in_[(bestOff + f) * ch_ + k] * w;
    }
    std::memcpy(outF_.data() + outFill_ * ch_, ola_.data(),
                kHop * ch_ * sizeof(float));
    outFill_ += kHop;
    std::memmove(ola_.data(), ola_.data() + kHop * ch_,
                 (kWin - kHop) * ch_ * sizeof(float));
    std::memset(ola_.data() + (kWin - kHop) * ch_, 0, kHop * ch_ * sizeof(float));

    tmplPos_ = inBase_ + bestOff + kHop;
    anaPos_ += double(kHop) / double(r); // nominal pointer advances by Ha
    return true;
}
