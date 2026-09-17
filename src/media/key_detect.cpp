#include "media/key_detect.h"

#include <cmath>

namespace {

constexpr int kLowMidi = 48;  // C3
constexpr int kNotes = 36;    // ...through B5
constexpr double kPi = 3.14159265358979323846;

// Temperley's Kostka-Payne key profiles: the tonal hierarchy of a major and a
// minor scale. Rotating these twelve ways and correlating is the standard way
// to turn a chromagram into a key.
//
// These are deliberately NOT the better-known Krumhansl-Kessler profiles.
// K-K is derived from listener probe-tone ratings and is far too flat for
// audio chromagrams: measured over 183 real tracks from this library it
// called 97 % of them minor (every major song came back as its relative
// minor). Temperley's near-zero weights for out-of-scale tones separate the
// modes properly — same measurement, 74 % major, and it agrees with the
// known key of every track we could check by hand. Retune with
// tools/dump_chroma if this is ever revisited; don't swap profiles by feel.
const double kMajor[12] = {0.748, 0.060, 0.488, 0.082, 0.670, 0.460,
                           0.096, 0.715, 0.104, 0.366, 0.057, 0.400};
const double kMinor[12] = {0.712, 0.084, 0.474, 0.618, 0.049, 0.460,
                           0.105, 0.747, 0.404, 0.067, 0.133, 0.330};

// Goertzel coefficient per semitone, and a Hann window — both fixed, so they
// are built once and shared by every detector.
struct Tables {
    double coeff[kNotes];
    std::vector<float> hann;
    Tables() {
        for (int n = 0; n < kNotes; ++n) {
            const double f = 440.0 * std::pow(2.0, (kLowMidi + n - 69) / 12.0);
            coeff[n] = 2.0 * std::cos(2.0 * kPi * f / 48000.0);
        }
        hann.resize(KeyDetector::kWin);
        for (size_t i = 0; i < KeyDetector::kWin; ++i)
            hann[i] = float(0.5 - 0.5 * std::cos(2.0 * kPi * double(i) /
                                                 double(KeyDetector::kWin)));
    }
};
const Tables& tables() {
    static const Tables t;
    return t;
}

double pearson(const double* a, const double* b) {
    double ma = 0.0, mb = 0.0;
    for (int i = 0; i < 12; ++i) { ma += a[i]; mb += b[i]; }
    ma /= 12.0;
    mb /= 12.0;
    double num = 0.0, da = 0.0, db = 0.0;
    for (int i = 0; i < 12; ++i) {
        const double x = a[i] - ma, y = b[i] - mb;
        num += x * y;
        da += x * x;
        db += y * y;
    }
    return (da > 0.0 && db > 0.0) ? num / std::sqrt(da * db) : 0.0;
}

} // namespace

void KeyDetector::feed(const float* io, size_t frames) {
    const Tables& t = tables();
    if (buf_.capacity() < kWin) buf_.reserve(kWin);
    for (size_t f = 0; f < frames; ++f) {
        buf_.push_back(0.5f * (io[f * 2] + io[f * 2 + 1]));
        if (buf_.size() < kWin) continue;

        // One windowed block -> 36 Goertzel magnitudes -> 12 pitch classes.
        double win[12] = {};
        double total = 0.0;
        for (int n = 0; n < kNotes; ++n) {
            const double c = t.coeff[n];
            double s1 = 0.0, s2 = 0.0;
            for (size_t i = 0; i < kWin; ++i) {
                const double s = double(buf_[i]) * t.hann[i] + c * s1 - s2;
                s2 = s1;
                s1 = s;
            }
            const double mag2 = s1 * s1 + s2 * s2 - c * s1 * s2;
            const double mag = mag2 > 0.0 ? std::sqrt(mag2) : 0.0;
            win[n % 12] += mag;
            total += mag;
        }
        buf_.clear();
        // Normalise per block so choruses don't outvote verses, and skip
        // near-silent blocks (intros, gaps) entirely.
        if (total <= 1e-6) continue;
        for (int i = 0; i < 12; ++i) chroma_[i] += win[i] / total;
        ++windows_;
    }
}

int KeyDetector::result() const {
    if (windows_ < 20) return -1; // under ~7 s of material: don't guess
    double best = 0.0;
    int bestKey = -1;
    for (int tonic = 0; tonic < 12; ++tonic) {
        double rotMaj[12], rotMin[12];
        for (int i = 0; i < 12; ++i) {
            rotMaj[i] = kMajor[(i - tonic + 12) % 12];
            rotMin[i] = kMinor[(i - tonic + 12) % 12];
        }
        const double cMaj = pearson(chroma_, rotMaj);
        if (cMaj > best) { best = cMaj; bestKey = tonic; }
        const double cMin = pearson(chroma_, rotMin);
        if (cMin > best) { best = cMin; bestKey = tonic + 12; }
    }
    // Measured over this library: median 0.83, and 0.5 rejects the bottom 2 %
    // (spoken word, drum-only tracks, heavy noise) — those say "no key"
    // rather than showing the operator a confident wrong answer.
    return best >= 0.5 ? bestKey : -1;
}

std::wstring keyName(int dbValue, int semitones) {
    if (dbValue <= 0) return L""; // never analysed, or analysed with no answer
    static const wchar_t* kNames[12] = {L"C",  L"C#", L"D",  L"D#",
                                        L"E",  L"F",  L"F#", L"G",
                                        L"G#", L"A",  L"A#", L"B"};
    const int k = dbValue - 1;
    const bool minor = k >= 12;
    const int tonic = ((k % 12) + semitones % 12 + 12) % 12;
    return std::wstring(kNames[tonic]) + (minor ? L"m" : L"");
}
