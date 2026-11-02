#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <functional>
#include <string>

// Viewer-mode page for Office and other documents: hosts the system preview
// handler (the same one File Explorer's preview pane uses) in a child window.
// LiteView bundles no document engine; a type with no handler installed shows
// a message instead. Must be used on the STA UI thread.
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

    // Loads `path` after the window has painted its "loading" state; the
    // preview handler may take a second or two to start.
    void Show(const std::wstring& path);
    void Clear();
    void SetFullscreen(bool fullscreen);
    void OnThemeChanged();

    enum class State { Empty, Loading, Shown, NoHandler, Failed };
    State GetState() const { return state_; }
    HRESULT Error() const { return error_; }  // when Failed

private:
    class Frame;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void Load();
    void Unload();
    void OnPaint();
    void ApplyColors();
    RECT HandlerRect() const;

    HWND hwnd_ = nullptr;
    HACCEL accelerators_ = nullptr;
    Callbacks callbacks_;
    Microsoft::WRL::ComPtr<IPreviewHandler> handler_;
    Microsoft::WRL::ComPtr<IStream> stream_;  // kept alive while a stream-based handler previews
    Microsoft::WRL::ComPtr<IPreviewHandlerFrame> frame_;
    std::wstring path_;
    State state_ = State::Empty;
    HRESULT error_ = S_OK;  // why the last load failed
    bool fullscreen_ = false;
};
