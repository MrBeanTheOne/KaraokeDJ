#include "video/video_window.h"

#include "qrcodegen.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <algorithm>
#include <vector>

#include "media/mf_video_decoder.h"

namespace {
struct MonRects {
    std::vector<RECT> rects;
};
BOOL CALLBACK monEnum(HMONITOR, HDC, LPRECT r, LPARAM p) {
    reinterpret_cast<MonRects*>(p)->rects.push_back(*r);
    return TRUE;
}
D2D1_RENDER_TARGET_PROPERTIES rtProps() {
    auto p = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    p.dpiX = p.dpiY = 96.f; // 1 unit = 1 physical pixel on scaled desktops
    return p;
}
} // namespace

int VideoWindow::monitorCount() {
    MonRects mons;
    EnumDisplayMonitors(nullptr, nullptr, monEnum, reinterpret_cast<LPARAM>(&mons));
    return int(mons.rects.size());
}

RECT VideoWindow::monitorRect(int index) {
    MonRects mons;
    EnumDisplayMonitors(nullptr, nullptr, monEnum, reinterpret_cast<LPARAM>(&mons));
    if (index < 0 || size_t(index) >= mons.rects.size()) return RECT{};
    return mons.rects[index];
}

LRESULT CALLBACK VideoWindow::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<VideoWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_KEYDOWN:
        if (!self) return 0;
        if (wp == VK_ESCAPE) self->quit_ = true;
        else self->key_.store(int(wp));
        return 0;
    case WM_SIZE:
        if (self && self->rt_) {
            D2D1_SIZE_U s{LOWORD(lp), HIWORD(lp)};
            self->rt_->Resize(s);
        }
        return 0;
    case WM_CLOSE:
    case WM_DESTROY:
        if (self) self->quit_ = true;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool VideoWindow::create(int monitor, int cascade) {
    SetProcessDPIAware();
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"KaraokeVideoOut";
    RegisterClassW(&wc); // repeated registration fails harmlessly

    const int off = 100 + cascade * 60;
    RECT r{off, off, off + 960, off + 540};
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (monitor >= 0) {
        MonRects mons;
        EnumDisplayMonitors(nullptr, nullptr, monEnum, reinterpret_cast<LPARAM>(&mons));
        if (size_t(monitor) >= mons.rects.size()) return false;
        r = mons.rects[monitor];
        style = WS_POPUP;
    }
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"Video Output", style, r.left, r.top,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                            wc.hInstance, nullptr);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ShowWindow(hwnd_, SW_SHOW);

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_))) return false;
    return ensureTarget();
}

bool VideoWindow::ensureTarget() {
    if (rt_) return true;
    RECT cr{};
    GetClientRect(hwnd_, &cr);
    return SUCCEEDED(factory_->CreateHwndRenderTarget(
        rtProps(),
        D2D1::HwndRenderTargetProperties(
            hwnd_, D2D1::SizeU(cr.right - cr.left, cr.bottom - cr.top)),
        &rt_));
}

void VideoWindow::setFrame(int slot, const VideoFrame& f) {
    if (!ensureTarget() || slot < 0 || slot > 1) return;
    if (!bmp_[slot] || bw_[slot] != f.width || bh_[slot] != f.height) {
        if (bmp_[slot]) { bmp_[slot]->Release(); bmp_[slot] = nullptr; }
        const auto props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(rt_->CreateBitmap(D2D1::SizeU(f.width, f.height), props, &bmp_[slot])))
            return;
        bw_[slot] = f.width;
        bh_[slot] = f.height;
    }
    bmp_[slot]->CopyFromMemory(nullptr, f.bgra.data(), f.width * 4);
    ++presented;
}

void VideoWindow::drawSlot(int slot, float alpha) {
    if (slot < 0 || slot > 1 || !bmp_[slot] || alpha <= 0.f) return;
    const D2D1_SIZE_F s = rt_->GetSize();
    float w, h;
    if (fit_ == 2) { // stretch: ignore aspect ratio
        w = s.width;
        h = s.height;
    } else { // fit (letterbox) or fill (crop): keep aspect ratio
        const float scale = fit_ == 1
                                ? (std::max)(s.width / bw_[slot], s.height / bh_[slot])
                                : (std::min)(s.width / bw_[slot], s.height / bh_[slot]);
        w = bw_[slot] * scale;
        h = bh_[slot] * scale;
    }
    const float x = (s.width - w) / 2, y = (s.height - h) / 2;
    rt_->DrawBitmap(bmp_[slot], D2D1::RectF(x, y, x + w, y + h), alpha,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void VideoWindow::setQr(const std::wstring& url) {
    if (url == qrUrl_) return;
    qrUrl_ = url;
    qrMods_.clear();
    qrSize_ = 0;
    if (url.empty()) return;
    std::string u8(url.size() * 3, 0); // the url is plain ASCII in practice
    const int n = WideCharToMultiByte(CP_UTF8, 0, url.c_str(), int(url.size()),
                                      u8.data(), int(u8.size()), nullptr, nullptr);
    if (n <= 0) return;
    u8.resize(n);
    try {
        const auto qr = qrcodegen::QrCode::encodeText(
            u8.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        qrSize_ = qr.getSize();
        qrMods_.resize(size_t(qrSize_) * qrSize_);
        for (int y = 0; y < qrSize_; ++y)
            for (int x = 0; x < qrSize_; ++x)
                qrMods_[size_t(y) * qrSize_ + x] = qr.getModule(x, y) ? 1 : 0;
    } catch (...) {
        qrSize_ = 0;
        qrMods_.clear();
    }
}

bool loadImageFile(const std::wstring& path, VideoFrame& out, int maxW) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return false;
    bool ok = false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapScaler* sc = nullptr;
    IWICFormatConverter* cv = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr,
                                                 GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand,
                                                 &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&w, &h)) &&
        w && h) {
        IWICBitmapSource* src = fr;
        if (w > UINT(maxW)) { // keeps upload + memory small
            const UINT nw = UINT(maxW),
                       nh = (std::max)(1u, UINT(uint64_t(h) * maxW / w));
            if (SUCCEEDED(wic->CreateBitmapScaler(&sc)) &&
                SUCCEEDED(sc->Initialize(fr, nw, nh,
                                         WICBitmapInterpolationModeFant))) {
                src = sc;
                w = nw;
                h = nh;
            }
        }
        // Premultiplied BGRA: what D2D wants for alpha-blended DrawBitmap.
        if (SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
            SUCCEEDED(cv->Initialize(src, GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom))) {
            out.width = w;
            out.height = h;
            out.bgra.assign(size_t(w) * h * 4, 0);
            ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, UINT(out.bgra.size()),
                                          out.bgra.data()));
        }
    }
    if (cv) cv->Release();
    if (sc) sc->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    wic->Release();
    return ok;
}

void VideoWindow::setIdleScene(const IdleScene& s) {
    scene_ = s;
    setQr(s.qrUrl);
}

void VideoWindow::setLogo(const VideoFrame& f) {
    if (!ensureTarget() || !f.width || !f.height) return;
    if (logo_) { logo_->Release(); logo_ = nullptr; }
    const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(
        DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(rt_->CreateBitmap(D2D1::SizeU(f.width, f.height), f.bgra.data(),
                                 f.width * 4, props, &logo_))) {
        logo_ = nullptr;
        return;
    }
    lw_ = f.width;
    lh_ = f.height;
}

void VideoWindow::clearLogo() {
    if (logo_) { logo_->Release(); logo_ = nullptr; }
}

void VideoWindow::setBackground(const VideoFrame& f) {
    if (!ensureTarget() || !f.width || !f.height) return;
    if (bg_) { bg_->Release(); bg_ = nullptr; }
    const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(
        DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(rt_->CreateBitmap(D2D1::SizeU(f.width, f.height), f.bgra.data(),
                                 f.width * 4, props, &bg_))) {
        bg_ = nullptr;
        return;
    }
    bgw_ = f.width;
    bgh_ = f.height;
}

void VideoWindow::clearBackground() {
    if (bg_) { bg_->Release(); bg_ = nullptr; }
}

// The waiting screen: each enabled element renders anchored to its 3x3 grid
// cell. Text spans the full width and uses its column as alignment, so long
// titles stay readable; boxes (logo, QR) pin into the cell's corner.
void VideoWindow::drawIdle() {
    if (!dw_ &&
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                   __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dw_))))
        return;
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(rt_->CreateSolidColorBrush(D2D1::ColorF(0xE8ECF1), &brush))) return;
    const D2D1_SIZE_F s = rt_->GetSize();
    if (bg_ && bgw_ && bgh_) { // fill-crop, then a scrim keeps text readable
        const float sc = (std::max)(s.width / bgw_, s.height / bgh_);
        const float bw2 = bgw_ * sc, bh2 = bgh_ * sc;
        rt_->DrawBitmap(bg_,
                        D2D1::RectF((s.width - bw2) / 2, (s.height - bh2) / 2,
                                    (s.width + bw2) / 2, (s.height + bh2) / 2),
                        1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        brush->SetColor(D2D1::ColorF(0, 0.45f));
        rt_->FillRectangle(D2D1::RectF(0, 0, s.width, s.height), brush);
    }
    const float mg = s.height * 0.05f;
    const auto sizeMul = [](int sz) {
        return sz == 0 ? 0.65f : sz == 2 ? 1.5f : 1.f;
    };
    const auto band = [&](int row) { // one horizontal third of the screen
        const float h3 = (s.height - 2 * mg) / 3;
        return D2D1::RectF(mg, mg + row * h3, s.width - mg, mg + (row + 1) * h3);
    };
    const auto text = [&](const std::wstring& t, const IdleElem& e, float base,
                          DWRITE_FONT_WEIGHT wgt, UINT32 color) {
        if (!e.on || t.empty()) return;
        const float sz = (std::max)(12.f, s.height * base * sizeMul(e.size));
        IDWriteTextFormat* f = nullptr;
        if (FAILED(dw_->CreateTextFormat(L"Segoe UI", nullptr, wgt,
                                         DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, sz, L"",
                                         &f)))
            return;
        const int c = e.pos % 3, r = e.pos / 3;
        f->SetTextAlignment(c == 0   ? DWRITE_TEXT_ALIGNMENT_LEADING
                            : c == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                     : DWRITE_TEXT_ALIGNMENT_CENTER);
        f->SetParagraphAlignment(r == 0   ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                                 : r == 2 ? DWRITE_PARAGRAPH_ALIGNMENT_FAR
                                          : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        brush->SetColor(D2D1::ColorF(color));
        rt_->DrawTextW(t.c_str(), UINT32(t.size()), f, band(r), brush);
        f->Release();
    };

    // Logo (aspect-fit into a height budget, anchored to its cell corner).
    const IdleElem& lg = scene_.elems[IdleScene::kLogo];
    if (lg.on && logo_ && lh_ > 0) {
        float bh = s.height * 0.22f * sizeMul(lg.size);
        float bw2 = bh * float(lw_) / float(lh_);
        const float maxW = s.width * 0.6f;
        if (bw2 > maxW) { bh *= maxW / bw2; bw2 = maxW; }
        const int c = lg.pos % 3, r = lg.pos / 3;
        const float x = c == 0 ? mg : c == 2 ? s.width - mg - bw2
                                             : (s.width - bw2) / 2;
        const float y = r == 0 ? mg : r == 2 ? s.height - mg - bh
                                             : (s.height - bh) / 2;
        rt_->DrawBitmap(logo_, D2D1::RectF(x, y, x + bw2, y + bh), 1.f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    text(scene_.title, scene_.elems[IdleScene::kTitle], 0.075f,
         DWRITE_FONT_WEIGHT_SEMI_BOLD, 0xE8ECF1);
    text(scene_.message, scene_.elems[IdleScene::kMessage], 0.038f,
         DWRITE_FONT_WEIGHT_NORMAL, 0xC9CDD3);
    text(scene_.nextUp, scene_.elems[IdleScene::kNextUp], 0.034f,
         DWRITE_FONT_WEIGHT_NORMAL, 0x4FC3F7);
    if (!scene_.singers.empty()) {
        std::wstring block = L"UP NEXT";
        for (const std::wstring& ln : scene_.singers) block += L"\n" + ln;
        text(block, scene_.elems[IdleScene::kSingers], 0.030f,
             DWRITE_FONT_WEIGHT_NORMAL, 0xE8ECF1);
    }

    // Phone-request QR: white card, black modules, caption + url underneath.
    const IdleElem& eq = scene_.elems[IdleScene::kQr];
    if (eq.on && qrSize_ > 0) {
        const float frac = 0.26f * sizeMul(eq.size);
        const float cell = (std::max)(2.f, s.height * frac / float(qrSize_ + 8));
        const float qw = cell * (qrSize_ + 8); // 4-module quiet zone each side
        const float szQ = (std::max)(11.f, s.height * 0.020f);
        const float capH = szQ * 3.4f;
        const int c = eq.pos % 3, r = eq.pos / 3;
        const float qx = c == 0 ? mg : c == 2 ? s.width - mg - qw
                                              : (s.width - qw) / 2;
        const float qy = r == 0   ? mg
                         : r == 2 ? s.height - mg - qw - capH
                                  : (s.height - qw - capH) / 2;
        brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
        rt_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(qx, qy, qx + qw, qy + qw), cell * 2,
                              cell * 2),
            brush);
        brush->SetColor(D2D1::ColorF(D2D1::ColorF::Black));
        const float ox = qx + cell * 4, oy = qy + cell * 4;
        for (int my = 0; my < qrSize_; ++my)
            for (int mx = 0; mx < qrSize_; ++mx)
                if (qrMods_[size_t(my) * qrSize_ + mx])
                    rt_->FillRectangle(
                        D2D1::RectF(ox + mx * cell, oy + my * cell,
                                    ox + (mx + 1) * cell + 0.5f,
                                    oy + (my + 1) * cell + 0.5f),
                        brush);
        IDWriteTextFormat* qf = nullptr;
        if (SUCCEEDED(dw_->CreateTextFormat(
                L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, szQ, L"",
                &qf))) {
            qf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            brush->SetColor(D2D1::ColorF(0xE8ECF1));
            const std::wstring cap = L"SCAN TO REQUEST A SONG";
            rt_->DrawTextW(cap.c_str(), UINT32(cap.size()), qf,
                           D2D1::RectF(qx - 80, qy + qw + 6, qx + qw + 80,
                                       qy + qw + 6 + szQ * 1.5f),
                           brush);
            brush->SetColor(D2D1::ColorF(0x9BA3AD));
            rt_->DrawTextW(qrUrl_.c_str(), UINT32(qrUrl_.size()), qf,
                           D2D1::RectF(qx - 80, qy + qw + 6 + szQ * 1.6f,
                                       qx + qw + 80, qy + qw + 6 + szQ * 3.2f),
                           brush);
            qf->Release();
        }
    }
    brush->Release();
}

void VideoWindow::draw(int baseSlot, int topSlot, float topAlpha) {
    if (!ensureTarget()) return;
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    const bool anyVideo = (baseSlot >= 0 && bmp_[baseSlot]) ||
                          (topSlot >= 0 && bmp_[topSlot] && topAlpha > 0.f);
    if (!anyVideo) drawIdle();
    drawSlot(baseSlot, 1.f);
    drawSlot(topSlot, topAlpha);
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        releaseTarget();
        lost_ = true; // caller re-uploads its current frames
    }
}

bool VideoWindow::pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !quit_;
}

void VideoWindow::releaseTarget() {
    for (auto& b : bmp_)
        if (b) { b->Release(); b = nullptr; }
    if (logo_) { logo_->Release(); logo_ = nullptr; }
    if (bg_) { bg_->Release(); bg_ = nullptr; }
    bw_[0] = bw_[1] = bh_[0] = bh_[1] = 0;
    if (rt_) { rt_->Release(); rt_ = nullptr; }
}

void VideoWindow::destroy() {
    releaseTarget();
    if (dw_) { dw_->Release(); dw_ = nullptr; }
    if (factory_) { factory_->Release(); factory_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}
