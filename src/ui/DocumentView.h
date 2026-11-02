#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <functional>
#include <memory>
#include <string>

struct DocumentHost;

// Viewer-mode page for Office and other documents: hosts the system preview
// handler (the same one File Explorer's preview pane uses) in a child window.
// LiteView bundles no document engine; a type with no handler installed shows
// a message instead. The handler is started and driven on a thread of its
// own, so a slow handler never holds up the window. Use on the UI thread.
class DocumentView {
public:
    struct Callbacks {
        std::function<bool(MSG*)> translateAccelerator;  // keys the handler did not use
        std::function<void()> changed;                   // load finished: refresh the status line
    };

    DocumentView() = default;
    ~DocumentView();
    DocumentView(const DocumentView&) = delete;
    DocumentView& operator=(const DocumentView&) = delete;

    HRESULT Create(HWND parent, HINSTANCE instance, HACCEL accelerators, Callbacks callbacks);
    HWND Hwnd() const { return hwnd_; }

    // Starts loading `path` and returns at once; the preview handler may
    // take a second or two to start.
    void Show(const std::wstring& path);
    void Clear();
    // Clear() and wait (briefly) until the handler has closed the file, so
    // that it can be renamed, moved or deleted.
    void Release();
    void SetFullscreen(bool fullscreen);
    void OnThemeChanged();
    // false: loading a document leaves keyboard focus where it is (the
    // browser's preview pane, where focus stays in the file list).
    void SetTakeFocus(bool take) { takeFocus_ = take; }

    enum class State { Empty, Loading, Shown, NoHandler, Failed };
    State GetState() const { return state_; }
    HRESULT Error() const { return error_; }  // when Failed

private:

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    bool StartHost();
    void StopHost();
    void OnLoaded(unsigned generation, HRESULT hr);
    void OnPaint();
    void ApplyColors();

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HACCEL accelerators_ = nullptr;
    Callbacks callbacks_;
    std::shared_ptr<DocumentHost> host_;  // shared with the host thread
    HANDLE thread_ = nullptr;
    unsigned generation_ = 0;  // the request whose result is awaited
    HWND focusBefore_ = nullptr;
    std::wstring path_;
    State state_ = State::Empty;
    HRESULT error_ = S_OK;  // why the last load failed
    bool takeFocus_ = true;
    bool fullscreen_ = false;
};
