#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/ThreadPool.h"

// A decoded picture ready to upload to the screen: top-down 32bpp
// premultiplied BGRA, already turned upright by EXIF orientation.
struct ViewerImage {
    std::wstring path;
    UINT width = 0;            // decoded size
    UINT height = 0;
    UINT originalWidth = 0;    // upright full size
    UINT originalHeight = 0;
    UINT maxSide = 0;          // what was asked for; 0 = original size
    UINT pageCount = 0;        // PDF documents only
    std::vector<BYTE> pixels;  // stride = width * 4

    bool IsFullSize() const { return width == originalWidth && height == originalHeight; }
};
using ViewerImagePtr = std::shared_ptr<const ViewerImage>;

// Decodes the current picture and its neighbours in the background and keeps
// the last few. Large pictures are decoded at screen size first; the original
// size is only decoded when the viewer zooms past that.
class ViewerCache {
public:
    // Called on a worker after a decode finished (or failed); post and return.
    using OnReady = std::function<void(const std::wstring& path, HRESULT result)>;

    explicit ViewerCache(OnReady onReady, std::size_t capacity = 4, std::size_t threads = 2) noexcept;
    ~ViewerCache();
    ViewerCache(const ViewerCache&) = delete;
    ViewerCache& operator=(const ViewerCache&) = delete;

    // Best cached image of `path` that is at least `maxSide` on its long side
    // (or full size). maxSide 0 asks for the original size.
    ViewerImagePtr Get(const std::wstring& path, UINT maxSide) const noexcept;
    // Any cached image of `path`, the largest first; for showing something
    // while a sharper version decodes.
    ViewerImagePtr GetAny(const std::wstring& path) const noexcept;
    // True if the last decode of `path` failed.
    bool Failed(const std::wstring& path) const noexcept;

    // Queues a decode unless a suitable image is cached or queued.
    void Request(const std::wstring& path, UINT maxSide, bool urgent) noexcept;
    // Drops queued work for pictures not in `keep` and evicts cached pictures
    // not in `keep` once the cache is over capacity.
    void Retain(const std::vector<std::wstring>& keep) noexcept;
    // Forgets everything about `path` (it changed on disk).
    void Invalidate(const std::wstring& path) noexcept;
    void Clear() noexcept;

private:
    struct Shared;
    static void Decode(const std::shared_ptr<Shared>& shared, std::wstring path, UINT maxSide) noexcept;

    std::shared_ptr<Shared> shared_;
    std::unique_ptr<ThreadPool> pool_;
};
