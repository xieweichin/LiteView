#pragma once

#include <windows.h>
#include <commctrl.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class ThreadPool;

// Left-top folder tree. Lists every drive; sub-folders are enumerated lazily
// on a background thread the first time a node is expanded, so slow network
// or optical drives never block the UI.
class FolderTree {
public:
    using NavigateCallback = std::function<void(const std::wstring& path)>;

    FolderTree();
    ~FolderTree();
    FolderTree(const FolderTree&) = delete;
    FolderTree& operator=(const FolderTree&) = delete;

    // `notify` receives WM_APP_TREE_CHILDREN; forward it to OnChildrenReady().
    HRESULT Create(HWND parent, HINSTANCE instance, UINT controlId, HWND notify);
    HWND Hwnd() const { return hwnd_; }

    // Called when the user picks a folder with the mouse or keyboard.
    void SetNavigateCallback(NavigateCallback cb) { onNavigate_ = std::move(cb); }

    // Expands the tree down to `path` and selects it, without firing the
    // navigate callback. Continues asynchronously as levels are loaded.
    void RevealPath(const std::wstring& path);

    // Handles WM_NOTIFY from the tree control. Returns true if handled.
    bool OnNotify(NMHDR* header, LRESULT& result);
    void OnChildrenReady();

private:
    struct Node {
        uint32_t id = 0;
        std::wstring path;  // "C:\", "C:\Photos"
        HTREEITEM item = nullptr;
        bool populated = false;
        bool loading = false;
    };
    struct Shared {
        std::mutex mutex;
        HWND notify = nullptr;
        struct Result {
            uint32_t nodeId;
            std::vector<std::wstring> names;
        };
        std::vector<Result> results;
    };

    void AddDrives();
    HTREEITEM InsertNode(HTREEITEM parent, const std::wstring& label, const std::wstring& path,
                         int image, bool hasChildren);
    void RequestChildren(Node& node);
    void ContinueReveal();
    Node* NodeFromItem(HTREEITEM item) const;
    HTREEITEM FindChild(HTREEITEM parent, const std::wstring& path) const;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    NavigateCallback onNavigate_;
    std::unordered_map<uint32_t, std::unique_ptr<Node>> nodes_;
    uint32_t nextNodeId_ = 1;
    int folderImage_ = 0;
    int folderOpenImage_ = 0;
    std::wstring revealTarget_;
    bool selectingProgrammatically_ = false;
    std::shared_ptr<Shared> shared_;
    std::unique_ptr<ThreadPool> pool_;  // declared last: joined first
};
