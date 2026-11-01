#pragma once

#include <windows.h>
#include <commctrl.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/FolderModel.h"

#include "core/ThumbnailService.h"

// Right-hand file list. A virtual (LVS_OWNERDATA) list view over FolderModel,
// so a folder with tens of thousands of images costs no per-item allocation
// in the control.
class FileListView {
public:
    enum class ViewMode { Thumbnails = 0, LargeIcons = 1, List = 2, Details = 3 };

    struct Selection {
        std::size_t count = 0;
        std::uint64_t bytes = 0;
    };

    explicit FileListView(FolderModel& model) : model_(model) {}
    ~FileListView();
    FileListView(const FileListView&) = delete;
    FileListView& operator=(const FileListView&) = delete;

    HRESULT Create(HWND parent, HINSTANCE instance, UINT controlId);
    // Thumbnails are drawn in the Thumbnails view when a service is set.
    void SetThumbnailService(ThumbnailService* service) { thumbnails_ = service; }
    void SetDpi(UINT dpi);
    HWND Hwnd() const { return hwnd_; }

    void SetViewMode(ViewMode mode);
    ViewMode GetViewMode() const { return mode_; }
    void SetSortIndicator(SortKey key, bool ascending);

    // Names of the selected and focused items. Take it before
    // FolderModel::Commit, because Commit invalidates the old items.
    struct SavedSelection {
        std::vector<std::wstring> names;
        std::wstring focus;
    };
    SavedSelection SaveSelection() const;

    // Call after FolderModel::Commit. Restores `saved` by file name, so a
    // re-sort or refresh keeps the selection; with nothing saved, the first
    // item is selected.
    // `generation` tags thumbnail requests so late results for an old folder
    // are ignored.
    void Reset(const SavedSelection& saved, std::uint32_t generation);
    void OnThumbnailReady(std::uint32_t generation, std::uint32_t itemId);

    void SelectAll();
    // Selects and focuses only `index` and scrolls it into view.
    void SelectOnly(int index);
    // The request the Thumbnails view uses for `index` (to look up its thumbnail).
    ThumbnailRequest MakeThumbnailRequest(int index) const;
    int FocusedIndex() const;
    std::vector<int> SelectedIndices() const;
    Selection GetSelection() const;

    using ColumnClick = std::function<void(SortKey key)>;
    using ItemActivate = std::function<void(int index)>;
    using SelectionChanged = std::function<void()>;
    // In-place rename finished with a new name; return true if the file was renamed.
    using RenameRequested = std::function<bool(int index, const std::wstring& newName)>;
    void SetRenameCallback(RenameRequested cb) { onRename_ = std::move(cb); }
    void BeginRename(int index);
    void SetColumnClickCallback(ColumnClick cb) { onColumnClick_ = std::move(cb); }
    void SetActivateCallback(ItemActivate cb) { onActivate_ = std::move(cb); }
    void SetSelectionChangedCallback(SelectionChanged cb) { onSelectionChanged_ = std::move(cb); }

    bool OnNotify(NMHDR* header, LRESULT& result);

private:
    void OnGetDispInfo(NMLVDISPINFOW* info);
    int FindItem(const NMLVFINDITEMW* find) const;
    const std::wstring& TypeName(const std::wstring& name);
    int IconIndex(const std::wstring& name);
    void AddColumns(HINSTANCE instance);
    LRESULT OnCustomDraw(NMLVCUSTOMDRAW* draw);
    void DrawThumbnail(HDC dc, int index);
    void RebuildThumbnailImageList();

    FolderModel& model_;
    HWND hwnd_ = nullptr;
    ViewMode mode_ = ViewMode::Details;
    HIMAGELIST imageSmall_ = nullptr;
    HIMAGELIST imageLarge_ = nullptr;
    HIMAGELIST imageExtraLarge_ = nullptr;
    HIMAGELIST imageThumbnail_ = nullptr;  // one blank cell the size of a thumbnail
    ThumbnailService* thumbnails_ = nullptr;
    std::uint32_t generation_ = 0;
    int thumbnailSize_ = 128;
    std::unordered_map<std::uint32_t, int> indexById_;
    std::unordered_map<std::wstring, std::wstring> typeNames_;  // ".jpg" -> "JPEG 影像"
    std::unordered_map<std::wstring, int> iconIndices_;
    wchar_t textBuffer_[64] = {};
    ColumnClick onColumnClick_;
    ItemActivate onActivate_;
    SelectionChanged onSelectionChanged_;
    RenameRequested onRename_;
};
