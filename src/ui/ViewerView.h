#pragma once

#include <windows.h>
#include <d2d1.h>
#include <wrl/client.h>

#include <functional>
#include <string>

#include "core/ThumbnailService.h"
#include "core/ViewerCache.h"

// Single-picture view drawn with Direct2D. Default is "fit to window, never
// enlarge"; zooming is anchored at the mouse (or the centre for keys) and a
// zoomed picture is dragged with the left button.
class ViewerView {
public:
    struct Callbacks {
        std::function<void(int step)> navigate;  // +1 next, -1 previous (wheel, arrows)
        std::function<void()> exit;              // double-click
        std::function<void()> needFullSize;      // zoomed past the decoded resolution
        std::function<void()> viewChanged;       // zoom changed: refresh the status bar
    };

    ViewerView() = default;
    ~ViewerView();
    ViewerView(const ViewerView&) = delete;
    ViewerView& operator=(const ViewerView&) = delete;

    HRESULT Create(HWND parent, HINSTANCE instance, Callbacks callbacks);
    HWND Hwnd() const { return hwnd_; }

    // Shows `image` for `path`. Until it is available, `placeholder` (a
    // thumbnail) is drawn enlarged. Passing the same path again keeps the
    // zoom and position, so a sharper decode can replace a smaller one.
    void SetImage(const std::wstring& path, ViewerImagePtr image, ThumbnailPtr placeholder, bool failed);
    void Clear();
    void SetFullscreen(bool fullscreen);

    void ZoomIn();
    void ZoomOut();
    void ToggleFit();

    bool HasImage() const { return static_cast<bool>(image_); }
    UINT ImageWidth() const { return image_ ? image_->originalWidth : 0; }
    UINT ImageHeight() const { return image_ ? image_->originalHeight : 0; }
    double EffectiveScale() const;
    bool NeedsFullSize() const;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void OnPaint();
    HRESULT EnsureTarget();
    void DiscardTarget();
    HRESULT EnsureBitmap();
    double FitScale() const;
    void ZoomTo(double scale, POINT anchor);
    void ClampCenter();
    void AfterViewChange();
    SIZE ViewSize() const;

    HWND hwnd_ = nullptr;
    Callbacks callbacks_;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap_;
    const void* bitmapSource_ = nullptr;  // ViewerImage or Thumbnail the bitmap was made from

    std::wstring path_;
    ViewerImagePtr image_;
    ThumbnailPtr placeholder_;
    bool failed_ = false;
    bool fullscreen_ = false;

    bool fit_ = true;
    double scale_ = 1.0;       // screen pixels per original pixel when !fit_
    double centerX_ = 0.0;     // original-image point at the view centre
    double centerY_ = 0.0;
    bool dragging_ = false;
    POINT dragLast_{};
    int wheelRemainder_ = 0;
};
