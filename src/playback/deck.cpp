#include "playback/deck.h"

#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <vector>

#include "media/mf_decoder.h"

using namespace std::chrono_literals;

bool Deck::load(const std::wstring& path, bool loopFile) {
    stopAndUnload();
    quit_.store(false);
    eos.store(false);
    decodeDone.store(false);
    paused.store(false);
    pendingSeekHns_.store(-1);
    framesPlayed.store(0);
    totalFrames.store(0);
    key.setSemitones(0, ch_);
    key.reset();
    ring.clear();
    state_.store(DeckState::Loading, std::memory_order_release);
    worker_ = std::thread(&Deck::workerMain, this, path, loopFile);
    return true;
}

void Deck::stopAndUnload() {
    playing.store(false);
    quit_.store(true);
    if (worker_.joinable()) worker_.join();
    ring.clear();
    // An emptied deck has no clock: leaving these set made a cleared slot keep
    // drawing the retired track's elapsed/remaining time and its markers.
    framesPlayed.store(0);
    totalFrames.store(0);
    eos.store(false);
    decodeDone.store(false);
    state_.store(DeckState::Empty, std::memory_order_release);
}

void Deck::seek(double sec) {
    const bool wasPaused = paused.exchange(true); // stop the mixer pulling
    std::this_thread::sleep_for(30ms);            // let the audio thread step out
    key.reset(); // don't stitch post-seek audio onto the pre-seek tail
    pendingSeekHns_.store(int64_t(sec * 10000000.0));
    for (int t = 0; t < 500 && pendingSeekHns_.load() >= 0; t += 5)
        std::this_thread::sleep_for(5ms); // worker applies it
    if (!wasPaused) paused.store(false);
}

void Deck::workerMain(std::wstring path, bool loopFile) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        MFDecoder dec;
        if (!dec.open(path, rate_, ch_)) {
            state_.store(DeckState::Error, std::memory_order_release);
            CoUninitialize();
            return;
        }
        if (!loopFile) totalFrames.store(dec.durationFrames(rate_));
        std::vector<float> chunk;
        chunk.reserve(16384);
        bool ended = false;
        while (!quit_.load(std::memory_order_relaxed)) {
            const int64_t sk = pendingSeekHns_.load(std::memory_order_relaxed);
            if (sk >= 0) { // mixer is paused off the ring while this runs
                dec.seekTo(sk);
                ring.clear();
                chunk.clear();
                ended = false;
                decodeDone.store(false);
                eos.store(false);
                framesPlayed.store(uint64_t(sk) * rate_ / 10000000ull);
                pendingSeekHns_.store(-1);
            }
            if (chunk.empty() && !ended) {
                if (!dec.readChunk(chunk)) {
                    if (loopFile) {
                        dec.seekStart();
                    } else {
                        ended = true;
                        decodeDone.store(true);
                    }
                }
            }
            if (!chunk.empty()) {
                const size_t pushed = ring.push(chunk.data(), chunk.size());
                if (pushed == chunk.size()) {
                    chunk.clear();
                } else {
                    chunk.erase(chunk.begin(), chunk.begin() + pushed);
                    std::this_thread::sleep_for(5ms); // ring full, back off
                }
            } else if (ended) {
                if (ring.size() == 0) eos.store(true);
                std::this_thread::sleep_for(10ms);
            }
            if (state_.load(std::memory_order_relaxed) == DeckState::Loading &&
                (ring.size() >= preload_ || ended)) {
                state_.store(DeckState::Ready, std::memory_order_release);
            }
        }
    }
    CoUninitialize();
}
