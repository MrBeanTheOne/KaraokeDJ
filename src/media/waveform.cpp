#include "media/waveform.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "media/mf_decoder.h"

// BPM from an onset-energy envelope sampled at 93.75 Hz (48 kHz / 512-frame
// hops): mean-removed autocorrelation over 60-180 BPM lags, mild preference
// for 80-160. Needs ~19 s of material (1800 hops) to answer at all.
static int bpmFromOnset(std::vector<float>& onset) {
    if (onset.size() < 1800) return 0;
    double mean = 0.0;
    for (float v : onset) mean += v;
    mean /= double(onset.size());
    for (float& v : onset) v -= float(mean);
    const double hopHz = 48000.0 / 512.0;
    double best = 0.0;
    int bestLag = 0;
    const int loLag = int(hopHz * 60.0 / 180.0); // ~31
    const int hiLag = int(hopHz * 60.0 / 60.0);  // ~94
    for (int lag = loLag; lag <= hiLag; ++lag) {
        double c = 0.0;
        const size_t n = onset.size() - lag;
        for (size_t i = 0; i < n; i += 2) // stride 2: plenty
            c += double(onset[i]) * onset[i + lag];
        const double b = hopHz * 60.0 / lag;
        const double pref = (b >= 80.0 && b <= 160.0) ? 1.0 : 0.85;
        c *= pref / double(n);
        if (c > best) { best = c; bestLag = lag; }
    }
    return bestLag > 0 ? int(hopHz * 60.0 / bestLag + 0.5) : 0;
}

int analyzeBpm(const std::wstring& path) {
    MFDecoder dec;
    if (!dec.open(path, 48000, 2)) return 0;
    // A middle segment hears enough beats; decoding the whole file would
    // triple the analysis time for nothing.
    const uint64_t total = dec.durationFrames(48000);
    if (total > 48000ull * 130)
        dec.seekTo(int64_t(total / 4 / 48000) * 10000000ll);
    std::vector<float> chunk, onset;
    const size_t maxHops = 48000ull * 100 / 512;
    onset.reserve(maxHops + 8);
    double hopE = 0.0;
    float prevE = 0.f;
    uint32_t hopN = 0;
    while (onset.size() < maxHops) {
        chunk.clear();
        if (!dec.readChunk(chunk)) break;
        for (size_t i = 0; i + 1 < chunk.size(); i += 2) {
            float v = std::fabs(chunk[i]);
            const float v2 = std::fabs(chunk[i + 1]);
            if (v2 > v) v = v2;
            hopE += double(v) * v;
            if (++hopN == 512) {
                const float e = float(hopE / 512.0);
                onset.push_back((std::max)(0.f, e - prevE));
                prevE = e;
                hopE = 0.0;
                hopN = 0;
            }
        }
    }
    return bpmFromOnset(onset);
}

void WaveformScanner::cancel() {
    cancel_.store(true);
    if (th_.joinable()) th_.join();
    cancel_.store(false);
    ready_.store(0);
    loud_.store(0.f);
    bpm_.store(0);
    for (auto& b : bins_) b.store(0.f);
}

void WaveformScanner::start(const std::wstring& path) {
    cancel();
    th_ = std::thread([this, path]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            MFDecoder dec;
            if (dec.open(path, 48000, 2)) {
                const uint64_t total = dec.durationFrames(48000);
                if (total > 0) {
                    const uint64_t perBin = (std::max)(uint64_t(1), total / kBins);
                    std::vector<float> chunk;
                    uint64_t frame = 0, binFrames = 0;
                    float peak = 0.f;
                    double sumsq = 0.0;
                    float rms[kBins]{};
                    int bin = 0;
                    // Onset envelope for BPM: energy per 512-frame hop
                    // (93.75 Hz), positive flux only.
                    std::vector<float> onset;
                    onset.reserve(size_t(total / 512) + 8);
                    double hopE = 0.0;
                    float prevE = 0.f;
                    uint32_t hopN = 0;
                    auto closeBin = [&]() {
                        bins_[bin].store(peak, std::memory_order_relaxed);
                        rms[bin] = binFrames
                                       ? float(std::sqrt(sumsq / double(binFrames)))
                                       : 0.f;
                        ready_.store(bin + 1, std::memory_order_release);
                        peak = 0.f;
                        sumsq = 0.0;
                        binFrames = 0;
                        ++bin;
                    };
                    while (!cancel_.load(std::memory_order_relaxed) && bin < kBins) {
                        chunk.clear();
                        if (!dec.readChunk(chunk)) break;
                        for (size_t i = 0; i + 1 < chunk.size(); i += 2) {
                            float v = std::fabs(chunk[i]);
                            const float v2 = std::fabs(chunk[i + 1]);
                            if (v2 > v) v = v2;
                            if (v > peak) peak = v;
                            sumsq += double(v) * v;
                            ++binFrames;
                            hopE += double(v) * v;
                            if (++hopN == 512) {
                                const float e = float(hopE / 512.0);
                                onset.push_back((std::max)(0.f, e - prevE));
                                prevE = e;
                                hopE = 0.0;
                                hopN = 0;
                            }
                            if (++frame >= (uint64_t(bin) + 1) * perBin) {
                                closeBin();
                                if (bin >= kBins) break;
                            }
                        }
                    }
                    if (bin < kBins && binFrames) closeBin();
                    if (bin > 0 && !cancel_.load(std::memory_order_relaxed)) {
                        // loudness = mean RMS of the loudest fifth of the track
                        std::sort(rms, rms + bin, std::greater<float>());
                        const int top = (std::max)(1, bin / 5);
                        double sum = 0.0;
                        for (int i = 0; i < top; ++i) sum += rms[i];
                        loud_.store(float(sum / top), std::memory_order_release);
                    }
                    if (!cancel_.load(std::memory_order_relaxed)) {
                        const int b = bpmFromOnset(onset);
                        if (b > 0) bpm_.store(b, std::memory_order_release);
                    }
                }
            }
        }
        CoUninitialize();
    });
}
