#include "ui/ModelView.h"

#include <windowsx.h>

#include <algorithm>
#include <utility>

#include "core/ThreadPool.h"
#include "res/resource.h"
#include "ui/Messages.h"
#include "ui/Theme.h"

namespace {

constexpr wchar_t kClassName[] = L"LiteViewModelView";
constexpr UINT_PTR kSpinTimer = 1;
constexpr UINT kSpinInterval = 30;            // ms
constexpr float kSpinSpeed = 0.5f;            // radians per second
constexpr float kDragSpeed = 0.01f;           // radians per pixel
constexpr float kStep = 3.14159265f / 12.0f;  // 15 degrees
constexpr float kZoomStep = 1.25f;
constexpr DWORD kSlowFrame = 150;             // ms; stop spinning a model this heavy

std::wstring LoadStr(UINT id) {
    const wchar_t* text = nullptr;
    const int length = LoadStringW(GetModuleHandleW(nullptr), id, reinterpret_cast<LPWSTR>(&text), 0);
    return length > 0 ? std::wstring(text, static_cast<std::size_t>(length)) : std::wstring();
}

}  // namespace

ModelView::ModelView() : shared_(std::make_shared<Shared>()), pool_(std::make_unique<ThreadPool>(1)) {}

ModelView::~ModelView() {
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->hwnd = nullptr;
    }
    pool_->Shutdown();
}

HRESULT ModelView::Create(HWND parent, HINSTANCE instance, Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
    WNDCLASSEXW wc = {sizeof(wc)};
    if (!GetClassInfoExW(instance, kClassName, &wc)) {
        wc = {sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &ModelView::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return HRESULT_FROM_WIN32(GetLastError());
    }
    hwnd_ = CreateWindowExW(0, kClassName, nullptr, WS_CHILD, 0, 0, 0, 0, parent, nullptr, instance, this);
    if (!hwnd_) return HRESULT_FROM_WIN32(GetLastError());
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->hwnd = hwnd_;
    return S_OK;
}

LRESULT CALLBACK ModelView::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ModelView* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<ModelView*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ModelView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (msg == WM_NCDESTROY && self) {
        {
            std::lock_guard<std::mutex> lock(self->shared_->mutex);
            self->shared_->hwnd = nullptr;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return self ? self->HandleMessage(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT ModelView::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        RequestFrame();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_TIMER:
        if (wParam == kSpinTimer && spinning_) {
            const DWORD now = GetTickCount();
            const DWORD elapsed = std::min<DWORD>(now - lastTick_, 200);
            lastTick_ = now;
            camera_.yaw += kSpinSpeed * static_cast<float>(elapsed) / 1000.0f;
            RequestFrame();
        }
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_KEYDOWN:
        switch (wParam) {
        case VK_LEFT: if (callbacks_.navigate) callbacks_.navigate(-1); return 0;
        case VK_RIGHT: if (callbacks_.navigate) callbacks_.navigate(+1); return 0;
        case VK_UP:
        case VK_DOWN:
            StopSpin();
            camera_.pitch = std::clamp(camera_.pitch + (wParam == VK_UP ? -kStep : kStep), -1.55f, 1.55f);
            RequestFrame();
            return 0;
        }
        break;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) {
            if (delta > 0) ZoomIn();
            else if (delta < 0) ZoomOut();
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
        ResetView();
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd_);
        StopSpin();
        dragging_ = true;
        dragLast_ = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetCapture(hwnd_);
        return 0;
    case WM_MOUSEMOVE:
        if (dragging_ && GetCapture() == hwnd_) {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            camera_.yaw += (pt.x - dragLast_.x) * kDragSpeed;
            camera_.pitch = std::clamp(camera_.pitch + (pt.y - dragLast_.y) * kDragSpeed, -1.55f, 1.55f);
            dragLast_ = pt;
            RequestFrame();
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (dragging_) {
            dragging_ = false;
            if (msg == WM_LBUTTONUP) ReleaseCapture();
            RequestFrame();  // a smoother still frame
        }
        return 0;
    case WM_APP_MODEL_LOADED:
        OnLoaded();
        return 0;
    case WM_APP_MODEL_FRAME:
        OnFrame();
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

COLORREF ModelView::Background() const {
    return fullscreen_ ? RGB(0, 0, 0) : theme::Current().viewer;
}

void ModelView::Show(const std::wstring& path) {
    Clear();
    loading_ = true;
    unsigned token;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        token = shared_->token;
    }
    const auto shared = shared_;
    pool_->Submit([shared, token, path] {
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (shared->token != token) return;
        }
        Mesh mesh;
        HRESULT hr = LoadModel(path, mesh);
        std::shared_ptr<PreparedModel> model;
        if (SUCCEEDED(hr)) {
            try {
                model = std::make_shared<PreparedModel>();
                if (!PrepareModel(std::move(mesh), *model)) hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            } catch (const std::bad_alloc&) {
                hr = E_OUTOFMEMORY;
            }
        }
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (shared->token != token) return;
        shared->result = hr;
        shared->model = SUCCEEDED(hr) ? std::move(model) : nullptr;
        shared->loadedToken = token;
        if (shared->hwnd) PostMessageW(shared->hwnd, WM_APP_MODEL_LOADED, 0, 0);
    });
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (callbacks_.changed) callbacks_.changed();
}

void ModelView::OnLoaded() {
    HRESULT hr;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->loadedToken != shared_->token) return;
        model_ = std::move(shared_->model);
        hr = shared_->result;
    }
    loading_ = false;
    failed_ = FAILED(hr) || !model_;
    triangles_ = model_ ? model_->triangles : 0;
    if (!failed_) {
        StartSpin();
        RequestFrame();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (callbacks_.changed) callbacks_.changed();
}

void ModelView::RequestFrame() {
    if (!model_ || !hwnd_) return;
    if (rendering_) {
        frameDirty_ = true;
        return;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int width = rc.right, height = rc.bottom;
    if (width <= 0 || height <= 0) return;
    // Moving frames are drawn plainly; a still frame gets smoothed edges.
    const bool moving = dragging_ || spinning_;
    const unsigned ss = !moving && static_cast<long long>(width) * height <= 2500000LL ? 2 : 1;
    unsigned token;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        token = shared_->token;
    }
    const auto shared = shared_;
    const auto model = model_;
    const ModelCamera camera = camera_;
    const COLORREF background = Background();
    rendering_ = true;
    frameDirty_ = false;
    const HRESULT submitted = pool_->Submit([shared, token, model, camera, width, height, ss, background] {
        const DWORD start = GetTickCount();
        std::vector<std::uint32_t> pixels;
        if (RasterizeModel(*model, camera, static_cast<unsigned>(width), static_cast<unsigned>(height), ss, pixels)) {
            // Blend onto the page colour so GDI can show it directly.
            const unsigned br = GetRValue(background), bg = GetGValue(background), bb = GetBValue(background);
            for (std::uint32_t& p : pixels) {
                const unsigned rest = 255 - (p >> 24);
                const unsigned r = ((p >> 16) & 0xFF) + (br * rest + 127) / 255;
                const unsigned g = ((p >> 8) & 0xFF) + (bg * rest + 127) / 255;
                const unsigned b = (p & 0xFF) + (bb * rest + 127) / 255;
                p = (std::min(r, 255u) << 16) | (std::min(g, 255u) << 8) | std::min(b, 255u);
            }
        } else {
            pixels.clear();
        }
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->pixels = std::move(pixels);
        shared->width = width;
        shared->height = height;
        shared->milliseconds = GetTickCount() - start;
        shared->frameToken = token;
        if (shared->hwnd) PostMessageW(shared->hwnd, WM_APP_MODEL_FRAME, 0, 0);
    });
    if (FAILED(submitted)) rendering_ = false;
}

void ModelView::OnFrame() {
    DWORD milliseconds = 0;
    bool current = false;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        current = shared_->frameToken == shared_->token;
        if (current && !shared_->pixels.empty()) {
            frame_ = std::move(shared_->pixels);
            frameWidth_ = shared_->width;
            frameHeight_ = shared_->height;
        }
        shared_->pixels.clear();
        milliseconds = shared_->milliseconds;
    }
    rendering_ = false;
    if (current) {
        if (spinning_ && milliseconds > kSlowFrame) StopSpin();  // too heavy to animate smoothly
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (frameDirty_) RequestFrame();
}

void ModelView::Paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    HBRUSH brush = CreateSolidBrush(Background());
    if (!frame_.empty() && model_) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = frameWidth_;
        info.bmiHeader.biHeight = -frameHeight_;  // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        if (frameWidth_ == rc.right && frameHeight_ == rc.bottom) {
            SetDIBitsToDevice(dc, 0, 0, frameWidth_, frameHeight_, 0, 0, 0, frameHeight_, frame_.data(), &info,
                              DIB_RGB_COLORS);
        } else {
            // Resizing: stretch the last frame until the new one arrives.
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, frameWidth_, frameHeight_, frame_.data(), &info,
                          DIB_RGB_COLORS, SRCCOPY);
        }
    } else {
        FillRect(dc, &rc, brush);
        if (loading_ || failed_) {
            const std::wstring text = LoadStr(failed_ ? IDS_MODEL_ERROR : IDS_VIEWER_LOADING);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, fullscreen_ ? RGB(220, 220, 220) : theme::Current().text);
            HGDIOBJ old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            DrawTextW(dc, text.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
        }
    }
    DeleteObject(brush);
    EndPaint(hwnd_, &ps);
}

void ModelView::StartSpin() {
    if (spinning_ || !hwnd_) return;
    spinning_ = true;
    lastTick_ = GetTickCount();
    SetTimer(hwnd_, kSpinTimer, kSpinInterval, nullptr);
}

void ModelView::StopSpin() {
    if (!spinning_) return;
    spinning_ = false;
    if (hwnd_) KillTimer(hwnd_, kSpinTimer);
    RequestFrame();  // a smoother still frame
}

void ModelView::Clear() {
    spinning_ = false;
    if (hwnd_) KillTimer(hwnd_, kSpinTimer);
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        ++shared_->token;
        shared_->model.reset();
        shared_->pixels.clear();
    }
    pool_->CancelPending();
    model_.reset();
    frame_.clear();
    frame_.shrink_to_fit();
    frameWidth_ = frameHeight_ = 0;
    camera_ = ModelCamera{};
    loading_ = failed_ = false;
    frameDirty_ = false;
    rendering_ = false;  // a frame still being drawn reports back and is ignored
    dragging_ = false;
    triangles_ = 0;
    wheelRemainder_ = 0;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void ModelView::SetFullscreen(bool fullscreen) {
    fullscreen_ = fullscreen;
    RequestFrame();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ModelView::OnThemeChanged() {
    RequestFrame();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ModelView::ZoomIn() {
    camera_.zoom = std::min(20.0f, camera_.zoom * kZoomStep);
    RequestFrame();
    if (callbacks_.changed) callbacks_.changed();
}

void ModelView::ZoomOut() {
    camera_.zoom = std::max(0.1f, camera_.zoom / kZoomStep);
    RequestFrame();
    if (callbacks_.changed) callbacks_.changed();
}

void ModelView::ResetView() {
    if (!model_) return;
    camera_ = ModelCamera{};
    StartSpin();
    RequestFrame();
    if (callbacks_.changed) callbacks_.changed();
}

void ModelView::Turn(int steps) {
    if (!model_) return;
    StopSpin();
    camera_.yaw += kStep * static_cast<float>(steps);
    RequestFrame();
}
