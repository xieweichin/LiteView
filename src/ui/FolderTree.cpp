#include "ui/FolderTree.h"
#include "ui/Theme.h"

#include <shellapi.h>
#include <shlwapi.h>
#include <uxtheme.h>

#include <algorithm>

#include "core/FileOps.h"
#include "core/ThreadPool.h"
#include "res/resource.h"
#include "ui/Messages.h"

namespace {

bool IsSeparator(wchar_t c) { return c == L'\\' || c == L'/'; }

// "C:\a\b\" -> "C:\a\b"; "C:" -> "C:\".
std::wstring Normalize(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3 && IsSeparator(path.back())) path.pop_back();
    if (path.size() == 2 && path[1] == L':') path += L'\\';
    return path;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// True if `path` is `prefix` or lies below it.
bool IsUnder(const std::wstring& path, const std::wstring& prefix) {
    if (path.size() < prefix.size()) return false;
    if (CompareStringOrdinal(path.c_str(), static_cast<int>(prefix.size()), prefix.c_str(),
                             static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL) {
        return false;
    }
    return path.size() == prefix.size() || IsSeparator(prefix.back()) || IsSeparator(path[prefix.size()]);
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    return IsSeparator(dir.back()) ? dir + name : dir + L'\\' + name;
}

std::vector<std::wstring> ListSubfolders(const std::wstring& dir) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileExW(Join(dir, L"*").c_str(), FindExInfoBasic, &fd,
                                   FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return names;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) && (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
        if (lstrcmpW(fd.cFileName, L".") == 0 || lstrcmpW(fd.cFileName, L"..") == 0) continue;
        names.emplace_back(fd.cFileName);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
        return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
    });
    return names;
}

int StockIcon(SHSTOCKICONID id) {
    SHSTOCKICONINFO info = {sizeof(info)};
    return SUCCEEDED(SHGetStockIconInfo(id, SHGSI_SYSICONINDEX | SHGSI_SMALLICON, &info))
               ? info.iSysImageIndex
               : 0;
}

std::wstring LoadStr(HINSTANCE instance, UINT id) {
    const wchar_t* text = nullptr;
    const int len = LoadStringW(instance, id, reinterpret_cast<LPWSTR>(&text), 0);
    return (len > 0 && text) ? std::wstring(text, static_cast<size_t>(len)) : std::wstring();
}

}  // namespace

FolderTree::FolderTree() : shared_(std::make_shared<Shared>()), pool_(std::make_unique<ThreadPool>(2)) {}

FolderTree::~FolderTree() {
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->notify = nullptr;
    }
    pool_->Shutdown();
}

HRESULT FolderTree::Create(HWND parent, HINSTANCE instance, UINT controlId, HWND notify) {
    instance_ = instance;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->notify = notify;
    }
    hwnd_ = CreateWindowExW(0, WC_TREEVIEWW, nullptr,
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT |
                                TVS_SHOWSELALWAYS | TVS_TRACKSELECT | TVS_EDITLABELS,
                            0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(controlId)),
                            instance, nullptr);
    if (!hwnd_) return HRESULT_FROM_WIN32(GetLastError());
    theme::ApplyTree(hwnd_);
    TreeView_SetExtendedStyle(hwnd_, TVS_EX_DOUBLEBUFFER | TVS_EX_AUTOHSCROLL,
                              TVS_EX_DOUBLEBUFFER | TVS_EX_AUTOHSCROLL);

    SHFILEINFOW sfi = {};
    auto images = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(
        L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
        SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
    folderImage_ = sfi.iIcon;
    SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                   SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_OPENICON);
    folderOpenImage_ = sfi.iIcon;
    if (images) TreeView_SetImageList(hwnd_, images, TVSIL_NORMAL);

    AddDrives();
    return S_OK;
}

void FolderTree::AddDrives() {
    wchar_t buffer[512] = {};
    const DWORD len = GetLogicalDriveStringsW(ARRAYSIZE(buffer) - 1, buffer);
    if (len == 0 || len >= ARRAYSIZE(buffer)) return;
    for (const wchar_t* drive = buffer; *drive; drive += wcslen(drive) + 1) {
        const std::wstring root = drive;  // "C:\"
        SHSTOCKICONID icon = SIID_DRIVEFIXED;
        switch (GetDriveTypeW(drive)) {
        case DRIVE_REMOVABLE: icon = SIID_DRIVEREMOVE; break;
        case DRIVE_REMOTE: icon = SIID_DRIVENET; break;
        case DRIVE_CDROM: icon = SIID_DRIVECD; break;
        case DRIVE_RAMDISK: icon = SIID_DRIVERAM; break;
        default: break;
        }
        // Volume labels would touch the drive; show the letter only.
        InsertNode(TVI_ROOT, root.substr(0, 2), root, StockIcon(icon), true);
    }
}

HTREEITEM FolderTree::InsertNode(HTREEITEM parent, const std::wstring& label, const std::wstring& path,
                                 int image, bool hasChildren) {
    auto node = std::make_unique<Node>();
    node->id = nextNodeId_++;
    node->path = path;

    TVINSERTSTRUCTW tvis = {};
    tvis.hParent = parent;
    tvis.hInsertAfter = TVI_LAST;
    tvis.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    tvis.item.pszText = const_cast<LPWSTR>(label.c_str());
    tvis.item.lParam = static_cast<LPARAM>(node->id);
    tvis.item.cChildren = hasChildren ? 1 : 0;
    tvis.item.iImage = image;
    tvis.item.iSelectedImage = image == folderImage_ ? folderOpenImage_ : image;
    node->item = TreeView_InsertItem(hwnd_, &tvis);
    HTREEITEM item = node->item;
    if (item) nodes_.emplace(node->id, std::move(node));
    return item;
}

FolderTree::Node* FolderTree::NodeFromItem(HTREEITEM item) const {
    if (!item) return nullptr;
    TVITEMW tvi = {};
    tvi.mask = TVIF_PARAM | TVIF_HANDLE;
    tvi.hItem = item;
    if (!TreeView_GetItem(hwnd_, &tvi)) return nullptr;
    auto it = nodes_.find(static_cast<uint32_t>(tvi.lParam));
    return it == nodes_.end() ? nullptr : it->second.get();
}

void FolderTree::RequestChildren(Node& node) {
    if (node.populated || node.loading) return;
    node.loading = true;

    // Placeholder so the expanded node shows activity.
    const std::wstring loading = LoadStr(instance_, IDS_TREE_LOADING);
    TVINSERTSTRUCTW tvis = {};
    tvis.hParent = node.item;
    tvis.hInsertAfter = TVI_LAST;
    tvis.item.mask = TVIF_TEXT | TVIF_PARAM;
    tvis.item.pszText = const_cast<LPWSTR>(loading.c_str());
    tvis.item.lParam = 0;
    TreeView_InsertItem(hwnd_, &tvis);

    const uint32_t id = node.id;
    const std::wstring path = node.path;
    std::weak_ptr<Shared> weak = shared_;
    pool_->Submit([weak, id, path] {
        std::vector<std::wstring> names = ListSubfolders(path);
        auto shared = weak.lock();
        if (!shared) return;
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (!shared->notify) return;
        shared->results.push_back({id, std::move(names)});
        PostMessageW(shared->notify, WM_APP_TREE_CHILDREN, 0, 0);
    });
}

void FolderTree::OnChildrenReady() {
    if (!pendingRefresh_.empty()) {
        const std::wstring path = pendingRefresh_;
        pendingRefresh_.clear();
        Refresh(path);
    }
    std::vector<Shared::Result> results;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        results.swap(shared_->results);
    }
    if (results.empty()) return;

    for (auto& result : results) {
        auto it = nodes_.find(result.nodeId);
        if (it == nodes_.end()) continue;
        Node& node = *it->second;
        node.loading = false;
        node.populated = true;

        SendMessageW(hwnd_, WM_SETREDRAW, FALSE, 0);
        // Remove the placeholder (lParam == 0).
        for (HTREEITEM child = TreeView_GetChild(hwnd_, node.item); child;) {
            HTREEITEM next = TreeView_GetNextSibling(hwnd_, child);
            if (!NodeFromItem(child)) TreeView_DeleteItem(hwnd_, child);
            child = next;
        }
        for (const auto& name : result.names) {
            InsertNode(node.item, name, Join(node.path, name), folderImage_, true);
        }
        if (result.names.empty()) {
            TVITEMW tvi = {};
            tvi.mask = TVIF_CHILDREN | TVIF_HANDLE;
            tvi.hItem = node.item;
            tvi.cChildren = 0;
            TreeView_SetItem(hwnd_, &tvi);
        }
        SendMessageW(hwnd_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    ContinueReveal();
    if (!editTarget_.empty()) {
        if (Node* node = FindNode(editTarget_)) {
            editTarget_.clear();
            TreeView_EnsureVisible(hwnd_, node->item);
            SetFocus(hwnd_);
            TreeView_EditLabel(hwnd_, node->item);
        }
    }
}

bool FolderTree::OnNotify(NMHDR* header, LRESULT& result) {
    if (header->hwndFrom != hwnd_) return false;
    switch (header->code) {
    case TVN_ITEMEXPANDINGW: {
        auto* nm = reinterpret_cast<NMTREEVIEWW*>(header);
        if (nm->action & TVE_EXPAND) {
            if (Node* node = NodeFromItem(nm->itemNew.hItem)) RequestChildren(*node);
        }
        result = FALSE;
        return true;
    }
    case TVN_BEGINLABELEDITW: {
        // Drives cannot be renamed here.
        auto* info = reinterpret_cast<NMTVDISPINFOW*>(header);
        Node* node = NodeFromItem(info->item.hItem);
        result = (!node || node->path.size() <= 3) ? TRUE : FALSE;
        return true;
    }
    case TVN_ENDLABELEDITW:
        result = OnEndLabelEdit(reinterpret_cast<NMTVDISPINFOW*>(header)) ? TRUE : FALSE;
        return true;
    case TVN_SELCHANGEDW: {
        auto* nm = reinterpret_cast<NMTREEVIEWW*>(header);
        if (!selectingProgrammatically_ && nm->action != TVC_UNKNOWN) {
            revealTarget_.clear();
            if (Node* node = NodeFromItem(nm->itemNew.hItem)) {
                if (onNavigate_) onNavigate_(node->path);
            }
        }
        result = 0;
        return true;
    }
    }
    return false;
}

HTREEITEM FolderTree::FindChild(HTREEITEM parent, const std::wstring& path) const {
    HTREEITEM child = parent ? TreeView_GetChild(hwnd_, parent) : TreeView_GetRoot(hwnd_);
    for (; child; child = TreeView_GetNextSibling(hwnd_, child)) {
        if (Node* node = NodeFromItem(child)) {
            if (IsUnder(path, node->path)) return child;
        }
    }
    return nullptr;
}

void FolderTree::RevealPath(const std::wstring& path) {
    revealTarget_ = Normalize(path);
    ContinueReveal();
}

void FolderTree::ContinueReveal() {
    if (revealTarget_.empty() || !hwnd_) return;
    HTREEITEM item = FindChild(nullptr, revealTarget_);
    HTREEITEM deepest = nullptr;
    while (item) {
        deepest = item;
        Node* node = NodeFromItem(item);
        if (!node) break;
        if (SamePath(node->path, revealTarget_)) break;
        if (!node->populated) {
            RequestChildren(*node);
            TreeView_Expand(hwnd_, item, TVE_EXPAND);
            // Select what we have so far; continue when children arrive.
            selectingProgrammatically_ = true;
            TreeView_SelectItem(hwnd_, item);
            selectingProgrammatically_ = false;
            return;
        }
        TreeView_Expand(hwnd_, item, TVE_EXPAND);
        HTREEITEM child = FindChild(item, revealTarget_);
        if (!child) break;  // hidden or missing folder: stop at the deepest match
        item = child;
    }
    if (deepest) {
        selectingProgrammatically_ = true;
        TreeView_SelectItem(hwnd_, deepest);
        TreeView_EnsureVisible(hwnd_, deepest);
        selectingProgrammatically_ = false;
    }
    revealTarget_.clear();
}

FolderTree::Node* FolderTree::FindNode(const std::wstring& path) const {
    const std::wstring target = Normalize(path);
    for (const auto& entry : nodes_) {
        if (SamePath(entry.second->path, target)) return entry.second.get();
    }
    return nullptr;
}

void FolderTree::Refresh(const std::wstring& path) {
    Node* node = FindNode(path);
    if (!node || node->loading) return;
    const bool expanded = (TreeView_GetItemState(hwnd_, node->item, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
    // Drop the children (and their bookkeeping), then list again.
    for (HTREEITEM child = TreeView_GetChild(hwnd_, node->item); child;) {
        HTREEITEM next = TreeView_GetNextSibling(hwnd_, child);
        TreeView_DeleteItem(hwnd_, child);
        child = next;
    }
    for (auto it = nodes_.begin(); it != nodes_.end();) {
        const std::wstring& p = it->second->path;
        const bool below = p.size() > node->path.size() && IsUnder(p, node->path);
        it = below ? nodes_.erase(it) : std::next(it);
    }
    node->populated = false;
    TVITEMW tvi = {};
    tvi.mask = TVIF_CHILDREN | TVIF_HANDLE;
    tvi.hItem = node->item;
    tvi.cChildren = 1;
    TreeView_SetItem(hwnd_, &tvi);
    if (expanded) RequestChildren(*node);
}

void FolderTree::EditChildWhenListed(const std::wstring& parent, const std::wstring& name) {
    editTarget_ = Join(Normalize(parent), name);
    Node* node = FindNode(parent);
    if (!node) {
        editTarget_.clear();
        return;
    }
    Refresh(parent);
    if (!node->loading) RequestChildren(*node);
    TreeView_Expand(hwnd_, node->item, TVE_EXPAND);
}

bool FolderTree::OnEndLabelEdit(NMTVDISPINFOW* info) {
    if (!info->item.pszText) return false;  // cancelled
    Node* node = NodeFromItem(info->item.hItem);
    if (!node) return false;
    const std::wstring name = info->item.pszText;
    if (!FileOps::IsValidFileName(name)) {
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    const auto slash = node->path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return false;
    const std::wstring parent = node->path.substr(0, slash == 2 ? 3 : slash);
    const std::wstring target = Join(parent, name);
    if (FAILED(FileOps::Rename(node->path, target))) {
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    // Paths below the renamed folder changed: list the parent again, but not
    // while the control is still inside this notification.
    pendingRefresh_ = parent;
    PostMessageW(GetParent(hwnd_), WM_APP_TREE_CHILDREN, 0, 0);
    return false;  // the refresh supplies the new label
}
