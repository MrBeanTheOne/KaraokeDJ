#pragma once
#include <cstddef>
#include <string>
#include <vector>

// Musical key detection. A chromagram (Goertzel on the 36 semitones of C3..B5,
// no FFT needed) averaged over the track, then correlated against the 24
// Krumhansl-Schmuckler major/minor profiles.
//
// Fed from the decode loops that already exist (the deck's waveform scan and
// the background library analyzer), so detecting a key costs no extra decode.
class KeyDetector {
public:
    static constexpr size_t kWin = 16384; // 0.34 s at 48 kHz -> 2.9 Hz bins

    void feed(const float* interleavedStereo, size_t frames); // 48 kHz stereo
    // -1 = no key confident enough; else tonic 0..11 (C..B), +12 for minor.
    int result() const;
    // Accumulated pitch-class weights, for retuning the profiles against real
    // material (tools/dump_chroma) rather than by guesswork.
    const double* chroma() const { return chroma_; }
    int windows() const { return windows_; }

private:
    std::vector<float> buf_; // mono, filled to kWin then analysed
    double chroma_[12] = {};
    int windows_ = 0;
};

// media_item.music_key encoding, mirroring what the bpm column already does:
//   0     = never analysed
//   -1    = analysed, no clear key
//   1..24 = KeyDetector::result() + 1
inline int keyToDb(int k) { return k < 0 ? -1 : k + 1; }

// Display name of a stored key transposed by `semitones`, e.g. (Am, +2) ->
// "Bm". Empty string when the key is unknown or not analysed yet.
std::wstring keyName(int dbValue, int semitones);
