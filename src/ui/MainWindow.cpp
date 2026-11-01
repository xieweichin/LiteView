#include "ui/MainWindow.h"

#include <commctrl.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <climits>
#include <cwchar>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <new>

#include "core/BatchRename.h"
#include "core/FileOps.h"
#include "core/Rotator.h"
#include "core/Settings.h"
#include "core/ThreadPool.h"
#include "res/resource.h"
#include "ui/Commands.h"
#include "ui/BatchRenameDialog.h"
#include "ui/Messages.h"
#include "ui/PropertiesDialog.h"

namespace {

constexpr wchar_t kClassName[] = L"LiteViewMainWindow";
constexpr int kMinWidth = 480;   // in 96-DPI pixels
constexpr int kMinHeight = 320;
constexpr UINT kSettingsReady = WM_APP_SETTINGS_READY;
constexpr UINT kSettingsSaved = WM_APP_SETTINGS_SAVED;
constexpr UINT_PTR kSettingsTimer = 1;
constexpr UINT_PTR kSlideshowTimer = 2;
constexpr UINT kIdTree = 10;
constexpr UINT kIdList = 11;
constexpr UINT kIdPathBar = 12;
constexpr int kSplitter = 5;        // 96-DPI pixels
constexpr int kMinPane = 80;        // 96-DPI pixels
constexpr int kGap = 3;             // 96-DPI pixels
constexpr UINT kSettingsWaitMs = 200;

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

bool FocusIsTextInput() {
    HWND focus = GetFocus();
    if (!focus) return false;
    wchar_t cls[32] = {};
    GetClassNameW(focus, cls, ARRAYSIZE(cls));
    return lstrcmpiW(cls, WC_EDITW) == 0;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// Full, backslash-separated path without a trailing separator except for
// drive roots ("C:\").
std::wstring NormalizeFolder(const std::wstring& path) {
    std::wstring text = path;
    // Strip surrounding quotes and spaces from a pasted path.
    while (!text.empty() && (text.back() == L' ' || text.back() == L'"')) text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && (text[start] == L' ' || text[start] == L'"')) ++start;
    text = text.substr(start);
    if (text.empty()) return text;
    for (auto& c : text) if (c == L'/') c = L'\\';
    wchar_t full[MAX_PATH * 4] = {};
    const DWORD len = GetFullPathNameW(text.c_str(), ARRAYSIZE(full), full, nullptr);
    if (len > 0 && len < ARRAYSIZE(full)) text = full;
    while (text.size() > 3 && text.back() == L'\\') text.pop_back();
    if (text.size() == 2 && text[1] == L':') text += L'\\';
    return text;
}

std::wstring ParentOf(const std::wstring& folder) {
    if (folder.size() <= 3) return std::wstring();  // drive root
    const auto slash = folder.find_last_of(L'\\');
    if (slash == std::wstring::npos) return std::wstring();
    if (slash < 3) return folder.substr(0, 3);
    if (slash <= 2 || (folder.compare(0, 2, L"\\\\") == 0 && folder.find(L'\\', 2) == slash)) {
        return std::wstring();  // "\\server\share" has no listable parent
    }
    return folder.substr(0, slash);
}

std::wstring PicturesFolder() {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &path)) && path) {
        result = path;
    }
    CoTaskMemFree(path);
    if (result.empty()) {
        wchar_t drive[MAX_PATH] = {};
        if (GetWindowsDirectoryW(drive, ARRAYSIZE(drive)) >= 3) result.assign(drive, 3);
    }
    return result;
}

bool IsDirectory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Format(const std::wstring& pattern, const std::wstring& arg) {
    std::wstring text = pattern;
    const auto at = text.find(L"%s");
    if (at != std::wstring::npos) text.replace(at, 2, arg);
    return text;
}

std::wstring HexResult(HRESULT hr) {
    wchar_t text[16] = {};
    swprintf(text, 16, L"0x%08lX", static_cast<unsigned long>(hr));
    return text;
}

std::wstring Format(const std::wstring& pattern, std::size_t value) {
    std::wstring text = pattern;
    const auto at = text.find(L"%u");
    if (at != std::wstring::npos) text.replace(at, 2, std::to_wstring(value));
    return text;
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
    BrowserSettings browser;
    HRESULT readResult = S_OK;
    HRESULT writeResult = S_OK;
};

struct MainWindow::RotateState {
    std::mutex mutex;
    HWND hwnd = nullptr;
    struct Result {
        std::wstring path;
        ThumbnailRequest thumbnail;  // as listed before the rotation
        HRESULT result;
    };
    std::vector<Result> done;
};

namespace {
struct WorkerCom {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~WorkerCom() { if (SUCCEEDED(result)) CoUninitialize(); }
};
}  // namespace

MainWindow::MainWindow()
    : pool_(std::make_shared<ThreadPool>(ThreadPool::DefaultThreadCount())),
      model_(std::make_unique<FolderModel>(pool_)),
      list_(std::make_unique<FileListView>(*model_)) {}

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
    // Finish the file being rotated, drop the rest.
    if (rotateState_) {
        std::lock_guard<std::mutex> lock(rotateState_->mutex);
        rotateState_->hwnd = nullptr;
    }
    if (rotatePool_) rotatePool_->Shutdown();
    // Stop folder loads and thumbnail work before the pool goes away.
    list_.reset();
    thumbnails_.reset();
    viewerCache_.reset();
    model_.reset();
    if (pool_) pool_->Shutdown();
}

HRESULT MainWindow::Create(HINSTANCE instance, int showCommand, const std::wstring& startPath) {
    instance_ = instance;
    showCommand_ = showCommand;
    startPath_ = startPath;

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &MainWindow::WndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);  // splitter gaps
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

    // Wait briefly for settings without blocking the message loop. A slow
    // disk must not leave the application invisible indefinitely.
    if (!SetTimer(hwnd_, kSettingsTimer, kSettingsWaitMs, nullptr)) {
        ShowInitialWindow(showCommand_);
    }
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
    case WM_TIMER:
        if (wParam == kSlideshowTimer) {
            if (mode_ == Mode::Viewer && model_->Count() > 0) {
                const int index = ViewerIndex();
                const int next = (index + 1) % static_cast<int>(model_->Count());  // wraps to the first picture
                ShowViewerIndex(next, 1);
            } else {
                StopSlideshow();
            }
            return 0;
        }
        if (wParam == kSettingsTimer) {
            KillTimer(hwnd_, kSettingsTimer);
            if (!closing_) {
                // Settings are slow: start with defaults rather than wait.
                InitBrowser(BrowserSettings{});
                ShowInitialWindow(showCommand_);
            }
            return 0;
        }
        break;
    case kSettingsReady:
        OnSettingsReady(wParam);
        return 0;
    case kSettingsSaved:
        OnSettingsSaved(wParam);
        return 0;
    case WM_APP_FOLDER_LOADED:
        OnFolderLoaded(static_cast<std::uint32_t>(wParam));
        return 0;
    case WM_APP_THUMB_READY:
        if (list_) list_->OnThumbnailReady(static_cast<std::uint32_t>(wParam), static_cast<std::uint32_t>(lParam));
        return 0;
    case WM_APP_ROTATED:
        OnRotated();
        return 0;
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wParam) == list_->Hwnd() || reinterpret_cast<HWND>(wParam) == viewer_.Hwnd()) {
            ShowContextMenu(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        }
        break;
    case WM_APP_VIEWER_READY:
        RefreshViewerImage();
        return 0;
    case WM_APP_TREE_CHILDREN:
        tree_.OnChildrenReady();
        return 0;
    case WM_APP_UPDATE_STATUS:
        statusPending_ = false;
        UpdateStatus();
        return 0;
    case WM_APP_PATH_ENTERED:
        NavigateToEnteredPath();
        return 0;
    case WM_INITMENUPOPUP:
        OnInitMenuPopup(reinterpret_cast<HMENU>(wParam));
        return 0;
    case WM_SETFOCUS:
        if (mode_ == Mode::Viewer && viewer_.Hwnd()) {
            SetFocus(viewer_.Hwnd());
        } else if (list_ && list_->Hwnd()) {
            SetFocus(list_->Hwnd());
        }
        return 0;
    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(wParam) == hwnd_ && LOWORD(lParam) == HTCLIENT && OnSetCursor()) return TRUE;
        break;
    case WM_LBUTTONDOWN:
        OnLButtonDown(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    case WM_MOUSEMOVE:
        OnMouseMove(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    case WM_LBUTTONUP:
        OnLButtonUp();
        return 0;
    case WM_CAPTURECHANGED:
        dragging_ = Splitter::None;
        return 0;
    case WM_DESTROY:
        if (font_) {
            DeleteObject(font_);
            font_ = nullptr;
        }
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
    NONCLIENTMETRICSW metrics = {sizeof(metrics)};
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_)) {
        font_ = CreateFontIndirectW(&metrics.lfMessageFont);
    }
    CreateToolbar();
    CreatePathBar();
    CreateStatusBar();
    if (!toolbar_ || !status_ || !pathBar_) return false;
    if (FAILED(tree_.Create(hwnd_, instance_, kIdTree, hwnd_))) return false;
    if (FAILED(preview_.Create(hwnd_, instance_))) return false;
    if (FAILED(list_->Create(hwnd_, instance_, kIdList))) return false;

    const HWND hwnd = hwnd_;
    thumbnails_ = std::make_unique<ThumbnailService>(
        [hwnd](std::uint32_t generation, std::uint32_t itemId, HRESULT) {
            PostMessageW(hwnd, WM_APP_THUMB_READY, static_cast<WPARAM>(generation), static_cast<LPARAM>(itemId));
        },
        ThumbnailService::Options{});
    list_->SetThumbnailService(thumbnails_.get());

    viewerCache_ = std::make_unique<ViewerCache>([hwnd](const std::wstring&, HRESULT) {
        PostMessageW(hwnd, WM_APP_VIEWER_READY, 0, 0);
    });
    ViewerView::Callbacks callbacks;
    callbacks.navigate = [this](int step) { ViewerStep(step); };
    callbacks.exit = [this] { ExitViewer(); };
    callbacks.needFullSize = [this] {
        if (!viewerPath_.empty()) viewerCache_->Request(viewerPath_, 0, true);
    };
    callbacks.viewChanged = [this] { UpdateViewerStatus(); };
    if (FAILED(viewer_.Create(hwnd_, instance_, std::move(callbacks)))) return false;
    menu_ = GetMenu(hwnd_);
    list_->SetRenameCallback([this](int index, const std::wstring& name) { return RenameItem(index, name); });
    rotateState_ = std::make_shared<RotateState>();
    rotateState_->hwnd = hwnd_;
    rotatePool_ = std::make_unique<ThreadPool>(1);  // one file at a time, in order

    tree_.SetNavigateCallback([this](const std::wstring& path) { Navigate(path); });
    list_->SetColumnClickCallback([this](SortKey key) { OnSortCommand(key); });
    list_->SetSelectionChangedCallback([this] { RequestStatusUpdate(); });
    list_->SetActivateCallback([this](int index) { EnterViewer(index); });
    return true;
}

void MainWindow::CreatePathBar() {
    pathBar_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, nullptr,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdPathBar)), instance_, nullptr);
    if (!pathBar_) return;
    if (font_) SendMessageW(pathBar_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    SetWindowSubclass(pathBar_, &MainWindow::PathBarProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SHAutoComplete(pathBar_, SHACF_FILESYS_DIRS | SHACF_AUTOSUGGEST_FORCE_ON | SHACF_AUTOAPPEND_FORCE_OFF);
}

LRESULT CALLBACK MainWindow::PathBarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR,
                                         DWORD_PTR data) {
    auto* self = reinterpret_cast<MainWindow*>(data);
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            PostMessageW(self->hwnd_, WM_APP_PATH_ENTERED, 0, 0);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            SetWindowTextW(hwnd, self->listFolder_.c_str());
            SetFocus(self->list_->Hwnd());
            return 0;
        }
        break;
    case WM_CHAR:
        if (wParam == L'\r' || wParam == 27) return 0;  // no beep
        break;
    case WM_GETDLGCODE:
        return DefSubclassProc(hwnd, msg, wParam, lParam) | DLGC_WANTALLKEYS;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, &MainWindow::PathBarProc, 1);
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
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
    Layout();
}

void MainWindow::Layout() {
    if (!hwnd_ || !pathBar_ || !list_ || !list_->Hwnd()) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int top = 0;
    if (toolbar_) {
        RECT tb{};
        GetWindowRect(toolbar_, &tb);
        top = tb.bottom - tb.top;
    }
    int bottom = rc.bottom;
    if (status_) {
        RECT sb{};
        GetWindowRect(status_, &sb);
        bottom -= sb.bottom - sb.top;
    }
    const int gap = Scale(kGap, dpi_);
    const int splitter = Scale(kSplitter, dpi_);

    TEXTMETRICW tm{};
    HDC dc = GetDC(pathBar_);
    HGDIOBJ old = SelectObject(dc, font_ ? static_cast<HGDIOBJ>(font_) : GetStockObject(DEFAULT_GUI_FONT));
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(pathBar_, dc);
    const int pathHeight = tm.tmHeight + Scale(8, dpi_);

    if (mode_ == Mode::Viewer) {
        // The picture takes everything between toolbar and status bar
        // (the whole client area in full screen).
        const int viewerTop = fullscreen_ ? 0 : top;
        const int viewerBottom = fullscreen_ ? rc.bottom : bottom;
        SetWindowPos(viewer_.Hwnd(), nullptr, 0, viewerTop, rc.right, viewerBottom - viewerTop,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }

    HDWP dwp = BeginDeferWindowPos(4);
    auto place = [&](HWND child, int x, int y, int w, int h) {
        if (dwp) dwp = DeferWindowPos(dwp, child, nullptr, x, y, std::max(w, 0), std::max(h, 0),
                                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    };
    place(pathBar_, gap, top + gap, rc.right - 2 * gap, pathHeight);

    content_ = RECT{0, top + gap + pathHeight + gap, rc.right, bottom};
    const int width = content_.right - content_.left;
    const int height = content_.bottom - content_.top;
    const int minPane = Scale(kMinPane, dpi_);
    int left = Scale(leftWidth96_, dpi_);
    left = std::min(left, width - splitter - minPane);
    left = std::max(left, std::min(minPane, width / 2));
    int tree = Scale(treeHeight96_, dpi_);
    tree = std::min(tree, height - splitter - minPane / 2);
    tree = std::max(tree, std::min(minPane, height / 2));

    place(tree_.Hwnd(), 0, content_.top, left, tree);
    place(preview_.Hwnd(), 0, content_.top + tree + splitter, left, height - tree - splitter);
    place(list_->Hwnd(), left + splitter, content_.top, width - left - splitter, height);
    if (dwp) EndDeferWindowPos(dwp);
}

MainWindow::Splitter MainWindow::HitTestSplitter(POINT pt) const {
    if (mode_ != Mode::Browser || !PtInRect(&content_, pt)) return Splitter::None;
    const int splitter = Scale(kSplitter, dpi_);
    RECT treeRect{};
    GetWindowRect(tree_.Hwnd(), &treeRect);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&treeRect), 2);
    if (pt.x >= treeRect.right && pt.x < treeRect.right + splitter) return Splitter::Vertical;
    if (pt.x < treeRect.right && pt.y >= treeRect.bottom && pt.y < treeRect.bottom + splitter) {
        return Splitter::Horizontal;
    }
    return Splitter::None;
}

bool MainWindow::OnSetCursor() {
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(hwnd_, &pt);
    const Splitter hit = dragging_ != Splitter::None ? dragging_ : HitTestSplitter(pt);
    if (hit == Splitter::None) return false;
    SetCursor(LoadCursorW(nullptr, hit == Splitter::Vertical ? IDC_SIZEWE : IDC_SIZENS));
    return true;
}

void MainWindow::OnLButtonDown(POINT pt) {
    dragging_ = HitTestSplitter(pt);
    if (dragging_ == Splitter::None) return;
    RECT treeRect{};
    GetWindowRect(tree_.Hwnd(), &treeRect);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&treeRect), 2);
    dragOffset_ = dragging_ == Splitter::Vertical ? pt.x - treeRect.right : pt.y - treeRect.bottom;
    SetCapture(hwnd_);
}

void MainWindow::OnMouseMove(POINT pt) {
    if (dragging_ == Splitter::None || GetCapture() != hwnd_) return;
    if (dragging_ == Splitter::Vertical) {
        leftWidth96_ = MulDiv(std::max(0, static_cast<int>(pt.x) - dragOffset_), USER_DEFAULT_SCREEN_DPI,
                              static_cast<int>(dpi_));
    } else {
        treeHeight96_ = MulDiv(std::max(0, static_cast<int>(pt.y - content_.top) - dragOffset_),
                               USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi_));
    }
    Layout();
}

void MainWindow::OnLButtonUp() {
    if (dragging_ == Splitter::None) return;
    dragging_ = Splitter::None;
    ReleaseCapture();
    // Store what the layout actually used, so a clamped drag does not leave
    // an out-of-range value in the settings.
    RECT treeRect{};
    GetWindowRect(tree_.Hwnd(), &treeRect);
    leftWidth96_ = MulDiv(treeRect.right - treeRect.left, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi_));
    treeHeight96_ = MulDiv(treeRect.bottom - treeRect.top, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi_));
}

void MainWindow::OnCommand(UINT id) {
    switch (id) {
    case IDM_FILE_EXIT:
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        break;
    case IDM_FILE_OPEN_FOLDER: {
        IFileOpenDialog* dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&dialog)))) {
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            IShellItem* current = nullptr;
            if (!listFolder_.empty() &&
                SUCCEEDED(SHCreateItemFromParsingName(listFolder_.c_str(), nullptr, IID_PPV_ARGS(&current)))) {
                dialog->SetFolder(current);
                current->Release();
            }
            IShellItem* picked = nullptr;
            if (SUCCEEDED(dialog->Show(hwnd_)) && SUCCEEDED(dialog->GetResult(&picked))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(picked->GetDisplayName(SIGDN_FILESYSPATH, &path))) Navigate(path);
                CoTaskMemFree(path);
                picked->Release();
            }
            dialog->Release();
        }
        break;
    }
    case IDM_NAV_PARENT_FOLDER:
        if (mode_ == Mode::Browser) NavigateToParent();
        break;
    case IDM_FILE_NEW_FOLDER:
        NewFolder();
        break;
    case IDM_FILE_PROPERTIES:
        ShowProperties();
        break;
    case IDM_EDIT_COPY_TO:
        CopyOrMoveSelection(false);
        break;
    case IDM_EDIT_MOVE_TO:
        CopyOrMoveSelection(true);
        break;
    case IDM_EDIT_DELETE:
        DeleteSelection();
        break;
    case IDM_EDIT_RENAME:
        RenameSelection();
        break;
    case IDM_EDIT_BATCH_RENAME:
        BatchRenameSelection();
        break;
    case IDM_EDIT_UNDO_BATCH_RENAME:
        UndoBatchRename();
        break;
    case IDM_ROTATE_LEFT:
        RotateSelection(false);
        break;
    case IDM_ROTATE_RIGHT:
        RotateSelection(true);
        break;
    case IDM_NAV_TOGGLE_MODE:
        if (mode_ == Mode::Viewer) {
            ExitViewer();
        } else {
            EnterViewer(list_->FocusedIndex());
        }
        break;
    case IDM_NAV_EXIT_VIEWER:
        if (mode_ == Mode::Viewer) ExitViewer();
        break;
    case IDM_NAV_NEXT:
        StopSlideshow();
        ViewerStep(+1);
        break;
    case IDM_NAV_PREV:
        StopSlideshow();
        ViewerStep(-1);
        break;
    case IDM_NAV_FIRST:
        StopSlideshow();
        if (mode_ == Mode::Viewer && model_->Count() > 0) ShowViewerIndex(0, 1);
        break;
    case IDM_NAV_LAST:
        StopSlideshow();
        if (mode_ == Mode::Viewer && model_->Count() > 0) ShowViewerIndex(static_cast<int>(model_->Count()) - 1, -1);
        break;
    case IDM_ZOOM_IN:
        if (mode_ == Mode::Viewer) viewer_.ZoomIn();
        break;
    case IDM_ZOOM_OUT:
        if (mode_ == Mode::Viewer) viewer_.ZoomOut();
        break;
    case IDM_ZOOM_TOGGLE_FIT:
        if (mode_ == Mode::Viewer) viewer_.ToggleFit();
        break;
    case IDM_VIEW_FULLSCREEN:
        if (mode_ == Mode::Browser) EnterViewer(list_->FocusedIndex());
        if (mode_ == Mode::Viewer) SetFullscreen(!fullscreen_);
        break;
    case IDM_VIEW_SLIDESHOW:
        ToggleSlideshow();
        break;
    case IDM_SLIDESHOW_2S: slideshowSeconds_ = 2; if (slideshow_) { StopSlideshow(); ToggleSlideshow(); } break;
    case IDM_SLIDESHOW_3S: slideshowSeconds_ = 3; if (slideshow_) { StopSlideshow(); ToggleSlideshow(); } break;
    case IDM_SLIDESHOW_5S: slideshowSeconds_ = 5; if (slideshow_) { StopSlideshow(); ToggleSlideshow(); } break;
    case IDM_SLIDESHOW_10S: slideshowSeconds_ = 10; if (slideshow_) { StopSlideshow(); ToggleSlideshow(); } break;
    case IDM_VIEW_REFRESH:
        if (mode_ == Mode::Browser) Refresh();
        break;
    case IDM_EDIT_SELECT_ALL:
        if (mode_ == Mode::Browser) list_->SelectAll();
        break;
    case IDM_VIEW_THUMBNAILS: SetViewMode(FileListView::ViewMode::Thumbnails); break;
    case IDM_VIEW_LARGE_ICONS: SetViewMode(FileListView::ViewMode::LargeIcons); break;
    case IDM_VIEW_LIST: SetViewMode(FileListView::ViewMode::List); break;
    case IDM_VIEW_DETAILS: SetViewMode(FileListView::ViewMode::Details); break;
    case IDM_SORT_NAME: OnSortCommand(SortKey::Name); break;
    case IDM_SORT_SIZE: OnSortCommand(SortKey::Size); break;
    case IDM_SORT_TYPE: OnSortCommand(SortKey::Type); break;
    case IDM_SORT_MODIFIED: OnSortCommand(SortKey::Modified); break;
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
    LRESULT result = 0;
    if (tree_.OnNotify(header, result)) return result;
    if (list_->OnNotify(header, result)) return result;
    if (header->code == TTN_GETDISPINFOW) {
        auto* info = reinterpret_cast<NMTTDISPINFOW*>(header);
        info->hinst = instance_;
        info->lpszText = MAKEINTRESOURCEW(header->idFrom);  // tooltip string ID == command ID
        return 0;
    }
    return 0;
}

void MainWindow::OnInitMenuPopup(HMENU menu) {
    const int view = static_cast<int>(list_->GetViewMode());
    CheckMenuRadioItem(menu, IDM_VIEW_THUMBNAILS, IDM_VIEW_DETAILS, IDM_VIEW_THUMBNAILS + view, MF_BYCOMMAND);
    CheckMenuRadioItem(menu, IDM_SORT_NAME, IDM_SORT_MODIFIED, IDM_SORT_NAME + static_cast<int>(sortKey_),
                       MF_BYCOMMAND);
    const UINT slideshowItem = slideshowSeconds_ <= 2 ? IDM_SLIDESHOW_2S
                               : slideshowSeconds_ <= 3 ? IDM_SLIDESHOW_3S
                               : slideshowSeconds_ <= 5 ? IDM_SLIDESHOW_5S
                                                        : IDM_SLIDESHOW_10S;
    CheckMenuRadioItem(menu, IDM_SLIDESHOW_2S, IDM_SLIDESHOW_10S, slideshowItem, MF_BYCOMMAND);
    CheckMenuItem(menu, IDM_VIEW_FULLSCREEN, MF_BYCOMMAND | (fullscreen_ ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, IDM_VIEW_SLIDESHOW, MF_BYCOMMAND | (slideshow_ ? MF_CHECKED : MF_UNCHECKED));
    const UINT browserOnly = mode_ == Mode::Browser ? MF_ENABLED : MF_GRAYED;
    EnableMenuItem(menu, IDM_NAV_PARENT_FOLDER, MF_BYCOMMAND | browserOnly);
    EnableMenuItem(menu, IDM_VIEW_REFRESH, MF_BYCOMMAND | browserOnly);
    EnableMenuItem(menu, IDM_EDIT_SELECT_ALL, MF_BYCOMMAND | browserOnly);
    EnableMenuItem(menu, IDM_EDIT_BATCH_RENAME, MF_BYCOMMAND | browserOnly);
    const bool canUndo = mode_ == Mode::Browser && BatchRename::CanUndo(UndoFilePath());
    EnableMenuItem(menu, IDM_EDIT_UNDO_BATCH_RENAME, MF_BYCOMMAND | (canUndo ? MF_ENABLED : MF_GRAYED));
}

void MainWindow::OnDpiChanged(UINT dpi, const RECT* suggested) {
    dpi_ = dpi;
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    NONCLIENTMETRICSW metrics = {sizeof(metrics)};
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_)) {
        font_ = CreateFontIndirectW(&metrics.lfMessageFont);
        if (font_ && pathBar_) SendMessageW(pathBar_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    if (list_) {
        list_->SetDpi(dpi);
        list_->SetViewMode(list_->GetViewMode());
    }
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
    // Browser layout. A bad value falls back to its default rather than
    // failing the whole read.
    BrowserSettings browser;
    if (SUCCEEDED(initResult)) {
        const BrowserSettings defaults;
        std::wstring folder;
        int value = 0;
        bool flag = true;
        if (SUCCEEDED(settings.ReadString(L"Browser", L"LastFolder", L"", folder)) && !folder.empty() &&
            IsDirectory(folder)) {
            browser.lastFolder = folder;
        }
        if (SUCCEEDED(settings.ReadInt(L"Browser", L"LeftWidth", defaults.leftWidth, value)) && value >= 0 &&
            value <= 10000) {
            browser.leftWidth = value;
        }
        if (SUCCEEDED(settings.ReadInt(L"Browser", L"TreeHeight", defaults.treeHeight, value)) && value >= 0 &&
            value <= 10000) {
            browser.treeHeight = value;
        }
        if (SUCCEEDED(settings.ReadInt(L"Browser", L"ViewMode", defaults.viewMode, value)) && value >= 0 &&
            value <= static_cast<int>(FileListView::ViewMode::Details)) {
            browser.viewMode = value;
        }
        if (SUCCEEDED(settings.ReadInt(L"Browser", L"SortKey", defaults.sortKey, value)) && value >= 0 &&
            value <= static_cast<int>(SortKey::Modified)) {
            browser.sortKey = value;
        }
        if (SUCCEEDED(settings.ReadBool(L"Browser", L"SortAscending", true, flag))) browser.sortAscending = flag;
        if (SUCCEEDED(settings.ReadInt(L"Viewer", L"SlideshowSeconds", defaults.slideshowSeconds, value)) &&
            value >= 1 && value <= 3600) {
            browser.slideshowSeconds = value;
        }
    }

    RECT final{};
    bool finalMaximized = false;
    BrowserSettings finalBrowser;
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
            state->browser = browser;
        }
        if (!state->stop && !state->closeRequested && state->hwnd) {
            PostMessageW(state->hwnd, kSettingsReady, reinterpret_cast<WPARAM>(state.get()), 0);
        }
        state->changed.wait(lock, [&] { return state->stop || state->closeRequested; });
        save = !state->stop && state->saveRequested;
        final = state->normal;
        finalMaximized = state->maximized;
        finalBrowser = state->browser;
    }

    // A malformed saved value should not prevent writing a corrected one.
    HRESULT writeResult = initResult;
    if (save && SUCCEEDED(writeResult)) {
        writeResult = settings.WriteInt(L"Window", L"Left", final.left);
        if (SUCCEEDED(writeResult)) writeResult = settings.WriteInt(L"Window", L"Top", final.top);
        if (SUCCEEDED(writeResult)) writeResult = settings.WriteInt(L"Window", L"Right", final.right);
        if (SUCCEEDED(writeResult)) writeResult = settings.WriteInt(L"Window", L"Bottom", final.bottom);
        if (SUCCEEDED(writeResult)) writeResult = settings.WriteBool(L"Window", L"Maximized", finalMaximized);
        if (SUCCEEDED(writeResult) && !finalBrowser.lastFolder.empty()) {
            writeResult = settings.WriteString(L"Browser", L"LastFolder", finalBrowser.lastFolder);
        }
        if (SUCCEEDED(writeResult)) writeResult = settings.WriteInt(L"Browser", L"LeftWidth", finalBrowser.leftWidth);
        if (SUCCEEDED(writeResult)) writeResult = settings.WriteInt(L"Browser", L"TreeHeight", finalBrowser.treeHeight);
        if (SUCCEEDED(writeResult)) writeResult = settings.WriteInt(L"Browser", L"ViewMode", finalBrowser.viewMode);
        if (SUCCEEDED(writeResult)) writeResult = settings.WriteInt(L"Browser", L"SortKey", finalBrowser.sortKey);
        if (SUCCEEDED(writeResult)) {
            writeResult = settings.WriteBool(L"Browser", L"SortAscending", finalBrowser.sortAscending);
        }
        if (SUCCEEDED(writeResult)) {
            writeResult = settings.WriteInt(L"Viewer", L"SlideshowSeconds", finalBrowser.slideshowSeconds);
        }
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
    KillTimer(hwnd_, kSettingsTimer);
    RECT rect{};
    bool maximized = false;
    bool canRestore = false;
    HRESULT hr = S_OK;
    BrowserSettings browser;
    {
        std::lock_guard<std::mutex> lock(settingsState_->mutex);
        rect = settingsState_->normal;
        maximized = settingsState_->maximized;
        canRestore = settingsState_->canRestore;
        hr = settingsState_->readResult;
        browser = settingsState_->browser;
    }
    int command = showCommand_;
    if (!shown_ && canRestore && MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)) {
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
    InitBrowser(browser);
    ShowInitialWindow(command);
    if (FAILED(hr)) {
        MessageBoxW(hwnd_, LoadStr(IDS_ERR_SETTINGS_READ).c_str(), LoadStr(IDS_APP_TITLE).c_str(),
                    MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::ShowInitialWindow(int command) {
    if (shown_) return;
    shown_ = true;
    ShowWindow(hwnd_, command);
    UpdateWindow(hwnd_);
}

void MainWindow::OnClose() {
    if (closing_) return;
    closing_ = true;
    KillTimer(hwnd_, kSettingsTimer);
    StopSlideshow();
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (fullscreen_) {
        placement = windowedPlacement_;  // remember the windowed position, not the full screen
    } else if (settingsState_ && !GetWindowPlacement(hwnd_, &placement)) {
        DestroyWindow(hwnd_);
        return;
    }
    if (!settingsState_) {
        DestroyWindow(hwnd_);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(settingsState_->mutex);
        settingsState_->normal = placement.rcNormalPosition;
        settingsState_->maximized = placement.showCmd == SW_SHOWMAXIMIZED ||
            (placement.showCmd == SW_SHOWMINIMIZED && (placement.flags & WPF_RESTORETOMAXIMIZED));
        BrowserSettings& browser = settingsState_->browser;
        if (browserStarted_) {
            if (!listFolder_.empty()) browser.lastFolder = listFolder_;
            browser.leftWidth = leftWidth96_;
            browser.treeHeight = treeHeight96_;
            browser.viewMode = static_cast<int>(list_->GetViewMode());
            browser.sortKey = static_cast<int>(sortKey_);
            browser.sortAscending = sortAscending_;
            browser.slideshowSeconds = slideshowSeconds_;
        }
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

// ---- Browser ----------------------------------------------------------------

void MainWindow::InitBrowser(const BrowserSettings& settings) {
    if (browserStarted_) return;
    browserStarted_ = true;
    leftWidth96_ = settings.leftWidth;
    treeHeight96_ = settings.treeHeight;
    sortKey_ = static_cast<SortKey>(settings.sortKey);
    sortAscending_ = settings.sortAscending;
    slideshowSeconds_ = settings.slideshowSeconds;
    model_->Sort(sortKey_, sortAscending_);  // nothing loaded yet: just sets the order
    list_->SetSortIndicator(sortKey_, sortAscending_);
    list_->SetViewMode(static_cast<FileListView::ViewMode>(settings.viewMode));
    Layout();

    // Command line first, then the last folder, then Pictures.
    std::wstring folder;
    std::wstring select;
    if (!startPath_.empty()) {
        const std::wstring path = NormalizeFolder(startPath_);
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            folder = path;
        } else if (attributes != INVALID_FILE_ATTRIBUTES) {
            const auto slash = path.find_last_of(L'\\');
            if (slash != std::wstring::npos) {
                folder = NormalizeFolder(path.substr(0, slash + 1));
                select = path.substr(slash + 1);
                openViewerOnLoad_ = true;  // an image on the command line opens in the viewer
            }
        }
    }
    if (folder.empty()) folder = settings.lastFolder;
    if (folder.empty()) folder = PicturesFolder();
    if (!folder.empty()) Navigate(folder, select);
    SetFocus(list_->Hwnd());
}

void MainWindow::Navigate(const std::wstring& path, const std::wstring& selectName) {
    const std::wstring folder = NormalizeFolder(path);
    if (folder.empty()) return;
    pendingFolder_ = folder;
    pendingSelect_ = selectName;
    SetWindowTextW(pathBar_, folder.c_str());
    tree_.RevealPath(folder);
    SetStatusText(0, Format(LoadStr(IDS_STATUS_LOADING), folder));

    const HWND hwnd = hwnd_;
    generation_ = model_->Open(folder, [hwnd](std::uint32_t generation) {
        // Runs on a worker under the model's lock: post only.
        PostMessageW(hwnd, WM_APP_FOLDER_LOADED, static_cast<WPARAM>(generation), 0);
    });
    if (generation_ == 0) {
        SetStatusText(0, Format(LoadStr(IDS_ERR_OPEN_FOLDER), folder));
    }
}

void MainWindow::NavigateToEnteredPath() {
    const int length = GetWindowTextLengthW(pathBar_);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(pathBar_, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    const std::wstring path = NormalizeFolder(text);
    if (path.empty()) return;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        MessageBeep(MB_ICONWARNING);
        SetStatusText(0, Format(LoadStr(IDS_ERR_OPEN_FOLDER), path));
        return;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        Navigate(path);
    } else {
        const auto slash = path.find_last_of(L'\\');
        if (slash == std::wstring::npos) return;
        Navigate(path.substr(0, slash + 1), path.substr(slash + 1));
    }
    SetFocus(list_->Hwnd());
}

void MainWindow::NavigateToParent() {
    const std::wstring& current = pendingFolder_.empty() ? listFolder_ : pendingFolder_;
    const std::wstring parent = ParentOf(current);
    if (!parent.empty()) Navigate(parent);
}

void MainWindow::Refresh() {
    if (!listFolder_.empty()) Navigate(listFolder_);
}

void MainWindow::OnFolderLoaded(std::uint32_t generation) {
    if (generation != generation_) return;  // superseded by a newer Open
    // Keep the selection across refresh and re-sort of the same folder.
    FileListView::SavedSelection saved;
    if (!pendingSelect_.empty()) {
        saved.names.push_back(pendingSelect_);
        saved.focus = pendingSelect_;
    } else if (!listFolder_.empty() && SamePath(pendingFolder_, listFolder_)) {
        saved = list_->SaveSelection();
    }
    const HRESULT hr = model_->Commit(generation);
    if (hr == E_PENDING || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    if (FAILED(hr)) {
        // Keep showing the previous folder.
        MessageBeep(MB_ICONWARNING);
        SetStatusText(0, Format(LoadStr(IDS_ERR_OPEN_FOLDER), pendingFolder_));
        SetWindowTextW(pathBar_, listFolder_.c_str());
        if (!listFolder_.empty()) tree_.RevealPath(listFolder_);
        pendingFolder_ = listFolder_;
        pendingSelect_.clear();
        return;
    }
    listFolder_ = pendingFolder_;
    const std::wstring wanted = pendingSelect_;
    pendingSelect_.clear();
    list_->Reset(saved, generation);
    if (mode_ == Mode::Viewer) {
        // Item ids are new after a reload: find the viewed picture by path.
        const int index = FindIndexByPath(viewerPath_);
        if (index < 0) {
            ExitViewer();
        } else {
            const bool changed = model_->At(static_cast<std::size_t>(index)).id != viewerItemId_;
            viewerItemId_ = model_->At(static_cast<std::size_t>(index)).id;
            if (changed) ShowViewerIndex(index, viewerDirection_);
            UpdateViewerStatus();
        }
        return;
    }
    if (selectIndexAfterLoad_ >= 0) {
        const int count = static_cast<int>(model_->Count());
        if (count > 0) list_->SelectOnly(std::min(selectIndexAfterLoad_, count - 1));
        selectIndexAfterLoad_ = -1;
    }
    SetStatusText(0, listFolder_);
    UpdateStatus();
    if (openViewerOnLoad_) {
        openViewerOnLoad_ = false;
        const int focused = list_->FocusedIndex();
        if (focused >= 0 && CompareStringOrdinal(model_->At(static_cast<std::size_t>(focused)).name.c_str(), -1,
                                                 wanted.c_str(), -1, TRUE) == CSTR_EQUAL) {
            EnterViewer(focused);
        }
    }
}

void MainWindow::OnSortCommand(SortKey key) {
    ApplySort(key, key == sortKey_ ? !sortAscending_ : true);
}

void MainWindow::ApplySort(SortKey key, bool ascending) {
    sortKey_ = key;
    sortAscending_ = ascending;
    list_->SetSortIndicator(key, ascending);
    // The model re-sorts on a worker and reports with the current generation.
    model_->Sort(key, ascending);
}

void MainWindow::SetViewMode(FileListView::ViewMode mode) { list_->SetViewMode(mode); }

void MainWindow::RequestStatusUpdate() {
    if (statusPending_ || !hwnd_) return;
    statusPending_ = PostMessageW(hwnd_, WM_APP_UPDATE_STATUS, 0, 0) != FALSE;
}

void MainWindow::UpdateStatus() {
    if (!status_) return;
    if (mode_ == Mode::Viewer) {
        UpdateViewerStatus();
        return;
    }
    const FileListView::Selection selection = list_->GetSelection();
    SetStatusText(1, Format(LoadStr(IDS_STATUS_FILES), model_->Count()));
    SetStatusText(2, Format(LoadStr(IDS_STATUS_SELECTED), selection.count));
    wchar_t size[32] = {};
    if (selection.count > 0) StrFormatByteSizeW(static_cast<LONGLONG>(selection.bytes), size, ARRAYSIZE(size));
    SetStatusText(3, size);
    UpdatePreview();
}

void MainWindow::UpdatePreview() {
    // Preview the focused picture while it is selected.
    const int focused = list_->FocusedIndex();
    std::wstring path;
    if (focused >= 0 && ListView_GetItemState(list_->Hwnd(), focused, LVIS_SELECTED)) {
        path = model_->FullPath(static_cast<std::size_t>(focused));
    }
    preview_.Show(path);
}

void MainWindow::SetStatusText(int part, const std::wstring& text) {
    if (status_) SendMessageW(status_, SB_SETTEXTW, static_cast<WPARAM>(part), reinterpret_cast<LPARAM>(text.c_str()));
}

// ---- Viewer -----------------------------------------------------------------

UINT MainWindow::ScreenSide() const {
    // Decode for the monitor, not the window, so resizing never re-decodes.
    MONITORINFO info = {sizeof(info)};
    if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &info)) {
        return static_cast<UINT>(std::max(info.rcMonitor.right - info.rcMonitor.left,
                                          info.rcMonitor.bottom - info.rcMonitor.top));
    }
    return 1920;
}

int MainWindow::ViewerIndex() const {
    const std::size_t count = model_->Count();
    for (std::size_t i = 0; i < count; ++i) {
        if (model_->At(i).id == viewerItemId_) return static_cast<int>(i);
    }
    return -1;
}

void MainWindow::EnterViewer(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= model_->Count()) return;
    if (mode_ != Mode::Viewer) {
        mode_ = Mode::Viewer;
        preview_.Show(std::wstring());
        ShowWindow(pathBar_, SW_HIDE);
        ShowWindow(tree_.Hwnd(), SW_HIDE);
        ShowWindow(preview_.Hwnd(), SW_HIDE);
        ShowWindow(list_->Hwnd(), SW_HIDE);
        Layout();  // size the viewer before its first paint
        ShowWindow(viewer_.Hwnd(), SW_SHOW);
        SetFocus(viewer_.Hwnd());
    }
    ShowViewerIndex(index, 1);
}

void MainWindow::ExitViewer() {
    if (mode_ != Mode::Viewer) return;
    StopSlideshow();
    const int index = ViewerIndex();
    if (fullscreen_) SetFullscreen(false);
    mode_ = Mode::Browser;
    ShowWindow(viewer_.Hwnd(), SW_HIDE);
    viewer_.Clear();
    viewerCache_->Retain({});
    ShowWindow(pathBar_, SW_SHOW);
    ShowWindow(tree_.Hwnd(), SW_SHOW);
    ShowWindow(preview_.Hwnd(), SW_SHOW);
    ShowWindow(list_->Hwnd(), SW_SHOW);
    Layout();
    // Land on the picture that was being viewed.
    if (index >= 0) list_->SelectOnly(index);
    SetFocus(list_->Hwnd());
    SetWindowTextW(hwnd_, LoadStr(IDS_APP_TITLE).c_str());
    SetStatusText(0, listFolder_);
    UpdateStatus();
}

void MainWindow::ViewerStep(int step) {
    if (mode_ != Mode::Viewer) return;
    const int count = static_cast<int>(model_->Count());
    const int index = ViewerIndex();
    if (count == 0 || index < 0) return;
    const int next = index + step;
    if (next < 0 || next >= count) {
        MessageBeep(MB_OK);  // at either end
        return;
    }
    ShowViewerIndex(next, step > 0 ? 1 : -1);
}

void MainWindow::ShowViewerIndex(int index, int direction) {
    const std::size_t count = model_->Count();
    if (index < 0 || static_cast<std::size_t>(index) >= count) return;
    viewerItemId_ = model_->At(static_cast<std::size_t>(index)).id;
    viewerPath_ = model_->FullPath(static_cast<std::size_t>(index));
    viewerDirection_ = direction;
    const UINT side = ScreenSide();

    ViewerImagePtr image = viewerCache_->Get(viewerPath_, side);
    ThumbnailPtr placeholder;
    if (!image) {
        placeholder = thumbnails_->TryGetCached(list_->MakeThumbnailRequest(index));
        viewerCache_->Request(viewerPath_, side, true);
    }
    viewer_.SetImage(viewerPath_, image, placeholder, viewerCache_->Failed(viewerPath_));

    // Preload the neighbours, the one in the travel direction first, and
    // let everything else go.
    std::vector<std::wstring> keep{viewerPath_};
    for (int offset : {direction, -direction}) {
        const int neighbour = index + offset;
        if (neighbour < 0 || static_cast<std::size_t>(neighbour) >= count) continue;
        const std::wstring path = model_->FullPath(static_cast<std::size_t>(neighbour));
        keep.push_back(path);
        viewerCache_->Request(path, side, false);
    }
    viewerCache_->Retain(keep);

    const auto slash = viewerPath_.find_last_of(L'\\');
    const std::wstring name = slash == std::wstring::npos ? viewerPath_ : viewerPath_.substr(slash + 1);
    SetWindowTextW(hwnd_, (name + L" - " + LoadStr(IDS_APP_TITLE)).c_str());
    UpdateViewerStatus();
}

void MainWindow::RefreshViewerImage() {
    if (mode_ != Mode::Viewer || viewerPath_.empty()) return;
    // Prefer the sharpest decode we have for the current picture.
    ViewerImagePtr image = viewerCache_->GetAny(viewerPath_);
    if (image) {
        viewer_.SetImage(viewerPath_, image, nullptr, false);
    } else if (viewerCache_->Failed(viewerPath_)) {
        viewer_.SetImage(viewerPath_, nullptr, nullptr, true);
    }
    UpdateViewerStatus();
}

void MainWindow::UpdateViewerStatus() {
    if (mode_ != Mode::Viewer || !status_) return;
    const int index = ViewerIndex();
    const auto slash = viewerPath_.find_last_of(L'\\');
    SetStatusText(0, slash == std::wstring::npos ? viewerPath_ : viewerPath_.substr(slash + 1));
    std::wstring position = LoadStr(IDS_VIEWER_INDEX);
    const auto first = position.find(L"%u");
    if (first != std::wstring::npos) position.replace(first, 2, std::to_wstring(index + 1));
    position = Format(position, model_->Count());
    SetStatusText(1, position);
    if (viewer_.HasImage()) {
        SetStatusText(2, std::to_wstring(viewer_.ImageWidth()) + L" × " + std::to_wstring(viewer_.ImageHeight()));
        const auto percent = static_cast<std::size_t>(viewer_.EffectiveScale() * 100.0 + 0.5);
        SetStatusText(3, Format(LoadStr(IDS_VIEWER_ZOOM), percent));
    } else {
        SetStatusText(2, LoadStr(IDS_VIEWER_LOADING));
        SetStatusText(3, std::wstring());
    }
}

void MainWindow::SetFullscreen(bool fullscreen) {
    if (fullscreen == fullscreen_) return;
    if (fullscreen) {
        windowedPlacement_.length = sizeof(windowedPlacement_);
        GetWindowPlacement(hwnd_, &windowedPlacement_);
        windowedStyle_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        MONITORINFO info = {sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &info);
        fullscreen_ = true;
        SetMenu(hwnd_, nullptr);
        ShowWindow(toolbar_, SW_HIDE);
        ShowWindow(status_, SW_HIDE);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (windowedStyle_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd_, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        fullscreen_ = false;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, windowedStyle_);
        SetMenu(hwnd_, menu_);
        ShowWindow(toolbar_, SW_SHOW);
        ShowWindow(status_, SW_SHOW);
        SetWindowPlacement(hwnd_, &windowedPlacement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    viewer_.SetFullscreen(fullscreen_);
    OnSize();
}

void MainWindow::ToggleSlideshow() {
    if (slideshow_) {
        StopSlideshow();
        return;
    }
    if (mode_ == Mode::Browser) EnterViewer(list_->FocusedIndex());
    if (mode_ != Mode::Viewer) return;
    slideshow_ = SetTimer(hwnd_, kSlideshowTimer, static_cast<UINT>(slideshowSeconds_) * 1000u, nullptr) != 0;
}

void MainWindow::StopSlideshow() {
    if (!slideshow_) return;
    slideshow_ = false;
    KillTimer(hwnd_, kSlideshowTimer);
}

// ---- File management --------------------------------------------------------

std::wstring MainWindow::PickFolder(UINT titleId) const {
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return result;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    const std::wstring title = LoadStr(titleId);
    if (!title.empty()) dialog->SetTitle(title.c_str());
    IShellItem* current = nullptr;
    if (!listFolder_.empty() &&
        SUCCEEDED(SHCreateItemFromParsingName(listFolder_.c_str(), nullptr, IID_PPV_ARGS(&current)))) {
        dialog->SetFolder(current);
        current->Release();
    }
    IShellItem* picked = nullptr;
    if (SUCCEEDED(dialog->Show(hwnd_)) && SUCCEEDED(dialog->GetResult(&picked))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(picked->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) result = path;
        CoTaskMemFree(path);
        picked->Release();
    }
    dialog->Release();
    return result;
}

std::vector<std::wstring> MainWindow::TargetPaths() const {
    std::vector<std::wstring> paths;
    if (mode_ == Mode::Viewer) {
        if (!viewerPath_.empty()) paths.push_back(viewerPath_);
        return paths;
    }
    for (int index : list_->SelectedIndices()) paths.push_back(model_->FullPath(static_cast<std::size_t>(index)));
    return paths;
}

int MainWindow::FindIndexByPath(const std::wstring& path) const {
    const std::size_t count = model_->Count();
    for (std::size_t i = 0; i < count; ++i) {
        if (SamePath(model_->FullPath(i), path)) return static_cast<int>(i);
    }
    return -1;
}

void MainWindow::ShowError(UINT id, const std::wstring& detail) {
    MessageBoxW(hwnd_, Format(LoadStr(id), detail).c_str(), LoadStr(IDS_APP_TITLE).c_str(), MB_OK | MB_ICONWARNING);
}

void MainWindow::AfterRemovingViewed(int index) {
    // The viewer moves on to the next picture (or the previous one at the end).
    const int count = static_cast<int>(model_->Count());
    int next = index + 1 < count ? index + 1 : index - 1;
    if (next < 0 || next == index) {
        ExitViewer();
        Refresh();
        return;
    }
    viewerPath_ = model_->FullPath(static_cast<std::size_t>(next));
    viewerDirection_ = next > index ? 1 : -1;
    Refresh();  // OnFolderLoaded shows viewerPath_ once the list is reloaded
}

void MainWindow::CopyOrMoveSelection(bool move) {
    StopSlideshow();
    const std::vector<std::wstring> paths = TargetPaths();
    if (paths.empty()) return;
    const std::wstring target = PickFolder(move ? IDS_PICK_MOVE_TARGET : IDS_PICK_COPY_TARGET);
    if (target.empty()) return;
    const int first = mode_ == Mode::Viewer ? ViewerIndex() : (list_->SelectedIndices().empty() ? -1 : list_->SelectedIndices().front());
    const HRESULT hr = FileOps::CopyOrMove(hwnd_, paths, target, move);
    if (FAILED(hr)) {
        ShowError(IDS_ERR_FILE_OP, HexResult(hr));
        return;
    }
    tree_.Refresh(target);
    if (!move) return;
    for (const auto& path : paths) viewerCache_->Invalidate(path);
    if (mode_ == Mode::Viewer) {
        AfterRemovingViewed(first);
    } else {
        selectIndexAfterLoad_ = first;
        Refresh();
    }
}

void MainWindow::DeleteSelection() {
    StopSlideshow();
    if (mode_ == Mode::Browser && GetFocus() == tree_.Hwnd()) return;  // folders are not deleted from here
    const std::vector<std::wstring> paths = TargetPaths();
    if (paths.empty()) return;
    const int first = mode_ == Mode::Viewer ? ViewerIndex() : list_->SelectedIndices().front();
    const HRESULT hr = FileOps::Recycle(hwnd_, paths);
    if (hr == S_FALSE) return;  // cancelled
    if (FAILED(hr)) {
        ShowError(IDS_ERR_FILE_OP, HexResult(hr));
        return;
    }
    for (const auto& path : paths) viewerCache_->Invalidate(path);
    if (mode_ == Mode::Viewer) {
        AfterRemovingViewed(first);
    } else {
        selectIndexAfterLoad_ = first;
        Refresh();
    }
}

std::wstring MainWindow::UndoFilePath() {
    if (undoFile_.empty()) {
        Settings settings;
        if (SUCCEEDED(Settings::ForCurrentProcess(settings))) {
            const std::wstring& ini = settings.FilePath();
            const auto slash = ini.find_last_of(L"\\/");
            if (slash != std::wstring::npos) undoFile_ = ini.substr(0, slash + 1) + L"rename-undo.txt";
        }
    }
    return undoFile_;
}

void MainWindow::BatchRenameSelection() {
    if (mode_ != Mode::Browser) return;
    StopSlideshow();
    std::vector<int> indices = list_->SelectedIndices();  // ascending = list order
    if (indices.size() <= 1) {
        // One file or none: rename the whole folder.
        indices.clear();
        for (std::size_t i = 0; i < model_->Count(); ++i) indices.push_back(static_cast<int>(i));
    }
    if (indices.empty()) return;
    std::vector<std::wstring> paths;
    paths.reserve(indices.size());
    for (int index : indices) paths.push_back(model_->FullPath(static_cast<std::size_t>(index)));
    const HRESULT hr = ShowBatchRenameDialog(hwnd_, instance_, paths, UndoFilePath());
    if (hr == S_FALSE) return;
    if (FAILED(hr)) ShowError(IDS_ERR_BATCH_RENAME, HexResult(hr));
    selectIndexAfterLoad_ = indices.front();
    Refresh();
}

void MainWindow::UndoBatchRename() {
    if (mode_ != Mode::Browser) return;
    const std::wstring undo = UndoFilePath();
    if (!BatchRename::CanUndo(undo)) {
        MessageBoxW(hwnd_, LoadStr(IDS_BR_NOTHING_TO_UNDO).c_str(), LoadStr(IDS_APP_TITLE).c_str(), MB_OK | MB_ICONINFORMATION);
        return;
    }
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const HRESULT hr = BatchRename::UndoLast(undo);
    SetCursor(old);
    if (FAILED(hr)) ShowError(IDS_ERR_UNDO_RENAME, HexResult(hr));
    Refresh();
}

void MainWindow::RenameSelection() {
    StopSlideshow();
    if (mode_ == Mode::Browser && GetFocus() == tree_.Hwnd()) {
        if (HTREEITEM item = TreeView_GetSelection(tree_.Hwnd())) TreeView_EditLabel(tree_.Hwnd(), item);
        return;
    }
    int index = mode_ == Mode::Viewer ? ViewerIndex() : list_->FocusedIndex();
    if (index < 0) return;
    if (mode_ == Mode::Viewer) ExitViewer();  // rename happens in the list
    list_->BeginRename(index);
}

bool MainWindow::RenameItem(int index, const std::wstring& newName) {
    if (index < 0 || static_cast<std::size_t>(index) >= model_->Count()) return false;
    std::wstring name = newName;
    while (!name.empty() && name.front() == L' ') name.erase(name.begin());
    const std::wstring& oldName = model_->At(static_cast<std::size_t>(index)).name;
    if (name == oldName) return false;
    // Typing over the whole name drops the extension; keep it so the
    // picture stays a picture.
    const auto oldDot = oldName.find_last_of(L'.');
    if (name.find(L'.') == std::wstring::npos && oldDot != std::wstring::npos && oldDot > 0) {
        name += oldName.substr(oldDot);
    }
    if (!FileOps::IsValidFileName(name)) {
        MessageBoxW(hwnd_, LoadStr(IDS_ERR_INVALID_NAME).c_str(), LoadStr(IDS_APP_TITLE).c_str(), MB_OK | MB_ICONWARNING);
        return false;
    }
    const std::wstring from = model_->FullPath(static_cast<std::size_t>(index));
    const std::wstring to = (listFolder_.back() == L'\\' ? listFolder_ : listFolder_ + L'\\') + name;
    const HRESULT hr = FileOps::Rename(from, to);
    if (FAILED(hr)) {
        ShowError(IDS_ERR_RENAME, name);
        return false;
    }
    viewerCache_->Invalidate(from);
    Navigate(listFolder_, name);  // reload and keep the renamed file selected
    return true;
}

void MainWindow::NewFolder() {
    if (listFolder_.empty()) return;
    if (mode_ == Mode::Viewer) ExitViewer();
    const std::wstring name = FileOps::UniqueChildName(listFolder_, LoadStr(IDS_NEW_FOLDER));
    const std::wstring path = (listFolder_.back() == L'\\' ? listFolder_ : listFolder_ + L'\\') + name;
    if (!CreateDirectoryW(path.c_str(), nullptr)) {
        ShowError(IDS_ERR_FILE_OP, name);
        return;
    }
    // Show it in the folder tree with its name ready to edit.
    tree_.EditChildWhenListed(listFolder_, name);
}

void MainWindow::RotateSelection(bool clockwise) {
    StopSlideshow();
    std::vector<int> indices;
    if (mode_ == Mode::Viewer) {
        const int index = ViewerIndex();
        if (index >= 0) indices.push_back(index);
        viewer_.RotatePending(clockwise);  // turn on screen right away
    } else {
        indices = list_->SelectedIndices();
    }
    for (int index : indices) {
        RotateState::Result job{model_->FullPath(static_cast<std::size_t>(index)), list_->MakeThumbnailRequest(index), E_PENDING};
        const auto state = rotateState_;
        const HRESULT submitted = rotatePool_->Submit([state, job, clockwise]() mutable {
            thread_local WorkerCom com;
            job.result = Rotator::RotateFile(job.path, clockwise);
            std::lock_guard<std::mutex> lock(state->mutex);
            state->done.push_back(std::move(job));
            if (state->hwnd) PostMessageW(state->hwnd, WM_APP_ROTATED, 0, 0);
        });
        if (SUCCEEDED(submitted)) ++rotationsPending_;
    }
    if (rotationsPending_ > 0 && mode_ == Mode::Browser) {
        SetStatusText(0, Format(LoadStr(IDS_STATUS_ROTATING), rotationsPending_));
    }
}

void MainWindow::OnRotated() {
    std::vector<RotateState::Result> done;
    {
        std::lock_guard<std::mutex> lock(rotateState_->mutex);
        done.swap(rotateState_->done);
    }
    for (const auto& job : done) {
        if (rotationsPending_ > 0) --rotationsPending_;
        if (FAILED(job.result)) {
            ++rotationFailures_;
            continue;
        }
        thumbnails_->Invalidate(job.thumbnail);
        viewerCache_->Invalidate(job.path);
        if (mode_ == Mode::Viewer && SamePath(job.path, viewerPath_)) {
            viewerCache_->Request(viewerPath_, ScreenSide(), true);
        }
        if (SamePath(job.path, viewerPath_) || mode_ == Mode::Browser) {
            // The preview pane shows the rotated picture after the reload.
            preview_.Show(std::wstring());
        }
    }
    if (mode_ == Mode::Browser && rotationsPending_ > 0) {
        SetStatusText(0, Format(LoadStr(IDS_STATUS_ROTATING), rotationsPending_));
    }
    if (rotationsPending_ == 0) {
        if (rotationFailures_ > 0) {
            MessageBoxW(hwnd_, Format(LoadStr(IDS_ERR_ROTATE), rotationFailures_).c_str(), LoadStr(IDS_APP_TITLE).c_str(),
                        MB_OK | MB_ICONWARNING);
            rotationFailures_ = 0;
            if (mode_ == Mode::Viewer) viewer_.SetImage(viewerPath_, viewerCache_->GetAny(viewerPath_), nullptr, false);
        }
        // Sizes (and thumbnails) changed: reload, keeping the selection.
        Refresh();
    }
}

void MainWindow::ShowProperties() {
    const int index = mode_ == Mode::Viewer ? ViewerIndex() : list_->FocusedIndex();
    if (index < 0) return;
    ShowPropertiesDialog(hwnd_, instance_, model_->FullPath(static_cast<std::size_t>(index)));
}

void MainWindow::ShowContextMenu(POINT screen) {
    if (mode_ == Mode::Browser) {
        if (list_->SelectedIndices().empty()) return;
        if (screen.x == -1 && screen.y == -1) {
            // Keyboard (Shift+F10): open at the focused item.
            RECT rc{};
            const int focused = list_->FocusedIndex();
            if (focused >= 0 && ListView_GetItemRect(list_->Hwnd(), focused, &rc, LVIR_ICON)) {
                screen = POINT{rc.left + (rc.right - rc.left) / 2, rc.top + (rc.bottom - rc.top) / 2};
                ClientToScreen(list_->Hwnd(), &screen);
            } else {
                GetCursorPos(&screen);
            }
        }
    } else if (screen.x == -1 && screen.y == -1) {
        GetCursorPos(&screen);
    }
    HMENU menu = LoadMenuW(instance_, MAKEINTRESOURCEW(IDR_CONTEXTMENU));
    if (!menu) return;
    HMENU popup = GetSubMenu(menu, 0);
    if (mode_ == Mode::Browser) SetMenuDefaultItem(popup, IDM_NAV_TOGGLE_MODE, FALSE);
    if (mode_ == Mode::Viewer) {
        DeleteMenu(popup, IDM_NAV_TOGGLE_MODE, MF_BYCOMMAND);
        DeleteMenu(popup, 0, MF_BYPOSITION);  // the separator that followed it
    }
    const UINT command = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (command) OnCommand(command);
}
