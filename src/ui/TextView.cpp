#include "ui/TextView.h"

#include <commctrl.h>
#include <uxtheme.h>

#include <algorithm>
#include <utility>

#include "core/ThreadPool.h"
#include "ui/Messages.h"
#include "ui/Theme.h"

namespace {

constexpr wchar_t kClassName[] = L"LiteViewTextView";
constexpr int kBasePoints = 11;
constexpr int kZoomSteps[] = {50, 67, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300, 400};

}  // namespace

TextView::TextView() : shared_(std::make_shared<Shared>()), pool_(std::make_unique<ThreadPool>(1)) {}

TextView::~TextView() {
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->hwnd = nullptr;
    }
    pool_->Shutdown();
    if (font_) DeleteObject(font_);
    if (background_) DeleteObject(background_);
}

HRESULT TextView::Create(HWND parent, HINSTANCE instance, std::function<void()> changed) {
    changed_ = std::move(changed);
    WNDCLASSEXW wc = {sizeof(wc)};
    if (!GetClassInfoExW(instance, kClassName, &wc)) {
        wc = {sizeof(wc)};
        wc.lpfnWndProc = &TextView::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return HRESULT_FROM_WIN32(GetLastError());
    }
    hwnd_ = CreateWindowExW(0, kClassName, nullptr, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, nullptr,
                            instance, this);
    if (!hwnd_) return HRESULT_FROM_WIN32(GetLastError());
    edit_ = CreateWindowExW(0, WC_EDITW, nullptr,
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                                ES_NOHIDESEL,
                            0, 0, 0, 0, hwnd_, nullptr, instance, nullptr);
    if (!edit_) return HRESULT_FROM_WIN32(GetLastError());
    SendMessageW(edit_, EM_SETLIMITTEXT, 0, 0);  // no limit beyond memory
    SetWindowTheme(edit_, theme::Current().dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    ApplyFont();
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->hwnd = hwnd_;
    return S_OK;
}

LRESULT CALLBACK TextView::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TextView* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<TextView*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TextView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (msg == WM_NCDESTROY && self) {
        {
            std::lock_guard<std::mutex> lock(self->shared_->mutex);
            self->shared_->hwnd = nullptr;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        self->edit_ = nullptr;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return self ? self->HandleMessage(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TextView::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE: {
        // A small margin around the text, like a page.
        const int w = LOWORD(lParam), h = HIWORD(lParam);
        const int pad = MulDiv(12, static_cast<int>(GetDpiForWindow(hwnd_)), USER_DEFAULT_SCREEN_DPI);
        SetWindowPos(edit_, nullptr, pad, pad / 2, std::max(0, w - pad), std::max(0, h - pad / 2),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const COLORREF color = fullscreen_ ? RGB(0, 0, 0) : theme::Current().surface;
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(reinterpret_cast<HDC>(wParam), &rc, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        // Read-only edits ask with WM_CTLCOLORSTATIC.
        const auto& p = theme::Current();
        const COLORREF back = fullscreen_ ? RGB(0, 0, 0) : p.surface;
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, fullscreen_ ? RGB(220, 220, 220) : p.text);
        SetBkColor(dc, back);
        if (background_) DeleteObject(background_);
        background_ = CreateSolidBrush(back);
        return reinterpret_cast<LRESULT>(background_);
    }
    case WM_SETFOCUS:
        if (edit_) SetFocus(edit_);
        return 0;
    case WM_DPICHANGED_AFTERPARENT:
        ApplyFont();
        return 0;
    case WM_APP_TEXT_READY:
        OnReady();
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void TextView::Show(const std::wstring& path) {
    path_ = path;
    loading_ = true;
    failed_ = false;
    truncated_ = false;
    SetWindowTextW(edit_, L"");
    unsigned token;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        token = ++shared_->token;
        shared_->document = {};
    }
    pool_->CancelPending();
    const auto shared = shared_;
    pool_->Submit([shared, token, path] {
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (shared->token != token) return;
        }
        TextDocument document;
        const HRESULT hr = LoadTextFile(path, kMaxBytes, document);
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (shared->token != token) return;
        shared->document = std::move(document);
        shared->result = hr;
        shared->readyToken = token;
        if (shared->hwnd) PostMessageW(shared->hwnd, WM_APP_TEXT_READY, 0, 0);
    });
    if (changed_) changed_();
}

void TextView::OnReady() {
    TextDocument document;
    HRESULT hr;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->readyToken != shared_->token) return;
        document = std::move(shared_->document);
        hr = shared_->result;
    }
    loading_ = false;
    failed_ = FAILED(hr);
    truncated_ = document.truncated;
    encoding_ = document.encoding;
    SetWindowTextW(edit_, failed_ ? L"" : document.text.c_str());
    SendMessageW(edit_, EM_SETSEL, 0, 0);
    SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
    if (changed_) changed_();
}

void TextView::Clear() {
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        ++shared_->token;
        shared_->document = {};
    }
    pool_->CancelPending();
    path_.clear();
    loading_ = failed_ = truncated_ = false;
    if (edit_) SetWindowTextW(edit_, L"");
}

void TextView::SetFullscreen(bool fullscreen) {
    fullscreen_ = fullscreen;
    InvalidateRect(hwnd_, nullptr, TRUE);
    InvalidateRect(edit_, nullptr, TRUE);
}

void TextView::OnThemeChanged() {
    SetWindowTheme(edit_, theme::Current().dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    InvalidateRect(hwnd_, nullptr, TRUE);
    InvalidateRect(edit_, nullptr, TRUE);
}

void TextView::ApplyFont() {
    if (!edit_) return;
    const int dpi = static_cast<int>(GetDpiForWindow(hwnd_));
    const int height = -MulDiv(kBasePoints * zoom_, dpi, 72 * 100);
    HFONT font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!font) return;
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (font_) DeleteObject(font_);
    font_ = font;
}

void TextView::SetZoom(int percent) {
    if (percent == zoom_) return;
    zoom_ = percent;
    ApplyFont();
    if (changed_) changed_();
}

void TextView::ZoomIn() {
    for (int step : kZoomSteps) {
        if (step > zoom_) { SetZoom(step); return; }
    }
}

void TextView::ZoomOut() {
    for (auto it = std::rbegin(kZoomSteps); it != std::rend(kZoomSteps); ++it) {
        if (*it < zoom_) { SetZoom(*it); return; }
    }
}

void TextView::ResetZoom() { SetZoom(100); }
