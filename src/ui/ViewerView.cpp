#include "ui/ViewerView.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "res/resource.h"
#include "ui/Theme.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClassName[] = L"LiteViewViewer";
constexpr double kZoomStep = 1.25;
constexpr double kMinScale = 0.02;
constexpr double kMaxScale = 32.0;

std::wstring LoadStr(UINT id) {
    const wchar_t* text = nullptr;
    const int len = LoadStringW(GetModuleHandleW(nullptr), id, reinterpret_cast<LPWSTR>(&text), 0);
    return (len > 0 && text) ? std::wstring(text, static_cast<size_t>(len)) : std::wstring();
}

}  // namespace

ViewerView::~ViewerView() = default;

HRESULT ViewerView::Create(HWND parent, HINSTANCE instance, Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf());
    if (FAILED(hr)) return hr;
    WNDCLASSEXW wc = {sizeof(wc)};
    if (!GetClassInfoExW(instance, kClassName, &wc)) {
        wc = {sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &ViewerView::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return HRESULT_FROM_WIN32(GetLastError());
    }
    hwnd_ = CreateWindowExW(0, kClassName, nullptr, WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, parent, nullptr, instance, this);
    return hwnd_ ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

LRESULT CALLBACK ViewerView::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ViewerView* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<ViewerView*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ViewerView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (msg == WM_NCDESTROY && self) {
        self->DiscardTarget();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return self ? self->HandleMessage(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

SIZE ViewerView::ViewSize() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    return SIZE{rc.right, rc.bottom};
}

LRESULT ViewerView::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT:
        OnPaint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (target_) target_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        ClampCenter();
        AfterViewChange();
        return 0;
    case WM_DISPLAYCHANGE:
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &pt);
            const double scale = EffectiveScale() * (delta > 0 ? kZoomStep : 1.0 / kZoomStep);
            ZoomTo(scale, pt);
            return 0;
        }
        wheelRemainder_ += delta;
        while (wheelRemainder_ >= WHEEL_DELTA) {
            wheelRemainder_ -= WHEEL_DELTA;
            if (callbacks_.navigate) callbacks_.navigate(-1);
        }
        while (wheelRemainder_ <= -WHEEL_DELTA) {
            wheelRemainder_ += WHEEL_DELTA;
            if (callbacks_.navigate) callbacks_.navigate(+1);
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (callbacks_.exit) callbacks_.exit();
        return 0;
    case WM_MBUTTONDOWN:
        ToggleFit();
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd_);
        dragging_ = true;
        dragLast_ = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetCapture(hwnd_);
        return 0;
    case WM_MOUSEMOVE:
        if (dragging_ && GetCapture() == hwnd_) {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const double scale = EffectiveScale();
            if (scale > 0) {
                centerX_ -= (pt.x - dragLast_.x) / scale;
                centerY_ -= (pt.y - dragLast_.y) / scale;
                ClampCenter();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            dragLast_ = pt;
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        dragging_ = false;
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && image_) {
            const SIZE view = ViewSize();
            const double scale = EffectiveScale();
            const bool scrollable = image_->originalWidth * scale > view.cx + 0.5 ||
                                    image_->originalHeight * scale > view.cy + 0.5;
            SetCursor(LoadCursorW(nullptr, scrollable ? IDC_SIZEALL : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_KEYDOWN: {
        // Arrows pan a zoomed picture, otherwise they change pictures.
        const SIZE view = ViewSize();
        const double scale = EffectiveScale();
        const bool wide = image_ && image_->originalWidth * scale > view.cx + 0.5;
        const bool tall = image_ && image_->originalHeight * scale > view.cy + 0.5;
        const double step = 64.0 / (scale > 0 ? scale : 1.0);
        switch (wParam) {
        case VK_LEFT:
            if (wide) centerX_ -= step; else if (callbacks_.navigate) callbacks_.navigate(-1);
            break;
        case VK_RIGHT:
            if (wide) centerX_ += step; else if (callbacks_.navigate) callbacks_.navigate(+1);
            break;
        case VK_UP:
            if (tall) centerY_ -= step; else if (callbacks_.navigate) callbacks_.navigate(-1);
            break;
        case VK_DOWN:
            if (tall) centerY_ += step; else if (callbacks_.navigate) callbacks_.navigate(+1);
            break;
        default:
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }
        ClampCenter();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void ViewerView::SetImage(const std::wstring& path, ViewerImagePtr image, ThumbnailPtr placeholder, bool failed) {
    const bool samePath = !path_.empty() &&
                          CompareStringOrdinal(path.c_str(), -1, path_.c_str(), -1, TRUE) == CSTR_EQUAL;
    const bool hadImage = static_cast<bool>(image_);
    const bool sizeChanged = hadImage && image &&
                             (image->originalWidth != image_->originalWidth || image->originalHeight != image_->originalHeight);
    path_ = path;
    if (image) {
        if (image != image_) pendingTurns_ = 0;  // a fresh decode already shows the saved rotation
        image_ = std::move(image);
        placeholder_.reset();
    } else if (!samePath) {
        image_.reset();
        placeholder_ = std::move(placeholder);
    }
    failed_ = failed && !image_;
    if (!samePath) pendingTurns_ = 0;
    if (!samePath || !hadImage || sizeChanged) {
        // A new picture starts fitted and centred.
        fit_ = true;
        scale_ = 1.0;
        if (image_) {
            centerX_ = image_->originalWidth / 2.0;
            centerY_ = image_->originalHeight / 2.0;
        }
    }
    ClampCenter();
    InvalidateRect(hwnd_, nullptr, FALSE);
    AfterViewChange();
}

void ViewerView::Clear() {
    path_.clear();
    image_.reset();
    placeholder_.reset();
    bitmap_.Reset();
    bitmapSource_ = nullptr;
    failed_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ViewerView::RotatePending(bool clockwise) {
    if (!image_) return;
    pendingTurns_ = (pendingTurns_ + (clockwise ? 1 : 3)) % 4;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ViewerView::SetFullscreen(bool fullscreen) {
    fullscreen_ = fullscreen;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

double ViewerView::FitScale() const {
    if (!image_ || image_->originalWidth == 0 || image_->originalHeight == 0) return 1.0;
    const SIZE view = ViewSize();
    if (view.cx <= 0 || view.cy <= 0) return 1.0;
    return std::min({1.0, static_cast<double>(view.cx) / image_->originalWidth,
                     static_cast<double>(view.cy) / image_->originalHeight});
}

double ViewerView::EffectiveScale() const { return fit_ ? FitScale() : scale_; }

bool ViewerView::NeedsFullSize() const {
    if (!image_ || image_->IsFullSize()) return false;
    // Displayed larger than the decoded pixels (5% slack for rounding).
    return EffectiveScale() * image_->originalWidth > image_->width * 1.05;
}

void ViewerView::ZoomIn() {
    const SIZE view = ViewSize();
    ZoomTo(EffectiveScale() * kZoomStep, POINT{view.cx / 2, view.cy / 2});
}

void ViewerView::ZoomOut() {
    const SIZE view = ViewSize();
    ZoomTo(EffectiveScale() / kZoomStep, POINT{view.cx / 2, view.cy / 2});
}

void ViewerView::ToggleFit() {
    if (!image_) return;
    if (fit_ && FitScale() < 1.0) {
        fit_ = false;
        scale_ = 1.0;  // actual pixels, keeping the centre
    } else {
        fit_ = true;
        centerX_ = image_->originalWidth / 2.0;
        centerY_ = image_->originalHeight / 2.0;
    }
    ClampCenter();
    InvalidateRect(hwnd_, nullptr, FALSE);
    AfterViewChange();
}

void ViewerView::ZoomTo(double scale, POINT anchor) {
    if (!image_) return;
    scale = std::clamp(scale, kMinScale, kMaxScale);
    const SIZE view = ViewSize();
    const double old = EffectiveScale();
    // Keep the picture point under the anchor where it is.
    const double ix = centerX_ + (anchor.x - view.cx / 2.0) / old;
    const double iy = centerY_ + (anchor.y - view.cy / 2.0) / old;
    fit_ = false;
    scale_ = scale;
    centerX_ = ix - (anchor.x - view.cx / 2.0) / scale;
    centerY_ = iy - (anchor.y - view.cy / 2.0) / scale;
    ClampCenter();
    InvalidateRect(hwnd_, nullptr, FALSE);
    AfterViewChange();
}

void ViewerView::ClampCenter() {
    if (!image_) return;
    const SIZE view = ViewSize();
    const double scale = EffectiveScale();
    const double w = image_->originalWidth, h = image_->originalHeight;
    const double halfW = view.cx / 2.0 / scale, halfH = view.cy / 2.0 / scale;
    centerX_ = (w * scale <= view.cx) ? w / 2.0 : std::clamp(centerX_, halfW, w - halfW);
    centerY_ = (h * scale <= view.cy) ? h / 2.0 : std::clamp(centerY_, halfH, h - halfH);
}

void ViewerView::AfterViewChange() {
    if (NeedsFullSize() && callbacks_.needFullSize) callbacks_.needFullSize();
    if (callbacks_.viewChanged) callbacks_.viewChanged();
}

HRESULT ViewerView::EnsureTarget() {
    if (target_) return S_OK;
    const SIZE view = ViewSize();
    // 96 DPI: one DIP is one pixel, so layout math stays in pixels.
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    HRESULT hr = factory_->CreateHwndRenderTarget(
        props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(std::max<LONG>(1, view.cx), std::max<LONG>(1, view.cy))),
        target_.GetAddressOf());
    return hr;
}

void ViewerView::DiscardTarget() {
    bitmap_.Reset();
    bitmapSource_ = nullptr;
    target_.Reset();
}

HRESULT ViewerView::EnsureBitmap() {
    const void* source = image_ ? static_cast<const void*>(image_.get()) : static_cast<const void*>(placeholder_.get());
    if (source == bitmapSource_ && (bitmap_ || !source)) return S_OK;
    bitmap_.Reset();
    bitmapSource_ = source;
    if (!source) return S_OK;
    const D2D1_BITMAP_PROPERTIES props =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (image_) {
        return target_->CreateBitmap(D2D1::SizeU(image_->width, image_->height), image_->pixels.data(),
                                     image_->width * 4, props, bitmap_.GetAddressOf());
    }
    DIBSECTION dib{};
    if (GetObjectW(placeholder_->bitmap, sizeof(dib), &dib) != sizeof(dib) || !dib.dsBm.bmBits) return E_FAIL;
    return target_->CreateBitmap(D2D1::SizeU(placeholder_->width, placeholder_->height), dib.dsBm.bmBits,
                                 static_cast<UINT32>(dib.dsBm.bmWidthBytes), props, bitmap_.GetAddressOf());
}

void ViewerView::OnPaint() {
    PAINTSTRUCT ps;
    HDC paintDc = BeginPaint(hwnd_, &ps);
    if (FAILED(EnsureTarget())) {
        // No Direct2D (e.g. a remote session without a driver): at least
        // keep the area clean.
        HBRUSH brush = CreateSolidBrush(fullscreen_ ? RGB(0, 0, 0) : theme::Current().viewer);
        FillRect(paintDc, &ps.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(hwnd_, &ps);
        return;
    }
    EndPaint(hwnd_, &ps);
    EnsureBitmap();

    const SIZE view = ViewSize();
    const D2D1_SIZE_U pixels = target_->GetPixelSize();
    if (pixels.width != static_cast<UINT32>(view.cx) || pixels.height != static_cast<UINT32>(view.cy)) {
        target_->Resize(D2D1::SizeU(std::max<LONG>(1, view.cx), std::max<LONG>(1, view.cy)));
    }
    target_->BeginDraw();
    const COLORREF background = fullscreen_ ? RGB(0, 0, 0) : theme::Current().viewer;
    target_->Clear(D2D1::ColorF(GetRValue(background) / 255.0f, GetGValue(background) / 255.0f,
                                GetBValue(background) / 255.0f));
    if (bitmap_ && image_ && pendingTurns_ != 0) {
        // Turned preview: fit the rotated picture, ignoring zoom.
        const bool sideways = pendingTurns_ % 2 != 0;
        const double w = image_->originalWidth, h = image_->originalHeight;
        const double scale = std::min({1.0, view.cx / (sideways ? h : w), view.cy / (sideways ? w : h)});
        const D2D1_RECT_F dest = D2D1::RectF(static_cast<float>(view.cx / 2.0 - w * scale / 2),
                                             static_cast<float>(view.cy / 2.0 - h * scale / 2),
                                             static_cast<float>(view.cx / 2.0 + w * scale / 2),
                                             static_cast<float>(view.cy / 2.0 + h * scale / 2));
        target_->SetTransform(D2D1::Matrix3x2F::Rotation(90.0f * pendingTurns_,
                                                         D2D1::Point2F(view.cx / 2.0f, view.cy / 2.0f)));
        target_->DrawBitmap(bitmap_.Get(), dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        target_->SetTransform(D2D1::Matrix3x2F::Identity());
    } else if (bitmap_ && image_) {
        const double scale = EffectiveScale();
        const double w = image_->originalWidth * scale, h = image_->originalHeight * scale;
        const double left = view.cx / 2.0 - centerX_ * scale;
        const double top = view.cy / 2.0 - centerY_ * scale;
        // Snap to whole pixels so 100% stays sharp.
        const D2D1_RECT_F dest = D2D1::RectF(static_cast<float>(std::floor(left)), static_cast<float>(std::floor(top)),
                                             static_cast<float>(std::floor(left) + std::round(w)),
                                             static_cast<float>(std::floor(top) + std::round(h)));
        target_->DrawBitmap(bitmap_.Get(), dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else if (bitmap_ && placeholder_) {
        // Enlarged thumbnail until the real decode arrives.
        const double scale = std::min(static_cast<double>(view.cx) / placeholder_->width,
                                      static_cast<double>(view.cy) / placeholder_->height);
        const double w = placeholder_->width * scale, h = placeholder_->height * scale;
        const D2D1_RECT_F dest = D2D1::RectF(static_cast<float>((view.cx - w) / 2), static_cast<float>((view.cy - h) / 2),
                                             static_cast<float>((view.cx + w) / 2), static_cast<float>((view.cy + h) / 2));
        target_->DrawBitmap(bitmap_.Get(), dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    const HRESULT hr = target_->EndDraw();
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        DiscardTarget();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (failed_) {
        // The message is drawn with GDI after Direct2D has presented.
        HDC dc = GetDC(hwnd_);
        const std::wstring text = LoadStr(IDS_VIEWER_ERROR);
        RECT rc{0, 0, view.cx, view.cy};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, fullscreen_ ? RGB(220, 220, 220) : theme::Current().text);
        HGDIOBJ old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(dc, text.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
    }
}
