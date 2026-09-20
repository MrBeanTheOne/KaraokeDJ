#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct IMFSourceReader;
struct IMFDXGIDeviceManager;
struct ID3D11Device;

struct VideoFrame {
    std::vector<uint8_t> bgra; // top-down, stride = width * 4
    uint32_t width = 0, height = 0;
    int64_t ptsHns = 0; // presentation time, 100 ns units
};

// Decodes the first video stream to BGRA on a worker thread into a small
// bounded queue. Video is not real-time critical the way audio is, so a
// mutex + deque is fine here (the audio path stays lock-free).
class MFVideoDecoder {
public:
    // Async: returns immediately, the worker thread does the (100 ms+) source
    // resolution and codec setup so deck loads never stall the UI. A file
    // with no decodable video stream shows up as failed() shortly after.
    bool open(const std::wstring& path);
    bool failed() const { return state_.load(std::memory_order_acquire) == 2; }
    void close();
    ~MFVideoDecoder(); // close() + releases the shared D3D decode device

    // Latest frame due at or before clockHns (older due frames are dropped),
    // or nullptr if nothing is due yet.
    std::unique_ptr<VideoFrame> popDue(int64_t clockHns);

    // Async: worker drops queued frames and repositions (revives after EOS).
    void seek(int64_t hns) { pendingSeekHns_.store(hns); }

    uint64_t dropped() const { return dropped_.load(); }
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }

private:
    void workerMain();
    bool openReader(); // worker thread: resolve source, negotiate BGRA

    void ensureD3D(); // GPU decode device, created once, kept across opens

    std::wstring path_;
    std::atomic<int> state_{0}; // 0 opening, 1 decoding, 2 failed
    IMFSourceReader* reader_ = nullptr;
    IMFDXGIDeviceManager* dxgiMgr_ = nullptr;
    ID3D11Device* d3d_ = nullptr;
    bool d3dTried_ = false;
    uint32_t w_ = 0, h_ = 0;
    int32_t stride_ = 0;
    mutable std::mutex mu_;
    std::deque<std::unique_ptr<VideoFrame>> q_;
    std::atomic<bool> quit_{false}, eos_{false};
    std::atomic<int64_t> pendingSeekHns_{-1};
    std::atomic<uint64_t> dropped_{0};
    std::thread worker_;
};
