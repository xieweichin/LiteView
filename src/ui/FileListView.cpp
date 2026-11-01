#include "ui/FileListView.h"

#include <commoncontrols.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <unordered_set>

#include "core/ThumbnailService.h"
#include "res/resource.h"

namespace {

std::wstring Lower(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

std::wstring ExtensionOf(const std::wstring& name) {
    const auto dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? std::wstring() : Lower(name.substr(dot));
}

// IID_IImageList; spelled out because not every SDK's uuid library has it.
constexpr IID kIidImageList = {0x46EB5926, 0x582E, 0x4017, {0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50}};

HIMAGELIST ShellImageList(int which) {
    IImageList* list = nullptr;
    if (FAILED(SHGetImageList(which, kIidImageList, reinterpret_cast<void**>(&list)))) return nullptr;
    // The shell keeps the list alive for the process lifetime.
    HIMAGELIST handle = reinterpret_cast<HIMAGELIST>(list);
    list->Release();
    return handle;
}

std::wstring LoadStr(HINSTANCE instance, UINT id) {
    const wchar_t* text = nullptr;
    const int len = LoadStringW(instance, id, reinterpret_cast<LPWSTR>(&text), 0);
    return (len > 0 && text) ? std::wstring(text, static_cast<size_t>(len)) : std::wstring();
}

enum Column { kColName, kColSize, kColType, kColModified };

constexpr int kThumbnailSize = 128;  // 96-DPI pixels

}  // namespace

FileListView::~FileListView() {
    if (imageThumbnail_) ImageList_Destroy(imageThumbnail_);
}

void FileListView::SetDpi(UINT dpi) {
    const int size = MulDiv(kThumbnailSize, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    if (size == thumbnailSize_ && imageThumbnail_) return;
    thumbnailSize_ = size;
    RebuildThumbnailImageList();
}

void FileListView::RebuildThumbnailImageList() {
    // The list view sizes icon cells from the normal image list, so the
    // Thumbnails view uses one transparent cell and paints over it.
    HIMAGELIST old = imageThumbnail_;
    imageThumbnail_ = ImageList_Create(thumbnailSize_, thumbnailSize_, ILC_COLOR32 | ILC_MASK, 1, 0);
    if (imageThumbnail_) {
        void* bits = nullptr;
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = thumbnailSize_;
        info.bmiHeader.biHeight = -thumbnailSize_;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        HBITMAP blank = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);  // zeroed
        // An all-ones mask marks every pixel transparent.
        std::vector<BYTE> ones(static_cast<std::size_t>((thumbnailSize_ + 15) / 16 * 2) * thumbnailSize_, 0xFF);
        HBITMAP mask = CreateBitmap(thumbnailSize_, thumbnailSize_, 1, 1, ones.data());
        if (blank && mask) ImageList_Add(imageThumbnail_, blank, mask);
        if (blank) DeleteObject(blank);
        if (mask) DeleteObject(mask);
    }
    if (hwnd_ && mode_ == ViewMode::Thumbnails) ListView_SetImageList(hwnd_, imageThumbnail_, LVSIL_NORMAL);
    if (old) ImageList_Destroy(old);
}


HRESULT FileListView::Create(HWND parent, HINSTANCE instance, UINT controlId) {
    hwnd_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
                                LVS_SHAREIMAGELISTS | LVS_SHOWSELALWAYS | LVS_EDITLABELS | LVS_AUTOARRANGE,
                            0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(controlId)),
                            instance, nullptr);
    if (!hwnd_) return HRESULT_FROM_WIN32(GetLastError());
    SetWindowTheme(hwnd_, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(hwnd_, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP);

    imageSmall_ = ShellImageList(SHIL_SMALL);
    imageLarge_ = ShellImageList(SHIL_LARGE);
    imageExtraLarge_ = ShellImageList(SHIL_EXTRALARGE);
    if (!imageExtraLarge_) imageExtraLarge_ = imageLarge_;
    ListView_SetImageList(hwnd_, imageSmall_, LVSIL_SMALL);

    AddColumns(instance);
    SetDpi(GetDpiForWindow(hwnd_));
    SetViewMode(ViewMode::Details);
    return S_OK;
}

void FileListView::AddColumns(HINSTANCE instance) {
    struct Def { UINT text; int width; int fmt; };
    const Def defs[] = {
        {IDS_COL_NAME, 260, LVCFMT_LEFT},
        {IDS_COL_SIZE, 90, LVCFMT_RIGHT},
        {IDS_COL_TYPE, 130, LVCFMT_LEFT},
        {IDS_COL_MODIFIED, 150, LVCFMT_LEFT},
    };
    const UINT dpi = GetDpiForWindow(hwnd_);
    for (int i = 0; i < static_cast<int>(ARRAYSIZE(defs)); ++i) {
        std::wstring text = LoadStr(instance, defs[i].text);
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
        col.fmt = defs[i].fmt;
        col.cx = MulDiv(defs[i].width, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
        col.pszText = text.data();
        col.iSubItem = i;
        ListView_InsertColumn(hwnd_, i, &col);
    }
}

void FileListView::SetViewMode(ViewMode mode) {
    mode_ = mode;
    DWORD view = LVS_REPORT;
    HIMAGELIST normal = imageLarge_;
    switch (mode) {
    case ViewMode::Thumbnails: view = LVS_ICON; normal = imageThumbnail_ ? imageThumbnail_ : imageExtraLarge_; break;
    case ViewMode::LargeIcons: view = LVS_ICON; normal = imageLarge_; break;
    case ViewMode::List: view = LVS_LIST; break;
    case ViewMode::Details: view = LVS_REPORT; break;
    }
    if (mode != ViewMode::Thumbnails && thumbnails_) thumbnails_->CancelQueued();
    ListView_SetImageList(hwnd_, normal, LVSIL_NORMAL);
    // Switch through the window style: the classic way, which every
    // list view implementation arranges virtual items for.
    const LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, (style & ~static_cast<LONG_PTR>(LVS_TYPEMASK)) | view);
    if (mode == ViewMode::Thumbnails) {
        const int pad = MulDiv(thumbnailSize_, 1, 5);
        ListView_SetIconSpacing(hwnd_, thumbnailSize_ + pad, thumbnailSize_ + pad + 2 * GetSystemMetrics(SM_CYMENU));
    } else if (mode == ViewMode::LargeIcons) {
        ListView_SetIconSpacing(hwnd_, -1, -1);  // system default
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
    const int focused = FocusedIndex();
    if (focused >= 0) ListView_EnsureVisible(hwnd_, focused, FALSE);
}

void FileListView::SetSortIndicator(SortKey key, bool ascending) {
    HWND header = ListView_GetHeader(hwnd_);
    const int sorted = static_cast<int>(key);  // columns follow SortKey order
    for (int i = 0; i < Header_GetItemCount(header); ++i) {
        HDITEMW item = {};
        item.mask = HDI_FORMAT;
        Header_GetItem(header, i, &item);
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == sorted) item.fmt |= ascending ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(header, i, &item);
    }
}

FileListView::SavedSelection FileListView::SaveSelection() const {
    SavedSelection saved;
    const std::size_t count = model_.Count();
    for (int i : SelectedIndices()) {
        if (static_cast<std::size_t>(i) < count) saved.names.push_back(model_.At(i).name);
    }
    const int focused = FocusedIndex();
    if (focused >= 0 && static_cast<std::size_t>(focused) < count) saved.focus = model_.At(focused).name;
    return saved;
}

void FileListView::Reset(const SavedSelection& saved, std::uint32_t generation) {
    if (generation != generation_ && thumbnails_) thumbnails_->CancelQueued();
    generation_ = generation;
    SendMessageW(hwnd_, WM_SETREDRAW, FALSE, 0);
    ListView_SetItemState(hwnd_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    const std::size_t count = model_.Count();
    ListView_SetItemCountEx(hwnd_, static_cast<int>(count), 0);
    indexById_.clear();
    indexById_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) indexById_[model_.At(i).id] = static_cast<int>(i);

    int focus = -1;
    bool anySelected = false;
    if (!saved.names.empty() || !saved.focus.empty()) {
        std::unordered_set<std::wstring> wanted;
        for (const auto& name : saved.names) wanted.insert(Lower(name));
        const std::wstring focusKey = Lower(saved.focus);
        for (std::size_t i = 0; i < count; ++i) {
            const std::wstring key = Lower(model_.At(i).name);
            if (wanted.count(key)) {
                ListView_SetItemState(hwnd_, static_cast<int>(i), LVIS_SELECTED, LVIS_SELECTED);
                anySelected = true;
            }
            if (focus < 0 && !focusKey.empty() && key == focusKey) focus = static_cast<int>(i);
        }
    }
    if (focus < 0) focus = anySelected ? ListView_GetNextItem(hwnd_, -1, LVNI_SELECTED) : (count > 0 ? 0 : -1);
    if (focus >= 0) {
        ListView_SetItemState(hwnd_, focus, LVIS_FOCUSED, LVIS_FOCUSED);
        // Like Explorer, an unselected list starts with the first item selected.
        if (!anySelected) ListView_SetItemState(hwnd_, focus, LVIS_SELECTED, LVIS_SELECTED);
        ListView_EnsureVisible(hwnd_, focus, FALSE);
    }
    SendMessageW(hwnd_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hwnd_, nullptr, TRUE);
    if (onSelectionChanged_) onSelectionChanged_();
}

void FileListView::SelectAll() { ListView_SetItemState(hwnd_, -1, LVIS_SELECTED, LVIS_SELECTED); }

void FileListView::SelectOnly(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= model_.Count()) return;
    ListView_SetItemState(hwnd_, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(hwnd_, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetSelectionMark(hwnd_, index);
    ListView_EnsureVisible(hwnd_, index, FALSE);
}

ThumbnailRequest FileListView::MakeThumbnailRequest(int index) const {
    ThumbnailRequest request;
    if (index < 0 || static_cast<std::size_t>(index) >= model_.Count()) return request;
    const FileItem& file = model_.At(static_cast<std::size_t>(index));
    request.generation = generation_;
    request.itemId = file.id;
    request.path = model_.FullPath(static_cast<std::size_t>(index));
    request.modified = file.modified;
    request.fileSize = file.size;
    request.size = static_cast<UINT>(thumbnailSize_);
    return request;
}

int FileListView::FocusedIndex() const { return ListView_GetNextItem(hwnd_, -1, LVNI_FOCUSED); }

std::vector<int> FileListView::SelectedIndices() const {
    std::vector<int> indices;
    const int selected = ListView_GetSelectedCount(hwnd_);
    if (selected <= 0) return indices;
    indices.reserve(static_cast<std::size_t>(selected));
    for (int i = ListView_GetNextItem(hwnd_, -1, LVNI_SELECTED); i >= 0;
         i = ListView_GetNextItem(hwnd_, i, LVNI_SELECTED)) {
        indices.push_back(i);
    }
    return indices;
}

FileListView::Selection FileListView::GetSelection() const {
    Selection selection;
    const std::size_t count = model_.Count();
    for (int i : SelectedIndices()) {
        if (static_cast<std::size_t>(i) >= count) continue;
        ++selection.count;
        selection.bytes += model_.At(i).size;
    }
    return selection;
}

const std::wstring& FileListView::TypeName(const std::wstring& name) {
    const std::wstring ext = ExtensionOf(name);
    auto it = typeNames_.find(ext);
    if (it != typeNames_.end()) return it->second;
    SHFILEINFOW sfi = {};
    std::wstring type;
    if (SHGetFileInfoW(ext.empty() ? L"file" : ext.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME)) {
        type = sfi.szTypeName;
    }
    if (type.empty()) type = ext.size() > 1 ? ext.substr(1) : ext;
    return typeNames_.emplace(ext, std::move(type)).first->second;
}

int FileListView::IconIndex(const std::wstring& name) {
    const std::wstring ext = ExtensionOf(name);
    auto it = iconIndices_.find(ext);
    if (it != iconIndices_.end()) return it->second;
    SHFILEINFOW sfi = {};
    SHGetFileInfoW(ext.empty() ? L"file" : ext.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                   SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX);
    return iconIndices_.emplace(ext, sfi.iIcon).first->second;
}

void FileListView::OnGetDispInfo(NMLVDISPINFOW* info) {
    LVITEMW& item = info->item;
    if (item.iItem < 0 || static_cast<std::size_t>(item.iItem) >= model_.Count()) return;
    const FileItem& file = model_.At(static_cast<std::size_t>(item.iItem));
    if (item.mask & LVIF_IMAGE) item.iImage = mode_ == ViewMode::Thumbnails ? 0 : IconIndex(file.name);
    if (!(item.mask & LVIF_TEXT) || !item.pszText || item.cchTextMax <= 0) return;

    switch (item.iSubItem) {
    case kColName:
        // The model owns the string until the next Commit, which outlives this call.
        item.pszText = const_cast<LPWSTR>(file.name.c_str());
        break;
    case kColSize:
        StrFormatKBSizeW(static_cast<LONGLONG>(file.size), textBuffer_, ARRAYSIZE(textBuffer_));
        item.pszText = textBuffer_;
        break;
    case kColType:
        item.pszText = const_cast<LPWSTR>(TypeName(file.name).c_str());
        break;
    case kColModified: {
        FILETIME local{};
        SYSTEMTIME st{};
        textBuffer_[0] = 0;
        if (FileTimeToLocalFileTime(&file.modified, &local) && FileTimeToSystemTime(&local, &st)) {
            int n = GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, textBuffer_,
                                    ARRAYSIZE(textBuffer_), nullptr);
            if (n > 0) {
                textBuffer_[n - 1] = L' ';
                GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, textBuffer_ + n,
                                ARRAYSIZE(textBuffer_) - n);
            }
        }
        item.pszText = textBuffer_;
        break;
    }
    }
}

int FileListView::FindItem(const NMLVFINDITEMW* find) const {
    if (!(find->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) || !find->lvfi.psz) return -1;
    const std::size_t count = model_.Count();
    if (count == 0) return -1;
    const int len = lstrlenW(find->lvfi.psz);
    const bool partial = (find->lvfi.flags & LVFI_PARTIAL) != 0;
    std::size_t start = find->iStart < 0 ? 0 : static_cast<std::size_t>(find->iStart);
    if (start >= count) start = 0;
    for (std::size_t n = 0; n < count; ++n) {
        const std::size_t i = (start + n) % count;
        if (!(find->lvfi.flags & LVFI_WRAP) && i < start) break;
        const std::wstring& name = model_.At(i).name;
        const int cmpLen = partial ? len : -1;
        if (partial && static_cast<int>(name.size()) < len) continue;
        if (CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, name.c_str(), cmpLen, find->lvfi.psz,
                            partial ? len : -1, nullptr, nullptr, 0) == CSTR_EQUAL) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool FileListView::OnNotify(NMHDR* header, LRESULT& result) {
    if (header->hwndFrom != hwnd_) return false;
    result = 0;
    switch (header->code) {
    case LVN_GETDISPINFOW:
        OnGetDispInfo(reinterpret_cast<NMLVDISPINFOW*>(header));
        return true;
    case LVN_ODFINDITEMW:
        result = FindItem(reinterpret_cast<NMLVFINDITEMW*>(header));
        return true;
    case LVN_COLUMNCLICK: {
        const int column = reinterpret_cast<NMLISTVIEW*>(header)->iSubItem;
        if (onColumnClick_ && column >= 0 && column <= kColModified) onColumnClick_(static_cast<SortKey>(column));
        return true;
    }
    case LVN_ITEMACTIVATE: {
        const int index = reinterpret_cast<NMITEMACTIVATE*>(header)->iItem;
        if (onActivate_) onActivate_(index >= 0 ? index : FocusedIndex());
        return true;
    }
    case LVN_ITEMCHANGED: {
        auto* nm = reinterpret_cast<NMLISTVIEW*>(header);
        if ((nm->uChanged & LVIF_STATE) && ((nm->uOldState ^ nm->uNewState) & (LVIS_SELECTED | LVIS_FOCUSED))) {
            if (onSelectionChanged_) onSelectionChanged_();
        }
        return true;
    }
    case LVN_ODSTATECHANGED:
        if (onSelectionChanged_) onSelectionChanged_();
        return true;
    case NM_CUSTOMDRAW:
        result = OnCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(header));
        return true;
    case LVN_BEGINSCROLL:
        // Items scrolled out of view no longer need thumbnails; visible
        // ones are requested again as they paint.
        if (thumbnails_) thumbnails_->CancelQueued();
        return true;
    case LVN_BEGINLABELEDITW:
        result = TRUE;  // in-place rename arrives with T7
        return true;
    }
    return false;
}

LRESULT FileListView::OnCustomDraw(NMLVCUSTOMDRAW* draw) {
    if (mode_ != ViewMode::Thumbnails || !thumbnails_) return CDRF_DODEFAULT;
    switch (draw->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYPOSTPAINT;
    case CDDS_ITEMPOSTPAINT:
        DrawThumbnail(draw->nmcd.hdc, static_cast<int>(draw->nmcd.dwItemSpec));
        return CDRF_DODEFAULT;
    }
    return CDRF_DODEFAULT;
}

void FileListView::DrawThumbnail(HDC dc, int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= model_.Count()) return;
    RECT cell{};
    if (!ListView_GetItemRect(hwnd_, index, &cell, LVIR_ICON)) return;
    const FileItem& file = model_.At(static_cast<std::size_t>(index));
    const ThumbnailRequest request = MakeThumbnailRequest(index);

    bool failed = false;
    ThumbnailPtr thumbnail = thumbnails_->TryGetCached(request, &failed);
    const int cellW = cell.right - cell.left;
    const int cellH = cell.bottom - cell.top;
    if (thumbnail) {
        const double scale = std::min({1.0, static_cast<double>(cellW) / thumbnail->width,
                                       static_cast<double>(cellH) / thumbnail->height});
        const int w = std::max(1, static_cast<int>(thumbnail->width * scale + 0.5));
        const int h = std::max(1, static_cast<int>(thumbnail->height * scale + 0.5));
        const int x = cell.left + (cellW - w) / 2;
        const int y = cell.top + (cellH - h) / 2;
        HDC memory = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(memory, thumbnail->bitmap);
        if (w == static_cast<int>(thumbnail->width) && h == static_cast<int>(thumbnail->height)) {
            BitBlt(dc, x, y, w, h, memory, 0, 0, SRCCOPY);
        } else {
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchBlt(dc, x, y, w, h, memory, 0, 0, static_cast<int>(thumbnail->width),
                       static_cast<int>(thumbnail->height), SRCCOPY);
        }
        SelectObject(memory, old);
        DeleteDC(memory);
        // Thin frame so white pictures do not melt into the background.
        RECT frame{x - 1, y - 1, x + w + 1, y + h + 1};
        FrameRect(dc, &frame, GetSysColorBrush(COLOR_BTNSHADOW));
        return;
    }
    // Not ready: the file-type icon stands in.
    int iconW = 0, iconH = 0;
    if (imageExtraLarge_ && ImageList_GetIconSize(imageExtraLarge_, &iconW, &iconH)) {
        ImageList_Draw(imageExtraLarge_, IconIndex(file.name), dc, cell.left + (cellW - iconW) / 2,
                       cell.top + (cellH - iconH) / 2, ILD_TRANSPARENT);
    }
    if (!failed) thumbnails_->Request(request, true);
}

void FileListView::OnThumbnailReady(std::uint32_t generation, std::uint32_t itemId) {
    if (generation != generation_ || mode_ != ViewMode::Thumbnails) return;
    auto it = indexById_.find(itemId);
    if (it == indexById_.end()) return;
    RECT rc{};
    if (ListView_GetItemRect(hwnd_, it->second, &rc, LVIR_ICON)) InvalidateRect(hwnd_, &rc, FALSE);
}
