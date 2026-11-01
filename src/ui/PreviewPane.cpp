#include "ui/PreviewPane.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <shlwapi.h>

#include <algorithm>

#include "core/FormatSupport.h"
#include "core/ImageDecoder.h"
#include "core/TextDocument.h"
#include "core/ThreadPool.h"
#include "core/ThumbnailService.h"
#include "ui/Messages.h"
#include "ui/Theme.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClassName[] = L"LiteViewPreviewPane";
constexpr UINT_PTR kDebounceTimer = 1;
constexpr UINT kDebounceMs = 60;
constexpr std::size_t kPreviewTextBytes = 32 * 1024;

struct ThreadCom {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ThreadCom() { if (SUCCEEDED(result)) CoUninitialize(); }
};

// Office documents: the shell's thumbnail (needs the program's thumbnail
// provider, e.g. Office); the full document is shown in the viewer.
HRESULT ShellFitted(const std::wstring& path, UINT boxWidth, UINT boxHeight, HBITMAP& bitmap, UINT& width,
                    UINT& height) {
    ThumbnailPtr thumbnail;
    HRESULT hr = ShellThumbnailImage(path, std::max(boxWidth, boxHeight), thumbnail);
    if (FAILED(hr)) return hr;
    bitmap = static_cast<HBITMAP>(CopyImage(thumbnail->bitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
    if (!bitmap) return E_OUTOFMEMORY;
    width = thumbnail->width;
    height = thumbnail->height;
    return S_OK;
}

HRESULT DecodeFitted(const std::wstring& path, UINT boxWidth, UINT boxHeight, HBITMAP& bitmap, UINT& width,
                     UINT& height) {
    if (IsDocumentExtension(PathFindExtensionW(path.c_str()))) return ShellFitted(path, boxWidth, boxHeight, bitmap, width, height);
    ImageDecoder decoder;
    DecodedImage image;
    HRESULT hr = decoder.Decode(path, std::max(boxWidth, boxHeight), image);
    if (FAILED(hr)) return hr;
    width = image.width;
    height = image.height;
    ComPtr<IWICBitmapSource> source = image.bitmap;
    if (width > boxWidth || height > boxHeight) {
        const double scale = std::min(static_cast<double>(boxWidth) / width, static_cast<double>(boxHeight) / height);
        width = std::max<UINT>(1, static_cast<UINT>(width * scale + 0.5));
        height = std::max<UINT>(1, static_cast<UINT>(height * scale + 0.5));
        ComPtr<IWICImagingFactory> factory;
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(hr)) hr = factory->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) hr = scaler->Initialize(image.bitmap.Get(), width, height, WICBitmapInterpolationModeFant);
        if (FAILED(hr)) return hr;
        source = scaler;
    }
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) return E_OUTOFMEMORY;
    hr = source->CopyPixels(nullptr, width * 4, width * 4 * height, static_cast<BYTE*>(bits));
    if (FAILED(hr)) {
        DeleteObject(bitmap);
        bitmap = nullptr;
    }
    GdiFlush();
    return hr;
}

}  // namespace

PreviewPane::PreviewPane() : shared_(std::make_shared<Shared>()), pool_(std::make_unique<ThreadPool>(1)) {}

PreviewPane::~PreviewPane() {
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->hwnd = nullptr;
    }
    pool_->Shutdown();
    if (shared_->bitmap) DeleteObject(shared_->bitmap);
    if (textFont_) DeleteObject(textFont_);
}

HRESULT PreviewPane::Create(HWND parent, HINSTANCE instance) {
    WNDCLASSEXW wc = {sizeof(wc)};
    if (!GetClassInfoExW(instance, kClassName, &wc)) {
        wc = {sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &PreviewPane::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;  // painted in WM_PAINT
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return HRESULT_FROM_WIN32(GetLastError());
    }
    hwnd_ = CreateWindowExW(0, kClassName, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                            parent, nullptr, instance, this);
    if (!hwnd_) return HRESULT_FROM_WIN32(GetLastError());
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->hwnd = hwnd_;
    return S_OK;
}

LRESULT CALLBACK PreviewPane::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PreviewPane* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<PreviewPane*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<PreviewPane*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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

LRESULT PreviewPane::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT:
        OnPaint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE: {
        // Re-decode when the pane grows past what the current image was made for.
        const int w = LOWORD(lParam), h = HIWORD(lParam);
        if (!path_.empty() && (w > decodedFor_.cx || h > decodedFor_.cy)) {
            SetTimer(hwnd_, kDebounceTimer, kDebounceMs * 3, nullptr);
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == kDebounceTimer) {
            KillTimer(hwnd_, kDebounceTimer);
            StartDecode();
            return 0;
        }
        break;
    case WM_APP_PREVIEW_READY:
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void PreviewPane::Show(const std::wstring& path) {
    if (path == path_) return;
    path_ = path;
    {
        // Invalidate any decode in flight; keep showing the old picture
        // until the new one is ready, unless the pane is being cleared.
        std::lock_guard<std::mutex> lock(shared_->mutex);
        ++shared_->token;
        if (path.empty() && shared_->bitmap) {
            DeleteObject(shared_->bitmap);
            shared_->bitmap = nullptr;
        }
        if (path.empty()) {
            shared_->text.clear();
            shared_->hasText = false;
        }
    }
    pool_->CancelPending();
    if (path.empty()) {
        KillTimer(hwnd_, kDebounceTimer);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    SetTimer(hwnd_, kDebounceTimer, kDebounceMs, nullptr);
}

void PreviewPane::StartDecode() {
    if (path_.empty()) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const UINT margin = 4;
    if (rc.right <= static_cast<LONG>(2 * margin) || rc.bottom <= static_cast<LONG>(2 * margin)) return;
    decodedFor_ = SIZE{rc.right, rc.bottom};
    unsigned token;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        token = ++shared_->token;
    }
    pool_->CancelPending();
    const auto shared = shared_;
    const std::wstring path = path_;
    const UINT w = static_cast<UINT>(rc.right) - 2 * margin;
    const UINT h = static_cast<UINT>(rc.bottom) - 2 * margin;
    pool_->Submit([shared, token, path, w, h] { Decode(shared, token, path, w, h); });
}

void PreviewPane::Decode(const std::shared_ptr<Shared>& shared, unsigned token, std::wstring path, UINT boxWidth,
                         UINT boxHeight) noexcept {
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (shared->token != token) return;
    }
    if (IsTextExtension(PathFindExtensionW(path.c_str()))) {
        TextDocument document;
        const HRESULT hr = LoadTextFile(path, kPreviewTextBytes, document);
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (shared->token != token) return;
        if (shared->bitmap) DeleteObject(shared->bitmap);
        shared->bitmap = nullptr;
        shared->text = SUCCEEDED(hr) ? std::move(document.text) : std::wstring();
        shared->hasText = SUCCEEDED(hr);
        shared->bitmapToken = token;
        if (shared->hwnd) PostMessageW(shared->hwnd, WM_APP_PREVIEW_READY, 0, 0);
        return;
    }
    thread_local ThreadCom com;
    HBITMAP bitmap = nullptr;
    UINT width = 0, height = 0;
    const HRESULT hr = (SUCCEEDED(com.result) || com.result == RPC_E_CHANGED_MODE)
                           ? DecodeFitted(path, boxWidth, boxHeight, bitmap, width, height)
                           : com.result;
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (shared->token != token) {
        if (bitmap) DeleteObject(bitmap);
        return;
    }
    if (shared->bitmap) DeleteObject(shared->bitmap);
    shared->bitmap = SUCCEEDED(hr) ? bitmap : nullptr;  // a broken file shows an empty pane
    shared->text.clear();
    shared->hasText = false;
    shared->width = width;
    shared->height = height;
    shared->bitmapToken = token;
    if (shared->hwnd) PostMessageW(shared->hwnd, WM_APP_PREVIEW_READY, 0, 0);
}

void PreviewPane::OnPaint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP canvas = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldCanvas = SelectObject(memory, canvas);
    FillRect(memory, &rc, theme::SurfaceBrush());
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->hasText && !path_.empty()) {
            // First lines of a text file, top-left, in a small monospaced font.
            const UINT dpi = GetDpiForWindow(hwnd_);
            if (!textFont_ || textFontDpi_ != dpi) {
                if (textFont_) DeleteObject(textFont_);
                textFont_ = CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        FIXED_PITCH | FF_MODERN, L"Consolas");
                textFontDpi_ = dpi;
            }
            const int pad = MulDiv(8, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
            RECT box{pad, pad, std::max(pad, static_cast<int>(rc.right) - pad), std::max(pad, static_cast<int>(rc.bottom) - pad)};
            HGDIOBJ oldFont = SelectObject(memory, textFont_ ? static_cast<HGDIOBJ>(textFont_) : GetStockObject(ANSI_FIXED_FONT));
            SetBkMode(memory, TRANSPARENT);
            SetTextColor(memory, theme::Current().text);
            // DrawText is slow on huge strings; the pane never shows more than a few KB.
            const int length = static_cast<int>(std::min<std::size_t>(shared_->text.size(), 8192));
            DrawTextW(memory, shared_->text.c_str(), length, &box,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL | DT_EXPANDTABS | DT_NOPREFIX);
            SelectObject(memory, oldFont);
        } else if (shared_->bitmap && !path_.empty()) {
            // Fit (never enlarge) and centre.
            const UINT w = shared_->width, h = shared_->height;
            const double scale = std::min({1.0, static_cast<double>(rc.right) / w, static_cast<double>(rc.bottom) / h});
            const int dw = std::max(1, static_cast<int>(w * scale + 0.5));
            const int dh = std::max(1, static_cast<int>(h * scale + 0.5));
            const int x = (rc.right - dw) / 2;
            const int y = (rc.bottom - dh) / 2;
            HDC source = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(source, shared_->bitmap);
            BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            SetStretchBltMode(memory, HALFTONE);
            AlphaBlend(memory, x, y, dw, dh, source, 0, 0, static_cast<int>(w), static_cast<int>(h), blend);
            SelectObject(source, old);
            DeleteDC(source);
        }
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, oldCanvas);
    DeleteObject(canvas);
    DeleteDC(memory);
    EndPaint(hwnd_, &ps);
}
