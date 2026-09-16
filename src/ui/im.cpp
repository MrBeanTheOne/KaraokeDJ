#include "ui/im.h"

#include <algorithm>

static const D2D1_COLOR_F kBg = col(0x0A0A0D);
static const D2D1_COLOR_F kBtnBg = col(0x1B1B1F);
static const D2D1_COLOR_F kBtnHover = col(0x2C2C33);
static const D2D1_COLOR_F kBtnBorder = col(0x38383F);
static const D2D1_COLOR_F kTrack = col(0x26262C);
static const D2D1_COLOR_F kThumb = col(0xE8E8EC);
static const D2D1_COLOR_F kText = col(0xECECEF);
static const D2D1_COLOR_F kTextDim = col(0x94949D);

bool Ui::init(HWND hwnd) {
    hwnd_ = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dw_))))
        return false;
    return ensureTarget();
}

bool Ui::ensureTarget() {
    if (rt) return true;
    RECT cr{};
    GetClientRect(hwnd_, &cr);
    // Render in DIPs at the window's DPI: layout units scale with the
    // monitor, and beginFrame converts mouse input into the same space.
    UINT dpi = 96;
    using GetDpiFn = UINT(WINAPI*)(HWND);
    static auto getDpi = reinterpret_cast<GetDpiFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (getDpi) dpi = getDpi(hwnd_);
    scale_ = float(dpi) / 96.f;
    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    props.dpiX = props.dpiY = float(dpi);
    if (FAILED(d2d_->CreateHwndRenderTarget(
            props,
            D2D1::HwndRenderTargetProperties(
                hwnd_, D2D1::SizeU(cr.right - cr.left, cr.bottom - cr.top)),
            &rt)))
        return false;
    if (brush_) { brush_->Release(); brush_ = nullptr; }
    rt->CreateSolidColorBrush(kText, &brush_);
    return brush_ != nullptr;
}

void Ui::releaseImages() {
    for (auto& [k, im] : imgs_)
        if (im.bmp) im.bmp->Release();
    imgs_.clear();
}

void Ui::shutdown() {
    releaseImages();
    for (auto& [k, f] : fmts_)
        if (f) f->Release();
    fmts_.clear();
    if (brush_) { brush_->Release(); brush_ = nullptr; }
    if (rt) { rt->Release(); rt = nullptr; }
    if (dw_) { dw_->Release(); dw_ = nullptr; }
    if (d2d_) { d2d_->Release(); d2d_ = nullptr; }
}

void Ui::resize(UINT w, UINT h) {
    if (rt) rt->Resize(D2D1::SizeU(w, h));
}

void Ui::beginFrame(const UiInput& input) {
    const auto now = std::chrono::steady_clock::now();
    if (lastFrame_.time_since_epoch().count())
        dt_ = std::clamp(
            std::chrono::duration<float>(now - lastFrame_).count(), 0.001f, 0.1f);
    lastFrame_ = now;
    if (!ensureTarget()) { in = input; return; }
    in = input; // convert physical mouse coordinates into DIPs
    in.mx /= scale_;
    in.my /= scale_;
    if (in.pressX >= 0) { in.pressX /= scale_; in.pressY /= scale_; }
    if (in.dblX >= 0) { in.dblX /= scale_; in.dblY /= scale_; }
    if (in.rX >= 0) { in.rX /= scale_; in.rY /= scale_; }
    rt->BeginDraw();
    rt->Clear(kBg);
}

float Ui::hover(int id, bool over) {
    float& h = hot_[id];
    const float target = over ? 1.f : 0.f;
    h += (target - h) * (std::min)(1.f, dt_ * 14.f);
    if (h < 0.004f) h = 0.f;
    else if (h > 0.996f) h = 1.f;
    return h;
}

void Ui::endFrame() {
    if (!rt) return;
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        releaseImages(); // bitmaps die with the target; callers re-setImage
        if (brush_) { brush_->Release(); brush_ = nullptr; }
        rt->Release();
        rt = nullptr; // recreated next frame
    }
}

float Ui::fitSize(const std::wstring& s, float maxW, float base) {
    if (s.empty() || maxW <= 8) return base;
    IDWriteTextLayout* tl = nullptr;
    if (FAILED(dw_->CreateTextLayout(s.c_str(), UINT32(s.size()),
                                     fmt(base, true, 1), 1e6f, 100.f, &tl)) ||
        !tl)
        return base;
    DWRITE_TEXT_METRICS mtr{};
    tl->GetMetrics(&mtr);
    tl->Release();
    if (mtr.width <= maxW || mtr.width <= 0) return base;
    return (std::max)(8.f, base * maxW / mtr.width - 0.5f);
}

IDWriteTextFormat* Ui::fmt(float size, bool bold, int align) {
    const int key = int(size) * 16 + (bold ? 8 : 0) + align;
    auto it = fmts_.find(key);
    if (it != fmts_.end()) return it->second;
    IDWriteTextFormat* f = nullptr;
    dw_->CreateTextFormat(L"Segoe UI", nullptr,
                          bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                          L"", &f);
    if (f) {
        f->SetTextAlignment(align == 1   ? DWRITE_TEXT_ALIGNMENT_CENTER
                            : align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                         : DWRITE_TEXT_ALIGNMENT_LEADING);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    fmts_[key] = f;
    return f;
}

void Ui::rect(const D2D1_RECT_F& r, D2D1_COLOR_F c, float rad) {
    if (!rt) return;
    brush_->SetColor(c);
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), brush_);
}

void Ui::frameRect(const D2D1_RECT_F& r, D2D1_COLOR_F c, float rad, float w) {
    if (!rt) return;
    brush_->SetColor(c);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), brush_, w);
}

void Ui::text(const D2D1_RECT_F& r, const std::wstring& s, float size, D2D1_COLOR_F c,
              int align, bool bold) {
    if (!rt || s.empty()) return;
    const std::wstring t = uiLanguage() ? uiTr(s) : s;
    brush_->SetColor(c);
    rt->DrawTextW(t.c_str(), UINT32(t.size()), fmt(size, bold, align), r, brush_,
                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Ui::circle(float cx, float cy, float rad, D2D1_COLOR_F c, bool fill,
                float strokeW) {
    if (!rt) return;
    brush_->SetColor(c);
    const D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad);
    if (fill) rt->FillEllipse(e, brush_);
    else rt->DrawEllipse(e, brush_, strokeW);
}

void Ui::line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float w) {
    if (!rt) return;
    brush_->SetColor(c);
    rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush_, w);
}

void Ui::setImage(int slot, const uint8_t* bgra, uint32_t w, uint32_t h) {
    if (!rt || !bgra || !w || !h) return;
    Img& im = imgs_[slot];
    if (im.bmp && (im.w != w || im.h != h)) {
        im.bmp->Release();
        im.bmp = nullptr;
    }
    if (!im.bmp) {
        const auto props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(rt->CreateBitmap(D2D1::SizeU(w, h), props, &im.bmp))) {
            im.bmp = nullptr;
            return;
        }
        im.w = w;
        im.h = h;
    }
    im.bmp->CopyFromMemory(nullptr, bgra, w * 4);
}

bool Ui::image(int slot, const D2D1_RECT_F& r) {
    const auto it = imgs_.find(slot);
    if (!rt || it == imgs_.end() || !it->second.bmp) return false;
    const Img& im = it->second;
    const float rw = r.right - r.left, rh = r.bottom - r.top;
    const float s = (std::min)(rw / im.w, rh / im.h);
    const float dw = im.w * s, dh = im.h * s;
    rt->DrawBitmap(im.bmp,
                   rc(r.left + (rw - dw) / 2, r.top + (rh - dh) / 2, dw, dh), 1.f,
                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    return true;
}

void Ui::clipPush(const D2D1_RECT_F& r) {
    if (rt) rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
}
void Ui::clipPop() {
    if (rt) rt->PopAxisAlignedClip();
}

bool Ui::button(int id, const D2D1_RECT_F& r, const std::wstring& label,
                D2D1_COLOR_F accent, bool filled) {
    const bool over = hit(r, in.mx, in.my);
    const float h = hover(id, over);
    const bool held = in.down && hit(r, in.pressX, in.pressY);
    const std::wstring lt = uiLanguage() ? uiTr(label) : label;
    const float sz = fitSize(lt, r.right - r.left - 12, 13);
    if (filled) {
        D2D1_COLOR_F bg = accent;
        if (held) bg = mix(accent, col(0x000000), 0.22f);
        else bg = mix(accent, col(0xFFFFFF), h * 0.12f);
        rect(r, bg, 7);
        text(r, lt, sz, col(0x0B0E12), 1, true);
    } else {
        rect(r, mix(kBtnBg, kBtnHover, h), 7);
        frameRect(r, mix(kBtnBorder, accent, h * 0.8f), 7);
        if (held) rect(r, col(0x000000, 0.18f), 7);
        text(r, lt, sz, mix(kTextDim, kText, h), 1, true);
    }
    return in.pressed && hit(r, in.pressX, in.pressY);
}

bool Ui::toggle(int id, const D2D1_RECT_F& r, const std::wstring& label, bool on,
                D2D1_COLOR_F accent) {
    const bool over = hit(r, in.mx, in.my);
    const float h = hover(id, over);
    D2D1_COLOR_F offBg = mix(kBtnBg, kBtnHover, h);
    D2D1_COLOR_F onBg = D2D1_COLOR_F{accent.r, accent.g, accent.b, 0.20f + h * 0.08f};
    rect(r, on ? onBg : offBg, 7);
    frameRect(r, on ? accent : mix(kBtnBorder, accent, h * 0.6f), 7);
    const std::wstring lt = uiLanguage() ? uiTr(label) : label;
    text(r, lt, fitSize(lt, r.right - r.left - 12, 13), on ? accent
                                                           : mix(kTextDim, kText, h),
         1, true);
    return in.pressed && hit(r, in.pressX, in.pressY);
}

bool Ui::sliderH(int id, const D2D1_RECT_F& r, float& v, D2D1_COLOR_F accent,
                 bool fillFromCenter, const D2D1_COLOR_F* leftAccent) {
    if (in.pressed && hit(r, in.pressX, in.pressY)) active_ = id;
    if (!in.down && active_ == id) active_ = 0;
    const float pad = 9.f;
    const float w = r.right - r.left - pad * 2;
    bool changed = false;
    if (active_ == id) {
        const float nv = std::clamp((in.mx - r.left - pad) / w, 0.f, 1.f);
        changed = nv != v;
        v = nv;
    }
    const float h = hover(id, hit(r, in.mx, in.my) || active_ == id);
    const float cy = (r.top + r.bottom) / 2;
    rect(rc(r.left + pad, cy - 2.5f, w, 5), kTrack, 2.5f);
    const float tx = r.left + pad + v * w;
    const D2D1_COLOR_F fill{accent.r, accent.g, accent.b, 0.65f};
    if (fillFromCenter) { // neutral at center; fill grows toward the thumb
        const D2D1_COLOR_F la = leftAccent ? *leftAccent : accent;
        const D2D1_COLOR_F fillL{la.r, la.g, la.b, 0.65f};
        const float mid = r.left + pad + w * 0.5f;
        if (tx > mid + 0.5f) rect(rc(mid, cy - 2.5f, tx - mid, 5), fill, 2.5f);
        else if (tx < mid - 0.5f) rect(rc(tx, cy - 2.5f, mid - tx, 5), fillL, 2.5f);
    } else {
        rect(rc(r.left + pad, cy - 2.5f, tx - r.left - pad, 5), fill, 2.5f);
    }
    const float tw = 14 + h * 2, th = 24 + h * 2;
    rect(rc(tx - tw / 2, cy - th / 2, tw, th),
         active_ == id ? accent : mix(kThumb, accent, h * 0.35f), 5);
    return changed;
}

bool Ui::sliderV(int id, const D2D1_RECT_F& r, float& v, D2D1_COLOR_F accent) {
    if (in.pressed && hit(r, in.pressX, in.pressY)) active_ = id;
    if (!in.down && active_ == id) active_ = 0;
    const float pad = 9.f;
    const float h = r.bottom - r.top - pad * 2;
    bool changed = false;
    if (active_ == id) {
        const float nv = std::clamp(1.f - (in.my - r.top - pad) / h, 0.f, 1.f);
        changed = nv != v;
        v = nv;
    }
    const float hv = hover(id, hit(r, in.mx, in.my) || active_ == id);
    const float cx = (r.left + r.right) / 2;
    rect(rc(cx - 2.5f, r.top + pad, 5, h), kTrack, 2.5f);
    const float ty = r.top + pad + (1.f - v) * h;
    rect(rc(cx - 2.5f, ty, 5, r.bottom - pad - ty),
         D2D1_COLOR_F{accent.r, accent.g, accent.b, 0.65f}, 2.5f);
    const float tw = 24 + hv * 2, th = 14 + hv * 2;
    rect(rc(cx - tw / 2, ty - th / 2, tw, th),
         active_ == id ? accent : mix(kThumb, accent, hv * 0.35f), 5);
    return changed;
}

