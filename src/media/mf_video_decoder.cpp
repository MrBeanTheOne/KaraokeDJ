#include "media/mf_video_decoder.h"

#include <windows.h>
#include <objbase.h>
#include <d3d10_1.h> // ID3D10Multithread (implemented by the D3D11 device too)
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <chrono>
#include <cstring>

using namespace std::chrono_literals;

static constexpr size_t kMaxQueued = 8; // bounded frame queue (plan §9)

// Hardware decode: hand the SourceReader a D3D11 video device so the GPU's
// fixed-function H.264/HEVC decoder does the heavy lifting — essential on
// iGPU-only laptops, where software 1080p decode eats most of a core. Created
// once per decoder instance, kept across opens; on failure (no GPU/driver)
// everything silently stays on the software path.
void MFVideoDecoder::ensureD3D() {
    if (d3dTried_) return;
    d3dTried_ = true;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &d3d_, nullptr, nullptr))) {
        d3d_ = nullptr;
        return;
    }
    ID3D10Multithread* mt = nullptr; // decoder MFTs use their own threads
    if (SUCCEEDED(d3d_->QueryInterface(IID_PPV_ARGS(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }
    UINT token = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &dxgiMgr_)) ||
        FAILED(dxgiMgr_->ResetDevice(d3d_, token))) {
        if (dxgiMgr_) { dxgiMgr_->Release(); dxgiMgr_ = nullptr; }
        d3d_->Release();
        d3d_ = nullptr;
    }
}

bool MFVideoDecoder::open(const std::wstring& path) {
    close();
    path_ = path;
    state_.store(0, std::memory_order_release);
    quit_.store(false);
    eos_.store(false);
    dropped_.store(0);
    pendingSeekHns_.store(-1);
    worker_ = std::thread(&MFVideoDecoder::workerMain, this);
    return true; // optimistic — callers demote via failed() once known
}

// Worker thread only: everything here can take 100 ms+ (source resolution,
// codec MFT setup, hardware decoder init) and must never run on the UI
// thread. The reader is built into a local until it is fully negotiated.
bool MFVideoDecoder::openReader() {
    ensureD3D();

    IMFAttributes* attrs = nullptr;
    MFCreateAttributes(&attrs, 3);
    if (attrs) {
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        if (dxgiMgr_) {
            attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dxgiMgr_);
            attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        }
    }
    IMFSourceReader* r = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(path_.c_str(), attrs, &r);
    if (attrs) attrs->Release();
    if (FAILED(hr)) return false;

    r->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(r->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE))) {
        r->Release();
        return false;
    }

    IMFMediaType* t = nullptr;
    if (FAILED(MFCreateMediaType(&t))) { r->Release(); return false; }
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32); // BGRX in memory = D2D B8G8R8A8
    hr = r->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t);
    t->Release();
    if (FAILED(hr)) { r->Release(); return false; }

    IMFMediaType* cur = nullptr;
    if (FAILED(r->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
        r->Release();
        return false;
    }
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
    stride_ = int32_t(MFGetAttributeUINT32(cur, MF_MT_DEFAULT_STRIDE, w * 4));
    cur->Release();
    if (!w || !h) { r->Release(); return false; }
    w_ = w;
    h_ = h;
    reader_ = r;
    return true;
}

void MFVideoDecoder::workerMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!openReader()) {
        state_.store(2, std::memory_order_release);
        CoUninitialize();
        return;
    }
    state_.store(1, std::memory_order_release);
    while (!quit_.load(std::memory_order_relaxed)) {
        const int64_t sk = pendingSeekHns_.exchange(-1);
        if (sk >= 0) { // reader is only ever touched from this thread
            PROPVARIANT var{};
            var.vt = VT_I8;
            var.hVal.QuadPart = sk;
            reader_->SetCurrentPosition(GUID_NULL, var);
            std::lock_guard<std::mutex> g(mu_);
            q_.clear();
            eos_.store(false);
        }
        if (eos_.load(std::memory_order_relaxed)) { // idle until seek or close
            std::this_thread::sleep_for(10ms);
            continue;
        }
        {
            std::unique_lock<std::mutex> g(mu_);
            if (q_.size() >= kMaxQueued) { // queue full: wait for presenter to drain
                g.unlock();
                std::this_thread::sleep_for(5ms);
                continue;
            }
        }

        DWORD flags = 0;
        LONGLONG pts = 0;
        IMFSample* sample = nullptr;
        if (FAILED(reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr,
                                       &flags, &pts, &sample))) {
            eos_.store(true);
            continue; // stay alive: a seek can revive playback
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (sample) sample->Release();
            eos_.store(true);
            continue;
        }
        if (!sample) continue;

        auto frame = std::make_unique<VideoFrame>();
        frame->width = w_;
        frame->height = h_;
        frame->ptsHns = pts;
        frame->bgra.resize(size_t(w_) * h_ * 4);

        IMFMediaBuffer* buf = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
            BYTE* data = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                const uint32_t rowBytes = w_ * 4;
                const uint32_t absStride = uint32_t(stride_ < 0 ? -stride_ : stride_);
                if (len >= size_t(absStride) * h_) {
                    for (uint32_t y = 0; y < h_; ++y) {
                        // negative stride = bottom-up bitmap; flip while copying
                        const BYTE* src = stride_ < 0
                                              ? data + size_t(h_ - 1 - y) * absStride
                                              : data + size_t(y) * absStride;
                        std::memcpy(frame->bgra.data() + size_t(y) * rowBytes, src, rowBytes);
                    }
                }
                buf->Unlock();
            }
            buf->Release();
        }
        sample->Release();

        std::lock_guard<std::mutex> g(mu_);
        q_.push_back(std::move(frame));
    }
    CoUninitialize();
}

std::unique_ptr<VideoFrame> MFVideoDecoder::popDue(int64_t clockHns) {
    std::lock_guard<std::mutex> g(mu_);
    if (q_.empty() || q_.front()->ptsHns > clockHns) return nullptr;
    // Keep only the newest frame that is already due; older due frames are late.
    while (q_.size() >= 2 && q_[1]->ptsHns <= clockHns) {
        q_.pop_front();
        dropped_.fetch_add(1);
    }
    auto f = std::move(q_.front());
    q_.pop_front();
    return f;
}

void MFVideoDecoder::close() {
    quit_.store(true);
    if (worker_.joinable()) worker_.join();
    if (reader_) { reader_->Release(); reader_ = nullptr; }
    std::lock_guard<std::mutex> g(mu_);
    q_.clear();
}

MFVideoDecoder::~MFVideoDecoder() {
    close();
    if (dxgiMgr_) { dxgiMgr_->Release(); dxgiMgr_ = nullptr; }
    if (d3d_) { d3d_->Release(); d3d_ = nullptr; }
}
