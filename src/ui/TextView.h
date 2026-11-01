#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "core/TextDocument.h"

class ThreadPool;

// Viewer-mode page for text files: a read-only, word-wrapped EDIT control in
// a monospaced font. The file is read on a background thread; only the
// latest request is kept. Zoom changes the font size.
class TextView {
public:
    static constexpr std::size_t kMaxBytes = 8u * 1024 * 1024;

    TextView();
    ~TextView();
    TextView(const TextView&) = delete;
    TextView& operator=(const TextView&) = delete;

    HRESULT Create(HWND parent, HINSTANCE instance, std::function<void()> changed);
    HWND Hwnd() const { return hwnd_; }
    HWND EditHwnd() const { return edit_; }

    void Show(const std::wstring& path);  // same path again reloads it
    void Clear();
    void SetFullscreen(bool fullscreen);
    void OnThemeChanged();

    void ZoomIn();
    void ZoomOut();
    void ResetZoom();

    bool Loading() const { return loading_; }
    bool Failed() const { return failed_; }
    bool Truncated() const { return truncated_; }
    TextEncoding Encoding() const { return encoding_; }
    int ZoomPercent() const { return zoom_; }

private:
    struct Shared {
        std::mutex mutex;
        HWND hwnd = nullptr;
        unsigned token = 0;
        unsigned readyToken = 0;
        HRESULT result = S_OK;
        TextDocument document;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void OnReady();
    void ApplyFont();
    void SetZoom(int percent);

    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH background_ = nullptr;
    std::function<void()> changed_;
    std::shared_ptr<Shared> shared_;
    std::unique_ptr<ThreadPool> pool_;
    std::wstring path_;
    bool loading_ = false;
    bool failed_ = false;
    bool truncated_ = false;
    bool fullscreen_ = false;
    TextEncoding encoding_ = TextEncoding::Utf8;
    int zoom_ = 100;
};
