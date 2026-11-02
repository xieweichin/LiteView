#include "ui/MainWindow.h"

#include <commctrl.h>
#include <knownfolders.h>
#include <shellapi.h>
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
#include "core/FormatSupport.h"
#include "core/PdfRenderer.h"
#include "core/Rotator.h"
#include "core/Settings.h"
#include "core/ThreadPool.h"
#include "res/resource.h"
#include "ui/Commands.h"
#include "ui/BatchRenameDialog.h"
#include "ui/Messages.h"
#include "ui/PropertiesDialog.h"
#include "ui/Theme.h"

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
constexpr int kGap = 6;             // 96-DPI pixels
constexpr int kFieldHeight = 32;    // 96-DPI pixels, path field
constexpr int kFieldPad = 10;       // 96-DPI pixels, text inset in the path field
constexpr int kRadius = 8;          // 96-DPI pixels, RoundRect ellipse (4 px corners)
constexpr int kStatusPad = 12;      // 96-DPI pixels

// Segoe Fluent Icons / Segoe MDL2 Assets code points.
constexpr wchar_t kGlyphUp = 0xE74A;
constexpr wchar_t kGlyphRefresh = 0xE72C;
constexpr wchar_t kGlyphPhoto = 0xE91B;
constexpr wchar_t kGlyphBack = 0xE72B;
constexpr wchar_t kGlyphPrev = 0xE76B;
constexpr wchar_t kGlyphNext = 0xE76C;
constexpr wchar_t kGlyphZoomOut = 0xE71F;
constexpr wchar_t kGlyphZoomIn = 0xE8A3;
constexpr wchar_t kGlyphFit = 0xE9A6;
constexpr wchar_t kGlyphRotateLeft = 0xE80D;
constexpr wchar_t kGlyphRotateRight = 0xE80C;
constexpr wchar_t kGlyphRename = 0xE8AC;
constexpr wchar_t kGlyphDelete = 0xE74D;
constexpr wchar_t kGlyphInfo = 0xE946;
constexpr wchar_t kGlyphSort = 0xE8CB;
constexpr wchar_t kGlyphView = 0xE8A9;
constexpr wchar_t kGlyphMore = 0xE712;
constexpr wchar_t kGlyphSlideshow = 0xE786;
constexpr wchar_t kGlyphFullscreen = 0xE740;
constexpr UINT kSettingsWaitMs = 200;

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

// The submenu of `root` (at any depth) that directly holds `command`.
HMENU FindSubMenu(HMENU root, UINT command) {
    const int count = GetMenuItemCount(root);
    for (int i = 0; i < count; ++i) {
        HMENU sub = GetSubMenu(root, i);
        if (!sub) continue;
        const int subCount = GetMenuItemCount(sub);
        for (int j = 0; j < subCount; ++j) {
            if (GetMenuItemID(sub, j) == command) return sub;
        }
        if (HMENU deeper = FindSubMenu(sub, command)) return deeper;
    }
    return nullptr;
}

// The "&X" mnemonic of a menu item, upper-cased, or 0.
wchar_t Mnemonic(const std::wstring& text) {
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] != L'&') continue;
        if (text[i + 1] == L'&') { ++i; continue; }
        return static_cast<wchar_t>(reinterpret_cast<UINT_PTR>(CharUpperW(
            reinterpret_cast<LPWSTR>(static_cast<UINT_PTR>(text[i + 1])))));
    }
    return 0;
}

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
    wc.hbrBackground = nullptr;  // painted in OnPaint with the theme colours
    wc.lpszMenuName = nullptr;   // the menu opens from the command bar
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
    // The read-only text page keeps its reading keys; the rest of the viewer
    // shortcuts (Esc, Enter, F2, zoom, Ctrl+F...) still apply there.
    if (mode_ == Mode::Viewer && viewerIsText_ && textView_.EditHwnd() && GetFocus() == textView_.EditHwnd()) {
        if (msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN) {
            switch (msg->wParam) {
            case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END: case VK_SPACE:
            case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_BACK: case VK_DELETE:
                return false;
            default:
                break;
            }
        }
        return accelViewer_ && ::TranslateAcceleratorW(hwnd_, accelViewer_, msg) != 0;
    }
    // The video page uses Space to play and pause.
    if (mode_ == Mode::Viewer && viewerPageKind_ == ViewerPage::Video && videoView_.Owns(GetFocus()) &&
        msg->message == WM_KEYDOWN && msg->wParam == VK_SPACE) {
        return false;
    }
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
        if (reinterpret_cast<HWND>(lParam) == pathBar_ && pathBar_ &&
            (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS)) {
            InvalidateRect(hwnd_, &fieldRect_, FALSE);  // focus underline
        }
        OnCommand(LOWORD(wParam));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        OnPaint();
        return 0;
    case WM_CTLCOLOREDIT: {
        LRESULT brush = 0;
        if (theme::HandleCtlColor(msg, wParam, lParam, brush)) return brush;
        break;
    }
    case WM_SYSCOMMAND:
        // Alt, F10 and Alt+letter open the menu that used to be the menu bar.
        if ((wParam & 0xFFF0) == SC_KEYMENU && OnKeyMenu(static_cast<wchar_t>(lParam))) return 0;
        break;
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
        if (theme::IsThemeChangeMessage(msg, lParam)) OnThemeChanged();
        break;
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
        if (reinterpret_cast<HWND>(wParam) == list_->Hwnd() || reinterpret_cast<HWND>(wParam) == viewer_.Hwnd() ||
            reinterpret_cast<HWND>(wParam) == textView_.Hwnd() || reinterpret_cast<HWND>(wParam) == documentView_.Hwnd() ||
            reinterpret_cast<HWND>(wParam) == modelView_.Hwnd() || videoView_.Owns(reinterpret_cast<HWND>(wParam))) {
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
            SetFocus(ViewerPageHwnd(viewerPageKind_));
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
        if (moreMenu_) DestroyMenu(moreMenu_);
        if (viewMenu_) DestroyMenu(viewMenu_);
        moreMenu_ = viewMenu_ = sortMenu_ = nullptr;
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
    theme::ApplyFrame(hwnd_);
    if (FAILED(navBar_.Create(hwnd_, instance_)) || FAILED(commandBar_.Create(hwnd_, instance_))) return false;
    navBar_.SetDpi(dpi_, font_);
    commandBar_.SetDpi(dpi_, font_);
    navBar_.SetItems({
        {CommandBar::Kind::Button, IDM_NAV_PARENT_FOLDER, kGlyphUp, 0},
        {CommandBar::Kind::Button, IDM_VIEW_REFRESH, kGlyphRefresh, 0},
    });
    CreatePathBar();
    if (!pathBar_) return false;
    statusText_[0] = LoadStr(IDS_STATUS_READY);
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
        if (!viewerKey_.empty()) viewerCache_->Request(viewerKey_, 0, true);
    };
    callbacks.viewChanged = [this] { UpdateViewerStatus(); };
    if (FAILED(viewer_.Create(hwnd_, instance_, std::move(callbacks)))) return false;
    if (FAILED(textView_.Create(hwnd_, instance_, [this] { UpdateViewerStatus(); }))) return false;
    {
        DocumentView::Callbacks documentCallbacks;
        documentCallbacks.translateAccelerator = [this](MSG* msg) {
            return mode_ == Mode::Viewer && accelViewer_ && ::TranslateAcceleratorW(hwnd_, accelViewer_, msg) != 0;
        };
        documentCallbacks.changed = [this] { UpdateViewerStatus(); };
        if (FAILED(documentView_.Create(hwnd_, instance_, accelViewer_, std::move(documentCallbacks)))) return false;
    }
    {
        ModelView::Callbacks modelCallbacks;
        modelCallbacks.changed = [this] { UpdateViewerStatus(); };
        modelCallbacks.navigate = [this](int step) { ViewerStep(step); };
        modelCallbacks.exit = [this] { ExitViewer(); };
        if (FAILED(modelView_.Create(hwnd_, instance_, std::move(modelCallbacks)))) return false;
    }
    {
        VideoView::Callbacks videoCallbacks;
        videoCallbacks.changed = [this] { UpdateViewerStatus(); };
        videoCallbacks.navigate = [this](int step) { ViewerStep(step); };
        videoCallbacks.exit = [this] { ExitViewer(); };
        if (FAILED(videoView_.Create(hwnd_, instance_, std::move(videoCallbacks)))) return false;
    }
    CreateMenus();
    SetBarItems();
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
    pathBar_ = CreateWindowExW(0, WC_EDITW, nullptr,
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

void MainWindow::CreateMenus() {
    // The old menu bar becomes one popup: "File", "Edit", ... as submenus,
    // so their Alt+letter mnemonics keep working.
    HMENU bar = LoadMenuW(instance_, MAKEINTRESOURCEW(IDR_MAINMENU));
    moreMenu_ = CreatePopupMenu();
    if (!bar || !moreMenu_) {
        if (bar) DestroyMenu(bar);
        return;
    }
    while (GetMenuItemCount(bar) > 0) {
        wchar_t text[64] = {};
        GetMenuStringW(bar, 0, text, ARRAYSIZE(text), MF_BYPOSITION);
        HMENU sub = GetSubMenu(bar, 0);
        RemoveMenu(bar, 0, MF_BYPOSITION);
        if (sub) AppendMenuW(moreMenu_, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(sub), text);
    }
    DestroyMenu(bar);
    sortMenu_ = FindSubMenu(moreMenu_, IDM_SORT_NAME);
    viewMenu_ = CreatePopupMenu();
    if (viewMenu_) {
        for (UINT id = IDM_VIEW_THUMBNAILS; id <= IDM_VIEW_DETAILS; ++id) {
            wchar_t text[64] = {};
            GetMenuStringW(moreMenu_, id, text, ARRAYSIZE(text), MF_BYCOMMAND);
            AppendMenuW(viewMenu_, MF_STRING, id, text);
        }
        wchar_t text[64] = {};
        GetMenuStringW(moreMenu_, IDM_VIEW_PREVIEW_PANE, text, ARRAYSIZE(text), MF_BYCOMMAND);
        AppendMenuW(viewMenu_, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(viewMenu_, MF_STRING, IDM_VIEW_PREVIEW_PANE, text);
    }
}

void MainWindow::SetBarItems() {
    using Kind = CommandBar::Kind;
    const CommandBar::Item separator{Kind::Separator, 0, 0, 0};
    if (mode_ == Mode::Viewer) {
        commandBar_.SetItems({
            {Kind::Button, IDM_NAV_EXIT_VIEWER, kGlyphBack, IDS_BAR_BROWSE},
            separator,
            {Kind::Button, IDM_NAV_PREV, kGlyphPrev, 0},
            {Kind::Button, IDM_NAV_NEXT, kGlyphNext, 0},
            separator,
            {Kind::Button, IDM_ZOOM_OUT, kGlyphZoomOut, 0},
            {Kind::Button, IDM_ZOOM_IN, kGlyphZoomIn, 0},
            {Kind::Button, IDM_ZOOM_TOGGLE_FIT, kGlyphFit, 0},
            separator,
            {Kind::Button, IDM_ROTATE_LEFT, kGlyphRotateLeft, 0},
            {Kind::Button, IDM_ROTATE_RIGHT, kGlyphRotateRight, 0},
            separator,
            {Kind::Button, IDM_EDIT_RENAME, kGlyphRename, 0},
            {Kind::Button, IDM_EDIT_DELETE, kGlyphDelete, 0},
            {Kind::Button, IDM_FILE_PROPERTIES, kGlyphInfo, 0},
            {Kind::Spacer, 0, 0, 0},
            {Kind::Button, IDM_VIEW_SLIDESHOW, kGlyphSlideshow, 0},
            {Kind::Button, IDM_VIEW_FULLSCREEN, kGlyphFullscreen, 0},
            {Kind::Button, IDM_BAR_MORE, kGlyphMore, 0},
        });
        commandBar_.SetChecked(IDM_VIEW_SLIDESHOW, slideshow_);
    } else {
        commandBar_.SetItems({
            {Kind::Button, IDM_NAV_TOGGLE_MODE, kGlyphPhoto, IDS_BAR_VIEW_PICTURE},
            separator,
            {Kind::Button, IDM_ROTATE_LEFT, kGlyphRotateLeft, 0},
            {Kind::Button, IDM_ROTATE_RIGHT, kGlyphRotateRight, 0},
            separator,
            {Kind::Button, IDM_EDIT_RENAME, kGlyphRename, 0},
            {Kind::Button, IDM_EDIT_DELETE, kGlyphDelete, 0},
            {Kind::Button, IDM_FILE_PROPERTIES, kGlyphInfo, 0},
            {Kind::Spacer, 0, 0, 0},
            {Kind::DropDown, IDM_BAR_SORT, kGlyphSort, IDS_BAR_SORT},
            {Kind::DropDown, IDM_BAR_VIEW, kGlyphView, IDS_BAR_VIEW},
            {Kind::Button, IDM_BAR_MORE, kGlyphMore, 0},
        });
    }
}

void MainWindow::ShowBarMenu(UINT command, bool fromKeyboard) {
    HMENU menu = command == IDM_BAR_SORT ? sortMenu_ : command == IDM_BAR_VIEW ? viewMenu_ : moreMenu_;
    if (!menu) return;
    RECT rc{};
    if (!commandBar_.ItemScreenRect(command, &rc)) {
        GetWindowRect(commandBar_.Hwnd(), &rc);
        rc.left = rc.right;
    }
    TPMPARAMS params = {sizeof(params), rc};
    const bool right = command == IDM_BAR_MORE;
    if (fromKeyboard) PostMessageW(hwnd_, WM_KEYDOWN, VK_DOWN, 0);  // highlight the first item
    TrackPopupMenuEx(menu, TPM_LEFTBUTTON | TPM_VERTICAL | (right ? TPM_RIGHTALIGN : TPM_LEFTALIGN),
                     right ? rc.right : rc.left, rc.bottom, hwnd_, &params);
}

bool MainWindow::OnKeyMenu(wchar_t key) {
    if (fullscreen_ || !moreMenu_) return false;  // no command bar to anchor to
    if (key == 0) {  // Alt or F10 alone
        ShowBarMenu(IDM_BAR_MORE, true);
        return true;
    }
    const wchar_t upper = static_cast<wchar_t>(reinterpret_cast<UINT_PTR>(
        CharUpperW(reinterpret_cast<LPWSTR>(static_cast<UINT_PTR>(key)))));
    const int count = GetMenuItemCount(moreMenu_);
    for (int i = 0; i < count; ++i) {
        wchar_t text[64] = {};
        GetMenuStringW(moreMenu_, static_cast<UINT>(i), text, ARRAYSIZE(text), MF_BYPOSITION);
        if (Mnemonic(text) == upper) {
            // The menu's own mnemonic handling opens the matching submenu.
            PostMessageW(hwnd_, WM_CHAR, key, 0);
            ShowBarMenu(IDM_BAR_MORE, false);
            return true;
        }
    }
    return false;  // Alt+Space and friends: the system menu
}

void MainWindow::OnThemeChanged() {
    theme::Refresh();
    theme::ApplyFrame(hwnd_);
    if (tree_.Hwnd()) theme::ApplyTree(tree_.Hwnd());
    if (list_ && list_->Hwnd()) theme::ApplyListView(list_->Hwnd());
    navBar_.OnThemeChanged();
    commandBar_.OnThemeChanged();
    textView_.OnThemeChanged();
    documentView_.OnThemeChanged();
    modelView_.OnThemeChanged();
    videoView_.OnThemeChanged();
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

int MainWindow::TextHeight() const {
    TEXTMETRICW tm{};
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_ ? static_cast<HGDIOBJ>(font_) : GetStockObject(DEFAULT_GUI_FONT));
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    return tm.tmHeight;
}

void MainWindow::OnPaint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right > 0 && rc.bottom > 0) {
        // Children are clipped out, so only the chrome around them is drawn.
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP canvas = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(memory, canvas);
        PaintChrome(memory, rc);
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, memory, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(memory, old);
        DeleteObject(canvas);
        DeleteDC(memory);
    }
    EndPaint(hwnd_, &ps);
}

void MainWindow::PaintChrome(HDC dc, const RECT& rc) {
    const theme::Palette& p = theme::Current();
    FillRect(dc, &rc, theme::WindowBrush());
    if (fullscreen_) return;
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, font_ ? static_cast<HGDIOBJ>(font_) : GetStockObject(DEFAULT_GUI_FONT));

    if (mode_ == Mode::Browser && !IsRectEmpty(&fieldRect_)) {
        // Rounded path field with a hairline border; accent underline while
        // it has the keyboard focus, like the Windows 11 text box.
        HBRUSH fill = CreateSolidBrush(p.field);
        HPEN pen = CreatePen(PS_SOLID, 1, p.border);
        HGDIOBJ oldBrush = SelectObject(dc, fill);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        RoundRect(dc, fieldRect_.left, fieldRect_.top, fieldRect_.right, fieldRect_.bottom, Scale(kRadius, dpi_),
                  Scale(kRadius, dpi_));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        DeleteObject(fill);
        if (GetFocus() == pathBar_) {
            const int r = Scale(kRadius, dpi_) / 2;
            RECT line{fieldRect_.left + r, fieldRect_.bottom - Scale(2, dpi_), fieldRect_.right - r, fieldRect_.bottom};
            HBRUSH accent = CreateSolidBrush(p.accent);
            FillRect(dc, &line, accent);
            DeleteObject(accent);
        }
    }

    // Hairline between the bars and the content.
    HBRUSH border = CreateSolidBrush(p.border);
    RECT line{0, content_.top - 1, rc.right, content_.top};
    FillRect(dc, &line, border);
    DeleteObject(border);

    // Status line: message on the left, the other parts right-aligned.
    if (!IsRectEmpty(&statusRect_)) {
        SetTextColor(dc, p.text);
        const int pad = Scale(kStatusPad, dpi_);
        int right = statusRect_.right - pad;
        for (int part = 3; part >= 1; --part) {
            const std::wstring& text = statusText_[part];
            if (text.empty()) continue;
            SIZE size{};
            GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
            RECT r{right - size.cx, statusRect_.top, right, statusRect_.bottom};
            DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            right -= size.cx + 2 * pad;
        }
        RECT r{statusRect_.left + pad, statusRect_.top, std::max(static_cast<int>(statusRect_.left) + pad, right), statusRect_.bottom};
        DrawTextW(dc, statusText_[0].c_str(), static_cast<int>(statusText_[0].size()), &r,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS);
    }
    SelectObject(dc, oldFont);
}

void MainWindow::OnSize() { Layout(); }

void MainWindow::Layout() {
    if (!hwnd_ || !pathBar_ || !list_ || !list_->Hwnd()) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int gap = Scale(kGap, dpi_);
    const int splitter = Scale(kSplitter, dpi_);
    const int textHeight = TextHeight();
    const bool chrome = !fullscreen_;

    int top = 0;
    if (chrome && mode_ == Mode::Browser) {
        // Address row: up / refresh, then the rounded path field.
        const int rowHeight = navBar_.Height();
        const int navWidth = navBar_.ContentWidth();
        SetWindowPos(navBar_.Hwnd(), nullptr, 0, 0, navWidth, rowHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        const int fieldHeight = Scale(kFieldHeight, dpi_);
        fieldRect_ = RECT{navWidth, (rowHeight - fieldHeight) / 2, std::max<LONG>(navWidth, rc.right - gap),
                          (rowHeight - fieldHeight) / 2 + fieldHeight};
        const int pad = Scale(kFieldPad, dpi_);
        SetWindowPos(pathBar_, nullptr, fieldRect_.left + pad, fieldRect_.top + (fieldHeight - textHeight) / 2,
                     std::max(0, static_cast<int>(fieldRect_.right - fieldRect_.left) - 2 * pad), textHeight,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        top = rowHeight;
    } else {
        fieldRect_ = RECT{};
    }
    if (chrome) {
        const int barHeight = commandBar_.Height();
        SetWindowPos(commandBar_.Hwnd(), nullptr, 0, top, rc.right, barHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        top += barHeight + 1;  // hairline
        const int statusHeight = textHeight + Scale(kStatusPad, dpi_);
        statusRect_ = RECT{0, rc.bottom - statusHeight, rc.right, rc.bottom};
    } else {
        statusRect_ = RECT{};
    }
    const int bottom = chrome ? statusRect_.top : rc.bottom;

    if (mode_ == Mode::Viewer) {
        // The picture takes everything between command bar and status line
        // (the whole client area in full screen).
        content_ = RECT{0, top, rc.right, bottom};
        SetWindowPos(viewer_.Hwnd(), nullptr, 0, top, rc.right, bottom - top, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(textView_.Hwnd(), nullptr, 0, top, rc.right, bottom - top, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(documentView_.Hwnd(), nullptr, 0, top, rc.right, bottom - top, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(modelView_.Hwnd(), nullptr, 0, top, rc.right, bottom - top, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(videoView_.Hwnd(), nullptr, 0, top, rc.right, bottom - top, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    HDWP dwp = BeginDeferWindowPos(4);
    auto place = [&](HWND child, int x, int y, int w, int h) {
        if (dwp) dwp = DeferWindowPos(dwp, child, nullptr, x, y, std::max(w, 0), std::max(h, 0),
                                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    };
    content_ = RECT{0, top, rc.right, bottom};
    const int width = content_.right - content_.left;
    const int height = content_.bottom - content_.top;
    const int minPane = Scale(kMinPane, dpi_);
    // Folder tree on the left, file list in the middle, preview on the right
    // (like Explorer's preview pane). The list keeps at least minPane.
    int left = Scale(leftWidth96_, dpi_);
    // Leave room for the list and, when shown, the preview at their minimum widths.
    left = std::min(left, showPreview_ ? width - 2 * splitter - 2 * minPane : width - splitter - minPane);
    left = std::max(left, std::min(minPane, width / 3));
    int preview = 0;
    if (showPreview_) {
        preview = Scale(previewWidth96_, dpi_);
        preview = std::min(preview, width - left - 2 * splitter - minPane);
        preview = std::max(preview, std::min(minPane, (width - left) / 2));
    }
    const int listWidth = width - left - splitter - (showPreview_ ? preview + splitter : 0);

    place(tree_.Hwnd(), 0, content_.top, left, height);
    place(list_->Hwnd(), left + splitter, content_.top, listWidth, height);
    if (showPreview_) place(preview_.Hwnd(), width - preview, content_.top, preview, height);
    if (dwp) EndDeferWindowPos(dwp);
    InvalidateRect(hwnd_, nullptr, FALSE);  // field, hairline and status line
}

MainWindow::Splitter MainWindow::HitTestSplitter(POINT pt) const {
    if (mode_ != Mode::Browser || !PtInRect(&content_, pt)) return Splitter::None;
    const int splitter = Scale(kSplitter, dpi_);
    RECT treeRect{};
    GetWindowRect(tree_.Hwnd(), &treeRect);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&treeRect), 2);
    if (pt.x >= treeRect.right && pt.x < treeRect.right + splitter) return Splitter::Left;
    if (showPreview_) {
        RECT previewRect{};
        GetWindowRect(preview_.Hwnd(), &previewRect);
        MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&previewRect), 2);
        if (pt.x >= previewRect.left - splitter && pt.x < previewRect.left) return Splitter::Right;
    }
    return Splitter::None;
}

bool MainWindow::OnSetCursor() {
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(hwnd_, &pt);
    const Splitter hit = dragging_ != Splitter::None ? dragging_ : HitTestSplitter(pt);
    if (hit == Splitter::None) {
        if (!StatusHintHit(pt)) return false;
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return true;
    }
    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
    return true;
}

void MainWindow::OnLButtonDown(POINT pt) {
    if (StatusHintHit(pt)) {
        OpenCodecStore(codecHintPacks_);
        return;
    }
    dragging_ = HitTestSplitter(pt);
    if (dragging_ == Splitter::None) return;
    RECT rect{};
    GetWindowRect(dragging_ == Splitter::Left ? tree_.Hwnd() : preview_.Hwnd(), &rect);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&rect), 2);
    dragOffset_ = dragging_ == Splitter::Left ? pt.x - rect.right : pt.x - rect.left;
    SetCapture(hwnd_);
}

void MainWindow::OnMouseMove(POINT pt) {
    if (dragging_ == Splitter::None || GetCapture() != hwnd_) return;
    if (dragging_ == Splitter::Left) {
        leftWidth96_ = MulDiv(std::max(0, static_cast<int>(pt.x) - dragOffset_), USER_DEFAULT_SCREEN_DPI,
                              static_cast<int>(dpi_));
    } else {
        previewWidth96_ = MulDiv(std::max(0, static_cast<int>(content_.right - pt.x) + dragOffset_),
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
    if (showPreview_) {
        RECT previewRect{};
        GetWindowRect(preview_.Hwnd(), &previewRect);
        previewWidth96_ = MulDiv(previewRect.right - previewRect.left, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi_));
    }
}

void MainWindow::SetPreviewVisible(bool visible) {
    if (visible == showPreview_) return;
    showPreview_ = visible;
    if (mode_ == Mode::Browser) ShowWindow(preview_.Hwnd(), visible ? SW_SHOW : SW_HIDE);
    Layout();
    UpdatePreview();  // hidden: drops the picture; shown: previews the selection
}

void MainWindow::OnCommand(UINT id) {
    switch (id) {
    case IDM_VIEW_PREVIEW_PANE:
        if (mode_ == Mode::Browser) SetPreviewVisible(!showPreview_);
        break;
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
        if (mode_ == Mode::Viewer && viewerPageKind_ == ViewerPage::Model) modelView_.Turn(-1);
        else RotateSelection(false);
        break;
    case IDM_ROTATE_RIGHT:
        if (mode_ == Mode::Viewer && viewerPageKind_ == ViewerPage::Model) modelView_.Turn(1);
        else RotateSelection(true);
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
        if (mode_ == Mode::Viewer) {
            if (viewerPageKind_ == ViewerPage::Text) textView_.ZoomIn();
            else if (viewerPageKind_ == ViewerPage::Model) modelView_.ZoomIn();
            else if (viewerPageKind_ == ViewerPage::Picture) viewer_.ZoomIn();
        }
        break;
    case IDM_ZOOM_OUT:
        if (mode_ == Mode::Viewer) {
            if (viewerPageKind_ == ViewerPage::Text) textView_.ZoomOut();
            else if (viewerPageKind_ == ViewerPage::Model) modelView_.ZoomOut();
            else if (viewerPageKind_ == ViewerPage::Picture) viewer_.ZoomOut();
        }
        break;
    case IDM_ZOOM_TOGGLE_FIT:
        if (mode_ == Mode::Viewer) {
            if (viewerPageKind_ == ViewerPage::Text) textView_.ResetZoom();
            else if (viewerPageKind_ == ViewerPage::Model) modelView_.ResetView();
            else if (viewerPageKind_ == ViewerPage::Picture) viewer_.ToggleFit();
        }
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
    case IDM_BAR_SORT:
    case IDM_BAR_VIEW:
    case IDM_BAR_MORE:
        ShowBarMenu(id, false);
        break;
    case IDM_PAGE_NEXT:
        StepPdfPage(1);
        break;
    case IDM_PAGE_PREV:
        StepPdfPage(-1);
        break;
    case IDM_HELP_CODECS:
        ShowCodecInfo();
        break;
    case IDM_HELP_ABOUT: {
        const std::wstring text = LoadStr(IDS_ABOUT_TEXT);
        const std::wstring title = LoadStr(IDS_APP_TITLE);
        MSGBOXPARAMSW params{};
        params.cbSize = sizeof(params);
        params.hwndOwner = hwnd_;
        params.hInstance = GetModuleHandleW(nullptr);
        params.lpszText = text.c_str();
        params.lpszCaption = title.c_str();
        params.dwStyle = MB_OK | MB_USERICON;
        params.lpszIcon = MAKEINTRESOURCEW(IDI_APP);
        MessageBoxIndirectW(&params);
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
    CheckMenuItem(menu, IDM_VIEW_PREVIEW_PANE, MF_BYCOMMAND | (showPreview_ ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(menu, IDM_VIEW_PREVIEW_PANE, MF_BYCOMMAND | (mode_ == Mode::Browser ? MF_ENABLED : MF_GRAYED));
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
    navBar_.SetDpi(dpi, font_);
    commandBar_.SetDpi(dpi, font_);
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
        if (SUCCEEDED(settings.ReadInt(L"Browser", L"PreviewWidth", defaults.previewWidth, value)) && value >= 0 &&
            value <= 10000) {
            browser.previewWidth = value;
        }
        if (SUCCEEDED(settings.ReadBool(L"Browser", L"ShowPreview", true, flag))) browser.showPreview = flag;
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
        if (SUCCEEDED(writeResult)) writeResult = settings.WriteInt(L"Browser", L"PreviewWidth", finalBrowser.previewWidth);
        if (SUCCEEDED(writeResult)) writeResult = settings.WriteBool(L"Browser", L"ShowPreview", finalBrowser.showPreview);
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
            browser.previewWidth = previewWidth96_;
            browser.showPreview = showPreview_;
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
    previewWidth96_ = settings.previewWidth;
    showPreview_ = settings.showPreview;
    if (!showPreview_) ShowWindow(preview_.Hwnd(), SW_HIDE);
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
    ShowFolderStatus();
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
    if (showPreview_ && focused >= 0 && ListView_GetItemState(list_->Hwnd(), focused, LVIS_SELECTED)) {
        path = model_->FullPath(static_cast<std::size_t>(focused));
    }
    preview_.Show(path);
}

void MainWindow::ShowFolderStatus() {
    const CodecGap gap = model_->MissingCodecs();
    codecHint_.clear();
    codecHintPacks_ = gap.files ? gap.packs : 0;
    if (!codecHintPacks_) {
        SetStatusText(0, listFolder_);
        return;
    }
    std::wstring names;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(CodecPack::Count); ++i) {
        const auto pack = static_cast<CodecPack>(i);
        if (!(codecHintPacks_ & CodecPackBit(pack))) continue;
        if (!names.empty()) names += LoadStr(IDS_LIST_SEPARATOR);
        names += CodecPackName(pack);
    }
    codecHint_ = Format(Format(LoadStr(IDS_STATUS_CODEC_HINT), gap.files), names);
    SetStatusText(0, codecHint_);
}

bool MainWindow::StatusHintHit(POINT client) const {
    // Only while the hint is what the status line shows (other messages replace it).
    if (!codecHintPacks_ || codecHint_.empty() || statusText_[0] != codecHint_) return false;
    if (dragging_ != Splitter::None || !PtInRect(&statusRect_, client)) return false;
    return client.x < statusRect_.left + (statusRect_.right - statusRect_.left) / 2;
}

void MainWindow::OpenCodecStore(std::uint32_t packs) {
    const CodecPack pack = FirstCodecPack(packs);
    if (pack == CodecPack::Count) return;
    ShellExecuteW(hwnd_, L"open", CodecPackStoreUri(pack), nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::ShowCodecInfo() {
    // Enumerating WIC decoders reads a few registry keys: fine for a menu command.
    std::vector<std::wstring> extensions;
    const HRESULT hr = DecoderExtensions(extensions);
    const std::uint32_t missing = SUCCEEDED(hr) ? MissingCodecPacks(extensions) : 0;
    const std::wstring title = LoadStr(IDS_CODECS_TITLE);
    if (!missing) {
        MessageBoxW(hwnd_, LoadStr(IDS_CODECS_ALL_INSTALLED).c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring names;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(CodecPack::Count); ++i) {
        const auto pack = static_cast<CodecPack>(i);
        if (!(missing & CodecPackBit(pack))) continue;
        if (!names.empty()) names += LoadStr(IDS_LIST_SEPARATOR);
        names += CodecPackName(pack);
    }
    const std::wstring text = Format(Format(LoadStr(IDS_CODECS_MISSING), names), CodecPackName(FirstCodecPack(missing)));
    if (MessageBoxW(hwnd_, text.c_str(), title.c_str(), MB_YESNO | MB_ICONQUESTION) == IDYES) OpenCodecStore(missing);
}

void MainWindow::SetStatusText(int part, const std::wstring& text) {
    if (part < 0 || part >= static_cast<int>(ARRAYSIZE(statusText_)) || statusText_[part] == text) return;
    statusText_[part] = text;
    if (hwnd_) InvalidateRect(hwnd_, &statusRect_, FALSE);
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
        SetBarItems();
        ShowWindow(navBar_.Hwnd(), SW_HIDE);
        ShowWindow(pathBar_, SW_HIDE);
        ShowWindow(tree_.Hwnd(), SW_HIDE);
        ShowWindow(preview_.Hwnd(), SW_HIDE);
        ShowWindow(list_->Hwnd(), SW_HIDE);
        Layout();  // size the viewer before its first paint
        viewerPageKind_ = PageFor(model_->At(static_cast<std::size_t>(index)).kind);
        viewerIsText_ = viewerPageKind_ == ViewerPage::Text;
        ShowViewerPage();
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
    ShowWindow(textView_.Hwnd(), SW_HIDE);
    ShowWindow(documentView_.Hwnd(), SW_HIDE);
    ShowWindow(modelView_.Hwnd(), SW_HIDE);
    ShowWindow(videoView_.Hwnd(), SW_HIDE);
    viewer_.Clear();
    textView_.Clear();
    documentView_.Clear();
    modelView_.Clear();
    videoView_.Clear();
    viewerPageKind_ = ViewerPage::Picture;
    viewerIsText_ = false;
    viewerCache_->Retain({});
    SetBarItems();
    ShowWindow(navBar_.Hwnd(), SW_SHOW);
    ShowWindow(pathBar_, SW_SHOW);
    ShowWindow(tree_.Hwnd(), SW_SHOW);
    if (showPreview_) ShowWindow(preview_.Hwnd(), SW_SHOW);
    ShowWindow(list_->Hwnd(), SW_SHOW);
    Layout();
    // Land on the picture that was being viewed.
    if (index >= 0) list_->SelectOnly(index);
    SetFocus(list_->Hwnd());
    SetWindowTextW(hwnd_, LoadStr(IDS_APP_TITLE).c_str());
    ShowFolderStatus();
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
    viewerPage_ = 0;
    viewerKey_ = viewerPath_;
    viewerDirection_ = direction;
    const UINT side = ScreenSide();

    const FileKind kind = model_->At(static_cast<std::size_t>(index)).kind;
    const ViewerPage pageKind = PageFor(kind);
    if (pageKind != viewerPageKind_) {
        viewerPageKind_ = pageKind;
        viewerIsText_ = pageKind == ViewerPage::Text;
        ShowViewerPage();
    }
    if (pageKind != ViewerPage::Text) textView_.Clear();
    if (pageKind != ViewerPage::Document) documentView_.Clear();
    if (pageKind != ViewerPage::Model) modelView_.Clear();
    if (pageKind != ViewerPage::Video) videoView_.Clear();
    if (pageKind == ViewerPage::Text) {
        viewer_.Clear();
        textView_.Show(viewerPath_);
    } else if (pageKind == ViewerPage::Document) {
        viewer_.Clear();
        documentView_.Show(viewerPath_);
    } else if (pageKind == ViewerPage::Model) {
        viewer_.Clear();
        modelView_.Show(viewerPath_);
    } else if (pageKind == ViewerPage::Video) {
        viewer_.Clear();
        videoView_.Show(viewerPath_);
    } else {
        ViewerImagePtr image = viewerCache_->Get(viewerKey_, side);
        ThumbnailPtr placeholder;
        if (!image) {
            placeholder = thumbnails_->TryGetCached(list_->MakeThumbnailRequest(index));
            viewerCache_->Request(viewerKey_, side, true);
        }
        viewer_.SetImage(viewerKey_, image, placeholder, viewerCache_->Failed(viewerKey_));
    }

    // Preload the neighbours, the one in the travel direction first, and
    // let everything else go.
    std::vector<std::wstring> keep{viewerPath_};
    for (int offset : {direction, -direction}) {
        const int neighbour = index + offset;
        if (neighbour < 0 || static_cast<std::size_t>(neighbour) >= count) continue;
        if (PageFor(model_->At(static_cast<std::size_t>(neighbour)).kind) != ViewerPage::Picture) continue;
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

void MainWindow::ShowViewerPage() {
    // Called in viewer mode when the kind of the current file changes.
    HWND page = ViewerPageHwnd(viewerPageKind_);
    ShowWindow(page, SW_SHOW);
    for (ViewerPage other : {ViewerPage::Picture, ViewerPage::Text, ViewerPage::Document, ViewerPage::Model,
                             ViewerPage::Video}) {
        if (other != viewerPageKind_) ShowWindow(ViewerPageHwnd(other), SW_HIDE);
    }
    SetFocus(page);
}

MainWindow::ViewerPage MainWindow::PageFor(FileKind kind) {
    switch (kind) {
    case FileKind::Text: return ViewerPage::Text;
    case FileKind::Document: return ViewerPage::Document;
    case FileKind::Model: return ViewerPage::Model;
    case FileKind::Video: return ViewerPage::Video;
    default: return ViewerPage::Picture;  // pictures and PDF pages
    }
}

HWND MainWindow::ViewerPageHwnd(ViewerPage page) const {
    switch (page) {
    case ViewerPage::Text: return textView_.Hwnd();
    case ViewerPage::Document: return documentView_.Hwnd();
    case ViewerPage::Model: return modelView_.Hwnd();
    case ViewerPage::Video: return videoView_.Hwnd();
    default: return viewer_.Hwnd();
    }
}

void MainWindow::StepPdfPage(int step) {
    if (mode_ != Mode::Viewer || viewerPageKind_ != ViewerPage::Picture || !IsPdfPath(viewerPath_)) return;
    // The page count is known once any page of this document has decoded.
    const ViewerImagePtr current = viewerCache_->GetAny(viewerKey_);
    if (!current || current->pageCount == 0) return;
    const int page = static_cast<int>(viewerPage_) + step;
    if (page < 0 || page >= static_cast<int>(current->pageCount)) {
        MessageBeep(MB_OK);
        return;
    }
    viewerPage_ = static_cast<UINT>(page);
    viewerKey_ = PdfPagePath(viewerPath_, viewerPage_);
    const UINT side = ScreenSide();
    ViewerImagePtr image = viewerCache_->Get(viewerKey_, side);
    if (!image) viewerCache_->Request(viewerKey_, side, true);
    viewer_.SetImage(viewerKey_, image, nullptr, viewerCache_->Failed(viewerKey_));
    // Render the following page ahead and keep the neighbours of the file.
    std::vector<std::wstring> keep{viewerKey_};
    if (viewerPage_ + 1 < current->pageCount) {
        keep.push_back(PdfPagePath(viewerPath_, viewerPage_ + 1));
        viewerCache_->Request(keep.back(), side, false);
    }
    viewerCache_->Retain(keep);
    UpdateViewerStatus();
}

void MainWindow::RefreshViewerImage() {
    if (mode_ != Mode::Viewer || viewerPath_.empty() || viewerPageKind_ != ViewerPage::Picture) return;
    // Prefer the sharpest decode we have for the current picture.
    ViewerImagePtr image = viewerCache_->GetAny(viewerKey_);
    if (image) {
        viewer_.SetImage(viewerKey_, image, nullptr, false);
    } else if (viewerCache_->Failed(viewerKey_)) {
        viewer_.SetImage(viewerKey_, nullptr, nullptr, true);
    }
    UpdateViewerStatus();
}

void MainWindow::UpdateViewerStatus() {
    if (mode_ != Mode::Viewer) return;
    const int index = ViewerIndex();
    const auto slash = viewerPath_.find_last_of(L'\\');
    SetStatusText(0, slash == std::wstring::npos ? viewerPath_ : viewerPath_.substr(slash + 1));
    std::wstring position = LoadStr(IDS_VIEWER_INDEX);
    const auto first = position.find(L"%u");
    if (first != std::wstring::npos) position.replace(first, 2, std::to_wstring(index + 1));
    position = Format(position, model_->Count());
    SetStatusText(1, position);
    if (viewerPageKind_ == ViewerPage::Document) {
        switch (documentView_.GetState()) {
        case DocumentView::State::Loading: SetStatusText(2, LoadStr(IDS_VIEWER_LOADING)); break;
        case DocumentView::State::NoHandler: SetStatusText(2, LoadStr(IDS_DOC_NO_HANDLER_SHORT)); break;
        case DocumentView::State::Failed:
            SetStatusText(2, Format(LoadStr(IDS_DOC_ERROR), HexResult(documentView_.Error())));
            break;
        default: SetStatusText(2, LoadStr(IDS_DOC_PREVIEW)); break;
        }
        SetStatusText(3, std::wstring());
    } else if (viewerPageKind_ == ViewerPage::Video) {
        UINT state = IDS_VIEWER_LOADING;
        switch (videoView_.GetState()) {
        case VideoView::State::Playing: state = IDS_VIDEO_PLAYING; break;
        case VideoView::State::Paused: state = IDS_VIDEO_PAUSED; break;
        case VideoView::State::Ended: state = IDS_VIDEO_ENDED; break;
        case VideoView::State::NoCodec: state = IDS_VIDEO_NO_CODEC_SHORT; break;
        case VideoView::State::Failed: state = IDS_VIDEO_ERROR; break;
        default: break;
        }
        SetStatusText(2, LoadStr(state));
        SetStatusText(3, videoView_.Duration() > 0
                             ? FormatMediaTime(videoView_.Position()) + L" / " + FormatMediaTime(videoView_.Duration())
                             : std::wstring());
    } else if (viewerPageKind_ == ViewerPage::Model) {
        if (modelView_.Loading()) SetStatusText(2, LoadStr(IDS_VIEWER_LOADING));
        else if (modelView_.Failed()) SetStatusText(2, LoadStr(IDS_MODEL_ERROR));
        else SetStatusText(2, Format(LoadStr(IDS_MODEL_TRIANGLES), modelView_.Triangles()));
        SetStatusText(3, modelView_.Loading() || modelView_.Failed()
                             ? std::wstring()
                             : Format(LoadStr(IDS_VIEWER_ZOOM), static_cast<std::size_t>(modelView_.ZoomPercent())));
    } else if (viewerIsText_) {
        if (textView_.Loading()) {
            SetStatusText(2, LoadStr(IDS_VIEWER_LOADING));
        } else if (textView_.Failed()) {
            SetStatusText(2, LoadStr(IDS_TEXT_ERROR));
        } else {
            std::wstring info = TextEncodingName(textView_.Encoding());
            if (textView_.Truncated()) info += LoadStr(IDS_TEXT_TRUNCATED);
            SetStatusText(2, info);
        }
        SetStatusText(3, Format(LoadStr(IDS_VIEWER_ZOOM), static_cast<std::size_t>(textView_.ZoomPercent())));
    } else if (viewer_.HasImage()) {
        const ViewerImagePtr shown = viewerCache_->GetAny(viewerKey_);
        if (shown && shown->pageCount > 0) {
            // PDF: page position instead of pixel size.
            std::wstring pages = LoadStr(IDS_PDF_PAGE);
            const auto first = pages.find(L"%u");
            if (first != std::wstring::npos) pages.replace(first, 2, std::to_wstring(viewerPage_ + 1));
            SetStatusText(2, Format(pages, shown->pageCount));
        } else {
            SetStatusText(2, std::to_wstring(viewer_.ImageWidth()) + L" × " + std::to_wstring(viewer_.ImageHeight()));
        }
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
        ShowWindow(commandBar_.Hwnd(), SW_HIDE);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (windowedStyle_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd_, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        fullscreen_ = false;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, windowedStyle_);
        ShowWindow(commandBar_.Hwnd(), SW_SHOW);
        SetWindowPlacement(hwnd_, &windowedPlacement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    viewer_.SetFullscreen(fullscreen_);
    textView_.SetFullscreen(fullscreen_);
    documentView_.SetFullscreen(fullscreen_);
    modelView_.SetFullscreen(fullscreen_);
    videoView_.SetFullscreen(fullscreen_);
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
    commandBar_.SetChecked(IDM_VIEW_SLIDESHOW, slideshow_);
}

void MainWindow::StopSlideshow() {
    if (!slideshow_) return;
    slideshow_ = false;
    KillTimer(hwnd_, kSlideshowTimer);
    commandBar_.SetChecked(IDM_VIEW_SLIDESHOW, false);
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
        if (index < 0 || viewerPageKind_ != ViewerPage::Picture || IsPdfPath(viewerPath_)) return;
        indices.push_back(index);
        viewer_.RotatePending(clockwise);  // turn on screen right away
    } else {
        indices = list_->SelectedIndices();
    }
    for (int index : indices) {
        if (model_->At(static_cast<std::size_t>(index)).kind != FileKind::Image) continue;  // text files
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
