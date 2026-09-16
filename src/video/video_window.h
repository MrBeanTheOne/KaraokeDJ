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

// One waiting-screen element: anchored to a 3x3 grid cell (0..8 row-major,
// top-left..bottom-right) in one of three sizes. The operator arranges these
// in Settings; drawIdle renders whatever is on and has content.
struct IdleElem {
    bool on = true;
    int pos = 4;  // 0..8 grid cell
    int size = 1; // 0 S, 1 M, 2 L
};

// Everything the waiting screen shows, rebuilt cheaply by the engine each
// tick. Element order: 0 logo, 1 title, 2 message, 3 next-up, 4 singer list,
// 5 QR code.
struct IdleScene {
    static constexpr int kLogo = 0, kTitle = 1, kMessage = 2, kNextUp = 3,
                         kSingers = 4, kQr = 5;
    std::wstring title, message, nextUp;
    std::vector<std::wstring> singers; // "1. Alice — song" lines, few
    std::wstring qrUrl;                // "" hides the QR
    IdleElem elems[6];
};

// Decode an image file (png/jpg/bmp/gif) to a BGRA VideoFrame via WIC,
// downscaled to a sane size for a logo. COM must be initialized.
bool loadImageFile(const std::wstring& path, VideoFrame& out);

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
    // to skip a slot; both -1 = the waiting screen (idle scene over black).
    void draw(int baseSlot, int topSlot, float topAlpha);
    // The full waiting-screen description (replaces setIdleText/setQr).
    void setIdleScene(const IdleScene& s);
    // Logo bitmap for the waiting screen. hasLogo() goes false after device
    // loss — the engine re-uploads exactly like the deck frames.
    void setLogo(const VideoFrame& f);
    void clearLogo();
    bool hasLogo() const { return logo_ != nullptr; }
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

    void setQr(const std::wstring& url); // regenerates only on change
    void drawIdle();

    HWND hwnd_ = nullptr;
    ID2D1Factory* factory_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    IdleScene scene_;
    ID2D1Bitmap* bmp_[2] = {nullptr, nullptr};
    uint32_t bw_[2] = {0, 0}, bh_[2] = {0, 0};
    ID2D1Bitmap* logo_ = nullptr;
    uint32_t lw_ = 0, lh_ = 0;
    std::wstring qrUrl_;
    std::vector<uint8_t> qrMods_; // qrSize_ x qrSize_ (1 = dark module)
    int qrSize_ = 0;
    int fit_ = 0; // 0 fit, 1 fill, 2 stretch
    bool lost_ = false;
    bool quit_ = false;
    std::atomic<int> key_{0};
};
