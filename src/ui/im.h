#pragma once
// UI language (lang.cpp): 0 = English (strings as written), 1 = French via
// the central dictionary. uiTr also retranslates composed heads
// ("QUEUE (3)", "playing: X").
void uiSetLanguage(int lang);
int uiLanguage();
#include <string>
std::wstring uiTr(const std::wstring& s);
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <chrono>
#include <map>
#include <string>
#include <vector>

// Per-frame input, filled by the window proc, consumed by the UI pass.
struct UiInput {
    float mx = 0, my = 0;
    bool down = false;     // primary button held
    bool pressed = false;  // went down this frame
    bool released = false; // went up this frame
    bool dblclick = false;
    // Click events latch their own coordinates: later WM_MOUSEMOVEs in the
    // same batch (e.g. the one ReleaseCapture reposts) must not retarget them.
    float pressX = -1, pressY = -1;
    bool shift = false, ctrl = false; // modifier state at the press
    float dblX = -1, dblY = -1;
    bool rpressed = false; // right-click edge (context menus)
    float rX = -1, rY = -1;
    float wheel = 0; // rows (+ = up)
    std::vector<wchar_t> typed;
    bool enter = false, del = false, pgdn = false, pgup = false;
    bool pauseKey = false, esc = false; // gig shortcuts (main window)
    int navKey = 0;                     // browser arrows: -1 up, +1 down
};

inline D2D1_COLOR_F col(uint32_t rgb, float a = 1.f) {
    return D2D1_COLOR_F{((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f,
                        (rgb & 0xFF) / 255.f, a};
}
inline D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1_COLOR_F{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
inline bool hit(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
inline D2D1_RECT_F rc(float x, float y, float w, float h) {
    return D2D1_RECT_F{x, y, x + w, y + h};
}

// Immediate-mode UI on Direct2D + DirectWrite: draw everything every frame,
// widgets identified by integer ids (unique per frame) for drag capture.
class Ui {
public:
    bool init(HWND hwnd);
    void shutdown();
    void resize(UINT w, UINT h);

    void beginFrame(const UiInput& input);
    void endFrame(); // handles device-loss by recreating the target

    // Physical pixels per DIP (layout unit). Input is scaled into DIPs in
    // beginFrame; callers convert client sizes and menu positions with this.
    float dpiScale() const { return scale_; }
    void refreshDpi() { // call on WM_DPICHANGED: next frame re-reads the DPI
        releaseImages();
        if (brush_) { brush_->Release(); brush_ = nullptr; }
        if (rt) { rt->Release(); rt = nullptr; }
    }

    void rect(const D2D1_RECT_F& r, D2D1_COLOR_F c, float rad = 5.f);
    void frameRect(const D2D1_RECT_F& r, D2D1_COLOR_F c, float rad = 5.f, float w = 1.f);
    void circle(float cx, float cy, float rad, D2D1_COLOR_F c, bool fill = true,
                float strokeW = 1.f);
    void line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float w = 1.f);
    // Cached image slots (video previews): setImage uploads BGRA pixels, image()
    // draws the cached bitmap letterboxed into r — false when nothing is cached
    // (e.g. after device loss: re-upload and draw again).
    void setImage(int slot, const uint8_t* bgra, uint32_t w, uint32_t h);
    bool image(int slot, const D2D1_RECT_F& r);
    // align: 0 left, 1 center, 2 right (vertically centered, single line)
    // Every label passes through uiTr(), so the language option needs no
    // changes at draw sites.
    void text(const D2D1_RECT_F& r, const std::wstring& s, float size, D2D1_COLOR_F c,
              int align = 0, bool bold = false);

    bool button(int id, const D2D1_RECT_F& r, const std::wstring& label,
                D2D1_COLOR_F accent, bool filled = false);
    bool toggle(int id, const D2D1_RECT_F& r, const std::wstring& label, bool on,
                D2D1_COLOR_F accent);
    // fillFromCenter: crossfader-style — neutral at center, the accent fill
    // grows from the middle toward the thumb. leftAccent (optional) colors the
    // left-of-center fill differently (deck A vs deck B colors).
    bool sliderH(int id, const D2D1_RECT_F& r, float& v, D2D1_COLOR_F accent,
                 bool fillFromCenter = false,
                 const D2D1_COLOR_F* leftAccent = nullptr);
    bool sliderV(int id, const D2D1_RECT_F& r, float& v, D2D1_COLOR_F accent);
    void clipPush(const D2D1_RECT_F& r);
    void clipPop();

    // Eased hover amount [0..1] for a widget id — gives transitions a frame
    // of animation instead of hard state flips.
    float hover(int id, bool over);
    bool dragging() const { return active_ != 0; }

    UiInput in;
    ID2D1HwndRenderTarget* rt = nullptr;

private:
    // Largest font size <= base whose rendered label fits maxW (French labels
    // run long; buttons shrink text instead of clipping it).
    float fitSize(const std::wstring& s, float maxW, float base);
    IDWriteTextFormat* fmtIcon(float size);
    IDWriteTextFormat* fmt(float size, bool bold, int align);
    bool ensureTarget();
    void releaseImages();

    struct Img {
        ID2D1Bitmap* bmp = nullptr;
        uint32_t w = 0, h = 0;
    };
    std::map<int, Img> imgs_;

    HWND hwnd_ = nullptr;
    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    std::map<int, IDWriteTextFormat*> fmts_;
    std::map<int, float> hot_;
    std::chrono::steady_clock::time_point lastFrame_{};
    float dt_ = 0.016f;
    int active_ = 0; // widget id holding mouse capture
    float scale_ = 1.f;
};
