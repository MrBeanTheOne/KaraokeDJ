#include "media/waveform.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "media/mf_decoder.h"

void WaveformScanner::cancel() {
    cancel_.store(true);
    if (th_.joinable()) th_.join();
    cancel_.store(false);
    ready_.store(0);
    loud_.store(0.f);
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
                }
            }
        }
        CoUninitialize();
    });
}
