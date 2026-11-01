#include "ui/PreviewPane.h"

namespace {
constexpr wchar_t kClassName[] = L"LiteViewPreviewPane";
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
    hwnd_ = CreateWindowExW(WS_EX_CLIENTEDGE, kClassName, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                            parent, nullptr, instance, this);
    return hwnd_ ? S_OK : HRESULT_FROM_WIN32(GetLastError());
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
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void PreviewPane::OnPaint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    FillRect(dc, &ps.rcPaint, GetSysColorBrush(COLOR_APPWORKSPACE));
    EndPaint(hwnd_, &ps);
}
