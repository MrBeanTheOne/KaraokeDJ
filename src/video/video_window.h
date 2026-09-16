#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>

#include <string>
#include <vector>

struct VideoFrame;
struct ID2D1Factory;
struct ID2D1HwndRenderTarget;
struct ID2D1Bitmap;
struct IDWriteFactory;

// One video output window with two bitmap slots (one per deck) so the master
// video can crossfade with the audio transition. Letterboxed Direct2D drawing.
// monitor < 0: resizable 960x540 dev window; monitor >= 0: borderless
// fullscreen on that display (0 = primary, 1 = second, ...).
// ponytail: single window; the shared-texture D3D11 compositor replaces this
// when FR-008 multi-display mirroring lands.
class VideoWindow {
public:
    static int monitorCount();
    static RECT monitorRect(int index); // {0,0,0,0} if out of range

    bool create(int monitor, int cascade = 0); // cascade offsets dev windows
    void setFrame(int slot, const VideoFrame& f); // upload a deck's newest frame
    // Compose: baseSlot at full opacity, topSlot over it at topAlpha. Pass -1
    // to skip a slot; both -1 = the waiting screen (idle text over black).
    void draw(int baseSlot, int topSlot, float topAlpha);
    void setIdleText(const std::wstring& title, const std::wstring& detail) {
        idleTitle_ = title;
        idleDetail_ = detail;
    }
    // Waiting-screen QR ("scan to request a song"). "" hides it; the module
    // grid is regenerated only when the url changes.
    void setQr(const std::wstring& url);
    // 0 = fit (letterbox), 1 = fill (crop), 2 = stretch.
    void setFitMode(int m) { fit_ = m; }
    bool pump(); // process messages; false once closed or ESC pressed
    // True once after the render target was lost+recreated: re-setFrame slots.
    bool bitmapsLost() { const bool l = lost_; lost_ = false; return l; }
    // Last key pressed in this window since the previous call (0 = none).
    int popKey() { return key_.exchange(0); }
    void destroy();
    ~VideoWindow() { destroy(); }
    uint64_t presented = 0; // frames uploaded via setFrame

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    void releaseTarget();
    bool ensureTarget();
    void drawSlot(int slot, float alpha);

    void drawIdle();

    HWND hwnd_ = nullptr;
    ID2D1Factory* factory_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    std::wstring idleTitle_, idleDetail_;
    std::wstring qrUrl_;
    std::vector<uint8_t> qrMods_; // qrSize_ x qrSize_ (1 = dark module)
    int qrSize_ = 0;
    ID2D1Bitmap* bmp_[2] = {nullptr, nullptr};
    uint32_t bw_[2] = {0, 0}, bh_[2] = {0, 0};
    int fit_ = 0; // 0 fit, 1 fill, 2 stretch
    bool lost_ = false;
    bool quit_ = false;
    std::atomic<int> key_{0};
};
