#include "ui/VideoView.h"

#include <mfplay.h>
#include <mferror.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <utility>

#include "core/FormatSupport.h"
#include "res/resource.h"
#include "ui/Theme.h"

namespace {

constexpr wchar_t kClassName[] = L"LiteViewVideoView";
constexpr wchar_t kSurfaceClass[] = L"LiteViewVideoSurface";
constexpr UINT_PTR kTickTimer = 1;
constexpr long long kSecond = 10000000;  // 100-ns units

std::wstring LoadStr(UINT id) {
    const wchar_t* text = nullptr;
    const int length = LoadStringW(GetModuleHandleW(nullptr), id, reinterpret_cast<LPWSTR>(&text), 0);
    return length > 0 ? std::wstring(text, static_cast<std::size_t>(length)) : std::wstring();
}

// Owns one GDI object and deletes it.
class GdiObject {
public:
    explicit GdiObject(HGDIOBJ object) : object_(object) {}
    ~GdiObject() { if (object_) DeleteObject(object_); }
    GdiObject(const GdiObject&) = delete;
    GdiObject& operator=(const GdiObject&) = delete;
    HGDIOBJ Get() const { return object_; }
    HBRUSH Brush() const { return static_cast<HBRUSH>(object_); }

private:
    HGDIOBJ object_;
};

// A memory DC with a bitmap selected, restored and freed on exit.
class MemoryDc {
public:
    MemoryDc(HDC target, int width, int height)
        : dc_(CreateCompatibleDC(target)), bitmap_(CreateCompatibleBitmap(target, width, height)) {
        if (dc_ && bitmap_.Get()) old_ = SelectObject(dc_, bitmap_.Get());
    }
    ~MemoryDc() {
        if (dc_ && old_) SelectObject(dc_, old_);
        if (dc_) DeleteDC(dc_);
    }
    MemoryDc(const MemoryDc&) = delete;
    MemoryDc& operator=(const MemoryDc&) = delete;
    HDC Get() const { return old_ ? dc_ : nullptr; }

private:
    HDC dc_;
    GdiObject bitmap_;
    HGDIOBJ old_ = nullptr;
};

// Selects an object into a DC for one scope.
class Selected {
public:
    Selected(HDC dc, HGDIOBJ object) : dc_(dc), old_(SelectObject(dc, object)) {}
    ~Selected() { SelectObject(dc_, old_); }
    Selected(const Selected&) = delete;
    Selected& operator=(const Selected&) = delete;

private:
    HDC dc_;
    HGDIOBJ old_;
};

bool IsCodecError(HRESULT hr) {
    return hr == MF_E_TOPO_CODEC_NOT_FOUND || hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE ||
           hr == MF_E_INVALIDMEDIATYPE || hr == MF_E_UNSUPPORTED_FORMAT || hr == MF_E_TOPO_UNSUPPORTED;
}

}  // namespace

// MFPlay reports on the thread that created the player (the UI thread).
class VideoView::Callback final : public IMFPMediaPlayerCallback {
public:
    explicit Callback(VideoView* owner) : owner_(owner) {}
    void Detach() { owner_ = nullptr; }

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFPMediaPlayerCallback)) {
            *object = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* header) override {
        if (owner_ && header) owner_->OnPlayerEvent(header);
    }

private:
    VideoView* owner_;
    std::atomic<ULONG> refs_{1};
};

VideoView::VideoView() = default;

VideoView::~VideoView() { Clear(); }

HRESULT VideoView::Create(HWND parent, HINSTANCE instance, Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
    WNDCLASSEXW wc = {sizeof(wc)};
    if (!GetClassInfoExW(instance, kClassName, &wc)) {
        wc = {sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &VideoView::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return HRESULT_FROM_WIN32(GetLastError());
    }
    if (!GetClassInfoExW(instance, kSurfaceClass, &wc)) {
        wc = {sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &VideoView::SurfaceProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kSurfaceClass;
        if (!RegisterClassExW(&wc)) return HRESULT_FROM_WIN32(GetLastError());
    }
    hwnd_ = CreateWindowExW(0, kClassName, nullptr, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, nullptr,
                            instance, this);
    if (!hwnd_) return HRESULT_FROM_WIN32(GetLastError());
    surface_ = CreateWindowExW(0, kSurfaceClass, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd_, nullptr,
                               instance, this);
    if (!surface_) return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

LRESULT CALLBACK VideoView::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    VideoView* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<VideoView*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<VideoView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (msg == WM_NCDESTROY && self) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return self ? self->HandleMessage(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

// The surface only paints the video (or black) and forwards mouse input.
LRESULT CALLBACK VideoView::SurfaceProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    VideoView* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<VideoView*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    self = reinterpret_cast<VideoView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->surface_ = nullptr;
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (self->player_ && self->hasVideo_) {
            self->player_->UpdateVideo();
        } else {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            UINT text = 0;
            switch (self->state_) {
            case State::Loading: text = IDS_VIEWER_LOADING; break;
            case State::NoCodec: text = IDS_VIDEO_NO_CODEC; break;
            case State::Failed: text = IDS_VIDEO_ERROR; break;
            default: break;
            }
            if (text) {
                const std::wstring message = LoadStr(text);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(220, 220, 220));
                HGDIOBJ old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
                RECT box = rc;
                DrawTextW(dc, message.c_str(), -1, &box, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
                const int h = box.bottom - box.top;
                box = RECT{rc.left + 16, (rc.bottom - h) / 2, rc.right - 16, (rc.bottom + h) / 2};
                DrawTextW(dc, message.c_str(), -1, &box, DT_CENTER | DT_WORDBREAK);
                SelectObject(dc, old);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (self->player_ && self->hasVideo_) self->player_->UpdateVideo();
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(self->hwnd_);
        if (self->state_ == State::NoCodec) {
            // Store search for the video extensions (HEVC, AV1, VP9...).
            ShellExecuteW(nullptr, L"open", L"ms-windows-store://search/?query=video%20extensions", nullptr, nullptr,
                          SW_SHOWNORMAL);
        } else {
            self->TogglePlay();
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (self->callbacks_.exit) self->callbacks_.exit();
        return 0;
    case WM_MOUSEWHEEL:
        return SendMessageW(self->hwnd_, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int VideoView::BarHeight() const {
    return MulDiv(40, static_cast<int>(GetDpiForWindow(hwnd_)), USER_DEFAULT_SCREEN_DPI);
}

RECT VideoView::ButtonRect() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int bar = BarHeight();
    return RECT{0, rc.bottom - bar, bar, rc.bottom};
}

RECT VideoView::TrackRect() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int bar = BarHeight();
    const int timeWidth = MulDiv(130, static_cast<int>(GetDpiForWindow(hwnd_)), USER_DEFAULT_SCREEN_DPI);
    const int mid = rc.bottom - bar / 2;
    const int half = std::max(2, bar / 10);
    return RECT{bar, mid - half, std::max(bar + 1, static_cast<int>(rc.right) - timeWidth), mid + half};
}

void VideoView::Layout() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    SetWindowPos(surface_, nullptr, 0, 0, rc.right, std::max(0, static_cast<int>(rc.bottom) - BarHeight()),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT VideoView::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        Layout();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        PaintBar(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kTickTimer) {
            RECT bar = ButtonRect();
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            bar.right = rc.right;
            InvalidateRect(hwnd_, &bar, FALSE);
            if (callbacks_.changed) callbacks_.changed();
        }
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
        switch (wParam) {
        case VK_SPACE: TogglePlay(); return 0;
        case VK_LEFT: Seek(-5); return 0;
        case VK_RIGHT: Seek(5); return 0;
        }
        break;
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd_);
        const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const RECT button = ButtonRect();
        RECT track = TrackRect();
        InflateRect(&track, 0, BarHeight() / 3);
        if (PtInRect(&button, pt)) {
            TogglePlay();
        } else if (PtInRect(&track, pt) && duration_ > 0) {
            const RECT exact = TrackRect();
            const double fraction = std::clamp(static_cast<double>(pt.x - exact.left) / std::max(1L, exact.right - exact.left), 0.0, 1.0);
            SeekTo(static_cast<long long>(fraction * static_cast<double>(duration_)));
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(wParam);
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
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void VideoView::PaintBar(HDC target) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int bar = BarHeight();
    RECT area{0, rc.bottom - bar, rc.right, rc.bottom};
    if (area.right <= 0 || bar <= 0) return;
    // Draw off-screen to avoid flicker on each tick.
    MemoryDc memory(target, area.right, bar);
    HDC dc = memory.Get();
    if (!dc) return;
    SetViewportOrgEx(dc, 0, -area.top, nullptr);

    const auto& palette = theme::Current();
    const COLORREF back = fullscreen_ ? RGB(0, 0, 0) : palette.surface;
    const COLORREF fore = fullscreen_ ? RGB(220, 220, 220) : palette.text;
    {
        GdiObject backBrush(CreateSolidBrush(back));
        FillRect(dc, &area, backBrush.Brush());
    }

    // Play / pause glyph.
    const RECT button = ButtonRect();
    const int cx = (button.left + button.right) / 2, cy = (button.top + button.bottom) / 2;
    const int s = bar / 5;
    {
        GdiObject foreBrush(CreateSolidBrush(fore));
        Selected brush(dc, foreBrush.Get());
        Selected pen(dc, GetStockObject(NULL_PEN));
        if (state_ == State::Playing) {
            RECT left{cx - s, cy - s, cx - s / 3, cy + s};
            RECT right{cx + s / 3, cy - s, cx + s, cy + s};
            FillRect(dc, &left, foreBrush.Brush());
            FillRect(dc, &right, foreBrush.Brush());
        } else {
            const POINT triangle[3] = {{cx - s * 2 / 3, cy - s}, {cx - s * 2 / 3, cy + s}, {cx + s, cy}};
            Polygon(dc, triangle, 3);
        }
    }

    // Track and progress.
    const RECT track = TrackRect();
    const long long position = Position();
    {
        GdiObject trackBrush(CreateSolidBrush(fullscreen_ ? RGB(80, 80, 80) : palette.border));
        FillRect(dc, &track, trackBrush.Brush());
    }
    if (duration_ > 0) {
        RECT done = track;
        done.right = track.left + static_cast<LONG>((track.right - track.left) *
                                                    std::clamp(static_cast<double>(position) / static_cast<double>(duration_), 0.0, 1.0));
        GdiObject accent(CreateSolidBrush(palette.accent));
        FillRect(dc, &done, accent.Brush());
    }

    // Time.
    const std::wstring time = FormatMediaTime(position) + L" / " + FormatMediaTime(duration_);
    RECT text{track.right + bar / 4, area.top, area.right - bar / 4, area.bottom};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fore);
    {
        Selected font(dc, GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(dc, time.c_str(), -1, &text, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    SetViewportOrgEx(dc, 0, 0, nullptr);
    BitBlt(target, 0, area.top, area.right, bar, dc, 0, 0, SRCCOPY);
}

void VideoView::Show(const std::wstring& path) {
    Clear();
    path_ = path;
    callback_.Attach(new Callback(this));
    SetState(State::Loading);
    const HRESULT hr = MFPCreateMediaPlayer(path.c_str(), TRUE, MFP_OPTION_NONE, callback_.Get(), surface_,
                                            player_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        player_.Reset();
        SetState(IsCodecError(hr) ? State::NoCodec : State::Failed);
        return;
    }
    SetTimer(hwnd_, kTickTimer, 250, nullptr);
}

void VideoView::OnPlayerEvent(MFP_EVENT_HEADER* header) {
    if (!player_) return;
    if (FAILED(header->hrEvent)) {
        hasVideo_ = false;
        SetState(IsCodecError(header->hrEvent) ? State::NoCodec : State::Failed);
        return;
    }
    switch (header->eEventType) {
    case MFP_EVENT_TYPE_MEDIAITEM_SET: {
        BOOL hasVideo = FALSE, selected = FALSE;
        Microsoft::WRL::ComPtr<IMFPMediaItem> item;
        if (SUCCEEDED(player_->GetMediaItem(item.GetAddressOf())) && item) {
            if (SUCCEEDED(item->HasVideo(&hasVideo, &selected))) hasVideo_ = hasVideo && selected;
        }
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(player_->GetDuration(GUID_NULL, &value)) && value.vt == VT_UI8)
            duration_ = static_cast<long long>(value.uhVal.QuadPart);
        PropVariantClear(&value);
        InvalidateRect(surface_, nullptr, FALSE);
        break;
    }
    case MFP_EVENT_TYPE_PLAY:
        SetState(State::Playing);
        break;
    case MFP_EVENT_TYPE_PAUSE:
        SetState(State::Paused);
        break;
    case MFP_EVENT_TYPE_PLAYBACK_ENDED:
        SetState(State::Ended);
        break;
    default:
        break;
    }
}

void VideoView::SetState(State state) {
    state_ = state;
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateRect(surface_, nullptr, FALSE);
    if (callbacks_.changed) callbacks_.changed();
}

long long VideoView::Position() const {
    if (!player_) return 0;
    if (state_ == State::Ended) return duration_;
    PROPVARIANT value;
    PropVariantInit(&value);
    long long position = 0;
    if (SUCCEEDED(player_->GetPosition(GUID_NULL, &value))) {
        if (value.vt == VT_I8) position = value.hVal.QuadPart;
        else if (value.vt == VT_UI8) position = static_cast<long long>(value.uhVal.QuadPart);
    }
    PropVariantClear(&value);
    return position;
}

void VideoView::TogglePlay() {
    if (!player_) return;
    if (state_ == State::Playing) {
        player_->Pause();
    } else if (state_ == State::Paused || state_ == State::Ended || state_ == State::Loading) {
        if (state_ == State::Ended) SeekTo(0);
        player_->Play();
    }
}

void VideoView::Seek(int seconds) {
    if (!player_ || duration_ <= 0) return;
    SeekTo(Position() + seconds * kSecond);
}

void VideoView::SeekTo(long long position) {
    if (!player_ || duration_ <= 0) return;
    position = std::clamp<long long>(position, 0, duration_);
    PROPVARIANT value;
    PropVariantInit(&value);
    value.vt = VT_I8;
    value.hVal.QuadPart = position;
    player_->SetPosition(GUID_NULL, &value);
    if (state_ == State::Ended) state_ = State::Paused;
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (callbacks_.changed) callbacks_.changed();
}

void VideoView::Clear() {
    if (hwnd_) KillTimer(hwnd_, kTickTimer);
    // Detach first so a late event cannot reach this view, then shut down.
    if (callback_) callback_->Detach();
    if (player_) {
        player_->Shutdown();
        player_.Reset();
    }
    callback_.Reset();
    path_.clear();
    duration_ = 0;
    hasVideo_ = false;
    wheelRemainder_ = 0;
    state_ = State::Empty;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    if (surface_) InvalidateRect(surface_, nullptr, FALSE);
}

void VideoView::SetFullscreen(bool fullscreen) {
    fullscreen_ = fullscreen;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void VideoView::OnThemeChanged() { InvalidateRect(hwnd_, nullptr, FALSE); }
