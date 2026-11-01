#include "ui/DocumentView.h"

#include <propsys.h>
#include <shlwapi.h>

#include <algorithm>
#include <utility>

#include "res/resource.h"
#include "ui/Messages.h"
#include "ui/Theme.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClassName[] = L"LiteViewDocumentView";
// Shell extension key of preview handlers (IPreviewHandler's IID).
constexpr wchar_t kPreviewHandlerKey[] = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";

bool HandlerFor(const std::wstring& path, CLSID& clsid) {
    const wchar_t* extension = PathFindExtensionW(path.c_str());
    if (!*extension) return false;
    wchar_t text[64] = {};
    DWORD length = ARRAYSIZE(text);
    if (FAILED(AssocQueryStringW(ASSOCF_INIT_DEFAULTTOSTAR, ASSOCSTR_SHELLEXTENSION, extension, kPreviewHandlerKey,
                                 text, &length))) {
        return false;
    }
    return SUCCEEDED(CLSIDFromString(text, &clsid));
}

std::wstring LoadStr(UINT id) {
    const wchar_t* text = nullptr;
    const int length = LoadStringW(GetModuleHandleW(nullptr), id, reinterpret_cast<LPWSTR>(&text), 0);
    return length > 0 ? std::wstring(text, static_cast<std::size_t>(length)) : std::wstring();
}

}  // namespace

// The site given to the preview handler: lets keys the handler does not use
// (Esc, PageDown, Ctrl+F...) reach LiteView's accelerators.
class DocumentView::Frame final : public IPreviewHandlerFrame {
public:
    Frame(HACCEL accelerators, std::function<bool(MSG*)> translate)
        : accelerators_(accelerators), translate_(std::move(translate)) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPreviewHandlerFrame) {
            *object = static_cast<IPreviewHandlerFrame*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const LONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs);
    }

    IFACEMETHODIMP GetWindowContext(PREVIEWHANDLERFRAMEINFO* info) override {
        if (!info) return E_POINTER;
        info->haccel = accelerators_;
        info->cAccelEntries = accelerators_ ? static_cast<UINT>(CopyAcceleratorTableW(accelerators_, nullptr, 0)) : 0;
        return S_OK;
    }

    IFACEMETHODIMP TranslateAccelerator(MSG* msg) override {
        return (msg && translate_ && translate_(msg)) ? S_OK : S_FALSE;
    }

private:
    ~Frame() = default;
    LONG refs_ = 1;
    HACCEL accelerators_;
    std::function<bool(MSG*)> translate_;
};

DocumentView::~DocumentView() { Unload(); }

HRESULT DocumentView::Create(HWND parent, HINSTANCE instance, HACCEL accelerators, Callbacks callbacks) {
    accelerators_ = accelerators;
    callbacks_ = std::move(callbacks);
    WNDCLASSEXW wc = {sizeof(wc)};
    if (!GetClassInfoExW(instance, kClassName, &wc)) {
        wc = {sizeof(wc)};
        wc.lpfnWndProc = &DocumentView::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return HRESULT_FROM_WIN32(GetLastError());
    }
    hwnd_ = CreateWindowExW(0, kClassName, nullptr, WS_CHILD | WS_CLIPCHILDREN | WS_TABSTOP, 0, 0, 0, 0, parent,
                            nullptr, instance, this);
    if (!hwnd_) return HRESULT_FROM_WIN32(GetLastError());
    try {
        frame_.Attach(new Frame(accelerators_, callbacks_.translateAccelerator));  // starts with one reference
    } catch (...) {
        frame_.Reset();
    }
    return S_OK;
}

LRESULT CALLBACK DocumentView::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DocumentView* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<DocumentView*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DocumentView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (msg == WM_NCDESTROY && self) {
        self->Unload();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return self ? self->HandleMessage(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT DocumentView::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT:
        OnPaint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (handler_) {
            const RECT rc = HandlerRect();
            handler_->SetRect(&rc);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_SETFOCUS:
        // Keep keyboard focus here so the viewer shortcuts work; a click in
        // the document moves it into the handler.
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd_);
        return 0;
    case WM_APP_DOCUMENT_LOAD:
        Load();
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

RECT DocumentView::HandlerRect() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    return rc;
}

void DocumentView::Show(const std::wstring& path) {
    Unload();
    path_ = path;
    state_ = State::Loading;
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (callbacks_.changed) callbacks_.changed();
    // Paint "loading" and the status line first: starting a handler blocks.
    PostMessageW(hwnd_, WM_APP_DOCUMENT_LOAD, 0, 0);
}

void DocumentView::Load() {
    if (state_ != State::Loading || path_.empty()) return;
    UpdateWindow(hwnd_);
    CLSID clsid{};
    if (!HandlerFor(path_, clsid)) {
        state_ = State::NoHandler;
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (callbacks_.changed) callbacks_.changed();
        return;
    }
    // Handlers normally run isolated in a surrogate process (prevhost.exe).
    ComPtr<IPreviewHandler> handler;
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(handler.GetAddressOf()));
    if (FAILED(hr)) hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(handler.GetAddressOf()));
    ComPtr<IStream> stream;
    if (SUCCEEDED(hr)) {
        ComPtr<IInitializeWithFile> withFile;
        ComPtr<IInitializeWithItem> withItem;
        ComPtr<IInitializeWithStream> withStream;
        if (SUCCEEDED(handler.As(&withFile))) {
            hr = withFile->Initialize(path_.c_str(), STGM_READ);
        } else if (SUCCEEDED(handler.As(&withItem))) {
            ComPtr<IShellItem> item;
            hr = SHCreateItemFromParsingName(path_.c_str(), nullptr, IID_PPV_ARGS(item.GetAddressOf()));
            if (SUCCEEDED(hr)) hr = withItem->Initialize(item.Get(), STGM_READ);
        } else if (SUCCEEDED(handler.As(&withStream))) {
            hr = SHCreateStreamOnFileEx(path_.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE,
                                        nullptr, stream.GetAddressOf());
            if (SUCCEEDED(hr)) hr = withStream->Initialize(stream.Get(), STGM_READ);
        } else {
            hr = E_NOINTERFACE;
        }
    }
    if (SUCCEEDED(hr) && frame_) {
        ComPtr<IObjectWithSite> site;
        if (SUCCEEDED(handler.As(&site))) site->SetSite(frame_.Get());
    }
    if (SUCCEEDED(hr)) {
        const RECT rc = HandlerRect();
        hr = handler->SetWindow(hwnd_, &rc);
    }
    if (SUCCEEDED(hr)) {
        handler_ = handler;
        stream_ = stream;
        ApplyColors();
        hr = handler->DoPreview();
    }
    if (FAILED(hr)) {
        Unload();
        state_ = State::Failed;
    } else {
        state_ = State::Shown;
        SetFocus(hwnd_);  // the handler may have taken it
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (callbacks_.changed) callbacks_.changed();
}

void DocumentView::Unload() {
    if (handler_) {
        handler_->Unload();
        ComPtr<IObjectWithSite> site;
        if (SUCCEEDED(handler_.As(&site))) site->SetSite(nullptr);
        handler_.Reset();
    }
    stream_.Reset();
}

void DocumentView::Clear() {
    Unload();
    path_.clear();
    state_ = State::Empty;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void DocumentView::ApplyColors() {
    ComPtr<IPreviewHandlerVisuals> visuals;
    if (!handler_ || FAILED(handler_.As(&visuals))) return;
    const auto& p = theme::Current();
    visuals->SetBackgroundColor(fullscreen_ ? RGB(0, 0, 0) : p.surface);
    visuals->SetTextColor(fullscreen_ ? RGB(220, 220, 220) : p.text);
}

void DocumentView::SetFullscreen(bool fullscreen) {
    fullscreen_ = fullscreen;
    ApplyColors();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void DocumentView::OnThemeChanged() {
    ApplyColors();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void DocumentView::OnPaint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const auto& p = theme::Current();
    HBRUSH brush = CreateSolidBrush(fullscreen_ ? RGB(0, 0, 0) : p.surface);
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
    UINT message = 0;
    switch (state_) {
    case State::Loading: message = IDS_VIEWER_LOADING; break;
    case State::NoHandler: message = IDS_DOC_NO_HANDLER; break;
    case State::Failed: message = IDS_DOC_ERROR; break;
    default: break;
    }
    if (message) {
        const std::wstring text = LoadStr(message);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, fullscreen_ ? RGB(220, 220, 220) : p.text);
        HGDIOBJ old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        RECT box = rc;
        InflateRect(&box, -24, -24);
        RECT measured = box;
        DrawTextW(dc, text.c_str(), -1, &measured, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        OffsetRect(&box, 0, std::max(0L, ((box.bottom - box.top) - (measured.bottom - measured.top)) / 2));
        DrawTextW(dc, text.c_str(), -1, &box, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
    }
    EndPaint(hwnd_, &ps);
}
