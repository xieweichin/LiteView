#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/ModelRenderer.h"

class ThreadPool;

// Viewer-mode page for 3D models: a turntable. The model spins slowly once
// loaded; dragging turns it, Ctrl+wheel and +/- zoom, the middle button or
// "/" resets the view. Loading and drawing run on a background thread with
// the software rasterizer; only the latest frame request is kept.
class ModelView {
public:
    struct Callbacks {
        std::function<void()> changed;         // load state or zoom changed
        std::function<void(int step)> navigate;  // wheel and Left/Right
        std::function<void()> exit;            // double-click
    };

    ModelView();
    ~ModelView();
    ModelView(const ModelView&) = delete;
    ModelView& operator=(const ModelView&) = delete;

    HRESULT Create(HWND parent, HINSTANCE instance, Callbacks callbacks);
    HWND Hwnd() const { return hwnd_; }

    void Show(const std::wstring& path);
    void Clear();
    void SetFullscreen(bool fullscreen);
    void OnThemeChanged();

    void ZoomIn();
    void ZoomOut();
    void ResetView();
    void Turn(int steps);  // 15 degrees per step, positive turns right

    bool Loading() const { return loading_; }
    bool Failed() const { return failed_; }
    std::size_t Triangles() const { return triangles_; }
    int ZoomPercent() const { return static_cast<int>(camera_.zoom * 100.0f + 0.5f); }

private:
    struct Shared {
        std::mutex mutex;
        HWND hwnd = nullptr;
        unsigned token = 0;  // current file
        // Load result
        unsigned loadedToken = 0;
        HRESULT result = S_OK;
        std::shared_ptr<const PreparedModel> model;
        // Last frame
        unsigned frameToken = 0;
        std::vector<std::uint32_t> pixels;  // opaque BGRA, top row first
        int width = 0;
        int height = 0;
        DWORD milliseconds = 0;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void OnLoaded();
    void OnFrame();
    void RequestFrame();
    void Paint();
    void StartSpin();
    void StopSpin();
    COLORREF Background() const;

    HWND hwnd_ = nullptr;
    Callbacks callbacks_;
    std::shared_ptr<Shared> shared_;
    std::unique_ptr<ThreadPool> pool_;
    std::shared_ptr<const PreparedModel> model_;
    ModelCamera camera_;
    std::vector<std::uint32_t> frame_;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
    bool loading_ = false;
    bool failed_ = false;
    bool fullscreen_ = false;
    bool rendering_ = false;     // a frame is being drawn
    bool frameDirty_ = false;    // another frame is wanted after it
    bool dragging_ = false;
    bool spinning_ = false;
    POINT dragLast_{};
    DWORD lastTick_ = 0;
    int wheelRemainder_ = 0;
    std::size_t triangles_ = 0;
};
