#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/FolderModel.h"
#include "core/ThumbnailService.h"
#include "ui/FileListView.h"
#include "ui/FolderTree.h"
#include "ui/PreviewPane.h"
#include "ui/ViewerView.h"
#include "core/ViewerCache.h"

class ThreadPool;

// Top-level frame: menu, toolbar, path bar, status bar and the browser panes
// (folder tree and preview on the left, file list on the right). Owns the
// per-mode accelerator tables.
class MainWindow {
public:
    enum class Mode { Browser, Viewer };

    MainWindow();
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // Registers the class and creates the window with its child controls.
    // `startPath` is a folder or file from the command line, or empty.
    HRESULT Create(HINSTANCE instance, int showCommand, const std::wstring& startPath);
    HWND Hwnd() const { return hwnd_; }

    // Call from the message loop before TranslateMessage/DispatchMessage.
    bool TranslateAccelerator(MSG* msg);

private:
    struct BrowserSettings {
        std::wstring lastFolder;  // validated by the settings worker
        int leftWidth = 260;      // 96-DPI pixels
        int treeHeight = 320;     // 96-DPI pixels
        int viewMode = static_cast<int>(FileListView::ViewMode::Details);
        int sortKey = static_cast<int>(SortKey::Name);
        bool sortAscending = true;
        int slideshowSeconds = 3;
    };
    enum class Splitter { None, Vertical, Horizontal };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK PathBarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR id, DWORD_PTR data);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    bool OnCreate();
    void OnSize();
    void OnCommand(UINT id);
    LRESULT OnNotify(NMHDR* header);
    void OnInitMenuPopup(HMENU menu);
    void OnDpiChanged(UINT dpi, const RECT* suggested);
    HRESULT StartSettingsWorker() noexcept;
    void OnSettingsReady(WPARAM token);
    void ShowInitialWindow(int command);
    void OnClose();
    void OnSettingsSaved(WPARAM token);

    struct SettingsState;
    static void SettingsWorker(const std::shared_ptr<SettingsState>& state) noexcept;

    // Browser
    void InitBrowser(const BrowserSettings& settings);
    void Navigate(const std::wstring& path, const std::wstring& selectName = std::wstring());
    void NavigateToEnteredPath();
    void NavigateToParent();
    void Refresh();
    void OnFolderLoaded(std::uint32_t generation);
    void ApplySort(SortKey key, bool ascending);
    void OnSortCommand(SortKey key);
    void SetViewMode(FileListView::ViewMode mode);
    void RequestStatusUpdate();
    void UpdateStatus();
    void UpdatePreview();
    void SetStatusText(int part, const std::wstring& text);

    // Viewer
    void EnterViewer(int index);
    void ExitViewer();
    void ShowViewerIndex(int index, int direction);
    void ViewerStep(int step);
    void RefreshViewerImage();
    void UpdateViewerStatus();
    void SetFullscreen(bool fullscreen);
    void ToggleSlideshow();
    void StopSlideshow();
    UINT ScreenSide() const;
    int ViewerIndex() const;  // current index of the viewed item, or -1

    // File management (T7)
    std::wstring PickFolder(UINT titleId) const;
    std::vector<std::wstring> TargetPaths() const;  // selection, or the viewed picture
    int FindIndexByPath(const std::wstring& path) const;
    void CopyOrMoveSelection(bool move);
    void DeleteSelection();
    void RenameSelection();
    bool RenameItem(int index, const std::wstring& newName);
    void NewFolder();
    void RotateSelection(bool clockwise);
    void OnRotated();
    void ShowProperties();
    void ShowContextMenu(POINT screen);
    void AfterRemovingViewed(int index);
    void ShowError(UINT id, const std::wstring& detail);

    // Layout and splitters
    void Layout();
    Splitter HitTestSplitter(POINT client) const;
    void OnLButtonDown(POINT client);
    void OnMouseMove(POINT client);
    void OnLButtonUp();
    bool OnSetCursor();

    void CreateToolbar();
    void CreateStatusBar();
    void CreatePathBar();
    void UpdateStatusParts();
    std::wstring LoadStr(UINT id) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND toolbar_ = nullptr;
    HWND status_ = nullptr;
    HWND pathBar_ = nullptr;
    HFONT font_ = nullptr;
    HACCEL accelBrowser_ = nullptr;
    HACCEL accelViewer_ = nullptr;
    Mode mode_ = Mode::Browser;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    bool closing_ = false;
    bool shown_ = false;
    bool browserStarted_ = false;
    int showCommand_ = SW_SHOWNORMAL;
    std::shared_ptr<SettingsState> settingsState_;
    std::thread settingsThread_;

    // Browser state
    std::wstring startPath_;
    std::wstring listFolder_;     // folder whose items the list shows
    std::wstring pendingFolder_;  // folder being loaded
    std::wstring pendingSelect_;  // file to select once it is loaded
    std::uint32_t generation_ = 0;
    SortKey sortKey_ = SortKey::Name;
    bool sortAscending_ = true;
    bool statusPending_ = false;
    int leftWidth96_ = 260;
    int treeHeight96_ = 320;
    Splitter dragging_ = Splitter::None;
    int dragOffset_ = 0;
    RECT content_{};  // area between path bar and status bar

    // Viewer state
    std::uint32_t viewerItemId_ = 0;
    std::wstring viewerPath_;
    int viewerDirection_ = 1;
    bool openViewerOnLoad_ = false;
    bool fullscreen_ = false;
    bool slideshow_ = false;
    int slideshowSeconds_ = 3;
    WINDOWPLACEMENT windowedPlacement_{sizeof(WINDOWPLACEMENT)};
    LONG_PTR windowedStyle_ = 0;
    HMENU menu_ = nullptr;
    int selectIndexAfterLoad_ = -1;  // keeps the cursor in place after a delete or move

    struct RotateState;
    std::shared_ptr<RotateState> rotateState_;
    std::unique_ptr<ThreadPool> rotatePool_;
    std::size_t rotationsPending_ = 0;
    std::size_t rotationFailures_ = 0;

    std::shared_ptr<ThreadPool> pool_;
    std::unique_ptr<FolderModel> model_;
    std::unique_ptr<ThumbnailService> thumbnails_;
    std::unique_ptr<FileListView> list_;
    FolderTree tree_;
    PreviewPane preview_;
    std::unique_ptr<ViewerCache> viewerCache_;
    ViewerView viewer_;
};
