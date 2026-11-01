#include "ui/MainWindow.h"

#include <commctrl.h>
#include <windowsx.h>

#include <climits>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <new>

#include "core/Settings.h"
#include "res/resource.h"
#include "ui/Commands.h"

namespace {

constexpr wchar_t kClassName[] = L"LiteViewMainWindow";
constexpr int kMinWidth = 480;   // in 96-DPI pixels
constexpr int kMinHeight = 320;
constexpr UINT kSettingsReady = WM_APP + 1;
constexpr UINT kSettingsSaved = WM_APP + 2;

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

bool FocusIsTextInput() {
    HWND focus = GetFocus();
    if (!focus) return false;
    wchar_t cls[32] = {};
    GetClassNameW(focus, cls, ARRAYSIZE(cls));
    return lstrcmpiW(cls, WC_EDITW) == 0;
}

}  // namespace

struct MainWindow::SettingsState {
    std::mutex mutex;
    std::condition_variable changed;
    HWND hwnd = nullptr;
    bool stop = false;
    bool closeRequested = false;
    bool saveRequested = false;
    bool canRestore = false;
    RECT normal{};
    bool maximized = false;
    HRESULT readResult = S_OK;
    HRESULT writeResult = S_OK;
};

MainWindow::~MainWindow() {
    if (settingsState_) {
        {
            std::lock_guard<std::mutex> lock(settingsState_->mutex);
            settingsState_->stop = true;
            settingsState_->hwnd = nullptr;
        }
        settingsState_->changed.notify_one();
    }
    if (settingsThread_.joinable()) settingsThread_.join();
}

HRESULT MainWindow::Create(HINSTANCE instance, int showCommand) {
    instance_ = instance;
    showCommand_ = showCommand;

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &MainWindow::WndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MAINMENU);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return HRESULT_FROM_WIN32(GetLastError());

    accelBrowser_ = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDR_ACCEL_BROWSER));
    accelViewer_ = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDR_ACCEL_VIEWER));

    const std::wstring title = LoadStr(IDS_APP_TITLE);
    hwnd_ = CreateWindowExW(0, kClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) {
        // WM_CREATE fails when a child control could not be created.
        const DWORD err = GetLastError();
        return err ? HRESULT_FROM_WIN32(err) : E_FAIL;
    }

    const HRESULT settingsResult = StartSettingsWorker();
    if (FAILED(settingsResult)) {
        DestroyWindow(hwnd_);
        return settingsResult;
    }

    // Keep the window hidden until the background settings read completes.
    // The message loop can run while the disk is slow, without displaying the
    // default placement first and then jumping to the saved placement.
    return S_OK;
}

bool MainWindow::TranslateAccelerator(MSG* msg) {
    if (!hwnd_) return false;
    // Let edit boxes (path bar, in-place rename) keep Backspace, Delete, Ctrl+A...
    if (FocusIsTextInput()) return false;
    HACCEL table = (mode_ == Mode::Viewer) ? accelViewer_ : accelBrowser_;
    return table && ::TranslateAcceleratorW(hwnd_, table, msg) != 0;
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        return OnCreate() ? 0 : -1;
    case WM_SIZE:
        OnSize();
        return 0;
    case WM_COMMAND:
        OnCommand(LOWORD(wParam));
        return 0;
    case WM_NOTIFY:
        return OnNotify(reinterpret_cast<NMHDR*>(lParam));
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Scale(kMinWidth, dpi_);
        info->ptMinTrackSize.y = Scale(kMinHeight, dpi_);
        return 0;
    }
    case WM_DPICHANGED:
        OnDpiChanged(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
        return 0;
    case WM_CLOSE:
        OnClose();
        return 0;
    case kSettingsReady:
        OnSettingsReady(wParam);
        return 0;
    case kSettingsSaved:
        OnSettingsSaved(wParam);
        return 0;
    case WM_DESTROY:
        if (settingsState_) {
            {
                std::lock_guard<std::mutex> lock(settingsState_->mutex);
                settingsState_->stop = true;
                settingsState_->hwnd = nullptr;
            }
            settingsState_->changed.notify_one();
        }
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY: {
        HWND hwnd = hwnd_;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

bool MainWindow::OnCreate() {
    dpi_ = GetDpiForWindow(hwnd_);
    CreateToolbar();
    CreateStatusBar();
    return toolbar_ && status_;
}

void MainWindow::CreateToolbar() {
    toolbar_ = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
                               WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_TOP,
                               0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);
    if (!toolbar_) return;
    SendMessageW(toolbar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(toolbar_, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DOUBLEBUFFER);

    // Placeholder system images until LiteView gets its own toolbar bitmap.
    const auto stdBase = static_cast<int>(
        SendMessageW(toolbar_, TB_LOADIMAGES, IDB_STD_SMALL_COLOR, reinterpret_cast<LPARAM>(HINST_COMMCTRL)));
    const auto viewBase = static_cast<int>(
        SendMessageW(toolbar_, TB_LOADIMAGES, IDB_VIEW_SMALL_COLOR, reinterpret_cast<LPARAM>(HINST_COMMCTRL)));

    const TBBUTTON buttons[] = {
        {viewBase + VIEW_PARENTFOLDER, IDM_NAV_PARENT_FOLDER, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
        {0, 0, 0, BTNS_SEP, {}, 0, 0},
        {viewBase + VIEW_LARGEICONS, IDM_NAV_TOGGLE_MODE, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
        {0, 0, 0, BTNS_SEP, {}, 0, 0},
        {stdBase + STD_UNDO, IDM_ROTATE_LEFT, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
        {stdBase + STD_REDOW, IDM_ROTATE_RIGHT, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
        {0, 0, 0, BTNS_SEP, {}, 0, 0},
        {stdBase + STD_DELETE, IDM_EDIT_DELETE, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
        {stdBase + STD_PROPERTIES, IDM_FILE_PROPERTIES, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
    };
    SendMessageW(toolbar_, TB_ADDBUTTONSW, ARRAYSIZE(buttons), reinterpret_cast<LPARAM>(buttons));
    SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
}

void MainWindow::CreateStatusBar() {
    status_ = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                              0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);
    if (!status_) return;
    UpdateStatusParts();
    const std::wstring ready = LoadStr(IDS_STATUS_READY);
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(ready.c_str()));
}

void MainWindow::UpdateStatusParts() {
    if (!status_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    // Parts: message | file count | selection | selection size
    const int w = Scale(140, dpi_);
    int parts[4] = {rc.right - 3 * w, rc.right - 2 * w, rc.right - w, -1};
    if (parts[0] < w) parts[0] = w;
    SendMessageW(status_, SB_SETPARTS, ARRAYSIZE(parts), reinterpret_cast<LPARAM>(parts));
}

void MainWindow::OnSize() {
    if (toolbar_) SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
    if (status_) {
        SendMessageW(status_, WM_SIZE, 0, 0);
        UpdateStatusParts();
    }
    // T4: lay out folder tree, preview pane and file list in the remaining area.
}

void MainWindow::OnCommand(UINT id) {
    switch (id) {
    case IDM_FILE_EXIT:
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        break;
    case IDM_HELP_ABOUT: {
        const std::wstring text = LoadStr(IDS_ABOUT_TEXT);
        const std::wstring title = LoadStr(IDS_APP_TITLE);
        MessageBoxW(hwnd_, text.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
        break;
    }
    default:
        // Commands are wired up by later work tickets (T2–T8).
        break;
    }
}

LRESULT MainWindow::OnNotify(NMHDR* header) {
    if (header->code == TTN_GETDISPINFOW) {
        auto* info = reinterpret_cast<NMTTDISPINFOW*>(header);
        info->hinst = instance_;
        info->lpszText = MAKEINTRESOURCEW(header->idFrom);  // tooltip string ID == command ID
        return 0;
    }
    return 0;
}

void MainWindow::OnDpiChanged(UINT dpi, const RECT* suggested) {
    dpi_ = dpi;
    SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                 suggested->right - suggested->left, suggested->bottom - suggested->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

std::wstring MainWindow::LoadStr(UINT id) const {
    const wchar_t* text = nullptr;
    const int len = LoadStringW(instance_, id, reinterpret_cast<LPWSTR>(&text), 0);
    return (len > 0 && text) ? std::wstring(text, static_cast<size_t>(len)) : std::wstring();
}

HRESULT MainWindow::StartSettingsWorker() noexcept {
    try {
        settingsState_ = std::make_shared<SettingsState>();
        settingsState_->hwnd = hwnd_;
        settingsThread_ = std::thread(&MainWindow::SettingsWorker, settingsState_);
        return S_OK;
    } catch (const std::bad_alloc&) {
        settingsState_.reset();
        return E_OUTOFMEMORY;
    } catch (...) {
        settingsState_.reset();
        return E_FAIL;
    }
}

void MainWindow::SettingsWorker(const std::shared_ptr<SettingsState>& state) noexcept {
    Settings settings;
    const HRESULT initResult = Settings::ForCurrentProcess(settings);
    HRESULT readResult = initResult;
    RECT initial{};
    bool maximized = false;
    bool canRestore = false;
    if (SUCCEEDED(readResult)) {
        int left = INT_MIN, top = INT_MIN, right = INT_MIN, bottom = INT_MIN;
        readResult = settings.ReadInt(L"Window", L"Left", INT_MIN, left);
        if (SUCCEEDED(readResult)) readResult = settings.ReadInt(L"Window", L"Top", INT_MIN, top);
        if (SUCCEEDED(readResult)) readResult = settings.ReadInt(L"Window", L"Right", INT_MIN, right);
        if (SUCCEEDED(readResult)) readResult = settings.ReadInt(L"Window", L"Bottom", INT_MIN, bottom);
        if (SUCCEEDED(readResult)) readResult = settings.ReadBool(L"Window", L"Maximized", false, maximized);
        const std::int64_t width = static_cast<std::int64_t>(right) - left;
        const std::int64_t height = static_cast<std::int64_t>(bottom) - top;
        canRestore = SUCCEEDED(readResult) && left != INT_MIN && top != INT_MIN &&
                     right != INT_MIN && bottom != INT_MIN && width >= kMinWidth &&
                     height >= kMinHeight && width <= 100000 && height <= 100000;
        initial = RECT{left, top, right, bottom};
    }

    RECT final{};
    bool finalMaximized = false;
    bool save = false;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->readResult = readResult;
        // The user may have closed the window while settings were loading.
        // In that case OnClose has already recorded the final placement.
        if (!state->closeRequested) {
            state->normal = initial;
            state->maximized = maximized;
            state->canRestore = canRestore;
        }
        if (!state->stop && !state->closeRequested && state->hwnd) {
            PostMessageW(state->hwnd, kSettingsReady, reinterpret_cast<WPARAM>(state.get()), 0);
        }
        state->changed.wait(lock, [&] { return state->stop || state->closeRequested; });
        save = !state->stop && state->saveRequested;
        final = state->normal;
        finalMaximized = state->maximized;
    }

    // A malformed saved value should not prevent writing a corrected one.
    HRESULT writeResult = initResult;
    if (save && SUCCEEDED(writeResult)) {
        writeResult = settings.WriteInt(L"Window", L"Left", final.left);
        if (SUCCEEDED(writeResult)) writeResult = settings.WriteInt(L"Window", L"Top", final.top);
        if (SUCCEEDED(writeResult)) writeResult = settings.WriteInt(L"Window", L"Right", final.right);
        if (SUCCEEDED(writeResult)) writeResult = settings.WriteInt(L"Window", L"Bottom", final.bottom);
        if (SUCCEEDED(writeResult)) writeResult = settings.WriteBool(L"Window", L"Maximized", finalMaximized);
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->writeResult = writeResult;
        if (!state->stop && state->closeRequested && state->hwnd) {
            PostMessageW(state->hwnd, kSettingsSaved, reinterpret_cast<WPARAM>(state.get()), 0);
        }
    }
}

void MainWindow::OnSettingsReady(WPARAM token) {
    if (!settingsState_ || token != reinterpret_cast<WPARAM>(settingsState_.get()) || closing_) return;
    RECT rect{};
    bool maximized = false;
    bool canRestore = false;
    HRESULT hr = S_OK;
    {
        std::lock_guard<std::mutex> lock(settingsState_->mutex);
        rect = settingsState_->normal;
        maximized = settingsState_->maximized;
        canRestore = settingsState_->canRestore;
        hr = settingsState_->readResult;
    }
    int command = showCommand_;
    if (canRestore && MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)) {
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (GetWindowPlacement(hwnd_, &placement)) {
            placement.rcNormalPosition = rect;
            placement.showCmd = SW_HIDE;
            if (SetWindowPlacement(hwnd_, &placement) && maximized && command != SW_HIDE &&
                command != SW_SHOWMINIMIZED && command != SW_MINIMIZE &&
                command != SW_SHOWMINNOACTIVE && command != SW_FORCEMINIMIZE) {
                command = SW_SHOWMAXIMIZED;
            }
        }
    }
    ShowWindow(hwnd_, command);
    UpdateWindow(hwnd_);
    if (FAILED(hr)) {
        MessageBoxW(hwnd_, LoadStr(IDS_ERR_SETTINGS_READ).c_str(), LoadStr(IDS_APP_TITLE).c_str(),
                    MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::OnClose() {
    if (closing_) return;
    closing_ = true;
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!settingsState_ || !GetWindowPlacement(hwnd_, &placement)) {
        DestroyWindow(hwnd_);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(settingsState_->mutex);
        settingsState_->normal = placement.rcNormalPosition;
        settingsState_->maximized = placement.showCmd == SW_SHOWMAXIMIZED ||
            (placement.showCmd == SW_SHOWMINIMIZED && (placement.flags & WPF_RESTORETOMAXIMIZED));
        settingsState_->saveRequested = true;
        settingsState_->closeRequested = true;
    }
    settingsState_->changed.notify_one();
}

void MainWindow::OnSettingsSaved(WPARAM token) {
    if (!settingsState_ || token != reinterpret_cast<WPARAM>(settingsState_.get())) return;
    HRESULT hr;
    {
        std::lock_guard<std::mutex> lock(settingsState_->mutex);
        hr = settingsState_->writeResult;
    }
    if (FAILED(hr)) {
        MessageBoxW(hwnd_, LoadStr(IDS_ERR_SETTINGS_WRITE).c_str(), LoadStr(IDS_APP_TITLE).c_str(),
                    MB_OK | MB_ICONWARNING);
    }
    DestroyWindow(hwnd_);
}
