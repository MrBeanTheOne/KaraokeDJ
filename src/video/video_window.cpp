#include "video/video_window.h"

#include "qrcodegen.hpp"

#include <d2d1.h>
#include <dwrite.h>

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

void VideoWindow::drawIdle() {
    if (idleTitle_.empty() && idleDetail_.empty()) return;
    if (!dw_ &&
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dw_))))
        return;
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(rt_->CreateSolidColorBrush(D2D1::ColorF(0xE8ECF1), &brush))) return;
    const D2D1_SIZE_F s = rt_->GetSize();
    const float szBig = (std::max)(28.f, s.height * 0.09f);
    const float szSmall = (std::max)(16.f, s.height * 0.045f);
    IDWriteTextFormat* f = nullptr;
    if (SUCCEEDED(dw_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                        DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, szBig, L"", &f))) {
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        rt_->DrawTextW(idleTitle_.c_str(), UINT32(idleTitle_.size()), f,
                       D2D1::RectF(0, 0, s.width, s.height - szBig), brush);
        f->Release();
    }
    if (!idleDetail_.empty() &&
        SUCCEEDED(dw_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, szSmall, L"", &f))) {
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        brush->SetColor(D2D1::ColorF(0x4FC3F7));
        rt_->DrawTextW(idleDetail_.c_str(), UINT32(idleDetail_.size()), f,
                       D2D1::RectF(0, s.height / 2, s.width, s.height), brush);
        f->Release();
    }
    // Phone-request QR, bottom-right: white card, black modules, url + hint.
    if (qrSize_ > 0) {
        const float cell = (std::max)(2.f, s.height * 0.30f / float(qrSize_ + 8));
        const float qw = cell * (qrSize_ + 8); // 4-module quiet zone each side
        const float qx = s.width - qw - s.height * 0.04f;
        const float qy = s.height - qw - s.height * 0.10f;
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
        const float szQ = (std::max)(11.f, s.height * 0.022f);
        if (dw_ && SUCCEEDED(dw_->CreateTextFormat(
                       L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                       szQ, L"", &qf))) {
            qf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            brush->SetColor(D2D1::ColorF(0xE8ECF1));
            const std::wstring cap = L"SCAN TO REQUEST A SONG";
            rt_->DrawTextW(cap.c_str(), UINT32(cap.size()), qf,
                           D2D1::RectF(qx - 60, qy + qw + 6, qx + qw + 60,
                                       qy + qw + 6 + szQ * 1.5f),
                           brush);
            brush->SetColor(D2D1::ColorF(0x9BA3AD));
            rt_->DrawTextW(qrUrl_.c_str(), UINT32(qrUrl_.size()), qf,
                           D2D1::RectF(qx - 60, qy + qw + 6 + szQ * 1.6f,
                                       qx + qw + 60, qy + qw + 6 + szQ * 3.2f),
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
    bw_[0] = bw_[1] = bh_[0] = bh_[1] = 0;
    if (rt_) { rt_->Release(); rt_ = nullptr; }
}

void VideoWindow::destroy() {
    releaseTarget();
    if (dw_) { dw_->Release(); dw_ = nullptr; }
    if (factory_) { factory_->Release(); factory_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}
