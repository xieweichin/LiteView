#pragma once

#include <windows.h>

#include <memory>
#include <mutex>
#include <string>

#include "ui/DocumentView.h"

class ThreadPool;

// Right-hand preview of the selected file (pictures, PDF, vector, 3D, Office
// documents through their preview handler, and the shell's thumbnail for videos), fitted into the pane and
// centred, never enlarged; a text file shows its first lines. With nothing
// to show, a short hint is drawn instead. Decoding runs on its own background thread;
// only the latest request is kept.
class PreviewPane {
public:
    PreviewPane();
    ~PreviewPane();
    PreviewPane(const PreviewPane&) = delete;
    PreviewPane& operator=(const PreviewPane&) = delete;

    HRESULT Create(HWND parent, HINSTANCE instance);
    HWND Hwnd() const { return hwnd_; }

    // Shows `path` (empty clears the pane). Repeated calls with the same
    // path are ignored; rapid changes are coalesced.
    void Show(const std::wstring& path);
    // A document shown by its preview handler can be held open by it; let
    // go of it before the file is renamed, moved or deleted, and show it
    // again afterwards if it is still there.
    void ReleaseDocument();
    void RestoreDocument();

private:
    struct Shared {
        std::mutex mutex;
        HWND hwnd = nullptr;
        unsigned token = 0;   // latest request; older results are dropped
        HBITMAP bitmap = nullptr;  // premultiplied top-down 32bpp DIB
        UINT width = 0;
        UINT height = 0;
        unsigned bitmapToken = 0;
        std::wstring text;    // a text file's first lines (shown instead of a bitmap)
        bool hasText = false;
        bool failed = false;  // the latest request has no preview
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void OnPaint();
    void DrawHint(HDC dc, const RECT& rc, UINT id);
    void StartDecode();
    static void Decode(const std::shared_ptr<Shared>& shared, unsigned token, std::wstring path, UINT boxWidth,
                       UINT boxHeight) noexcept;

    HWND hwnd_ = nullptr;
    HFONT textFont_ = nullptr;
    UINT textFontDpi_ = 0;
    HFONT hintFont_ = nullptr;
    UINT hintFontDpi_ = 0;
    std::wstring path_;
    SIZE decodedFor_{};  // pane size of the last decode
    std::shared_ptr<Shared> shared_;
    std::unique_ptr<ThreadPool> pool_;
    // Office documents are shown live by their preview handler, like
    // Explorer's preview pane; this child covers the pane while one is shown.
    DocumentView documents_;
    std::wstring documentPath_;
    bool documentReleased_ = false;
    void ShowDocument(bool show);
};
