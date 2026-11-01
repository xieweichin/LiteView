#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/ThreadPool.h"

// A finished thumbnail: a top-down 32bpp DIB section, opaque (transparent
// areas are flattened onto white), long side <= the requested size.
struct Thumbnail {
    HBITMAP bitmap = nullptr;
    UINT width = 0;
    UINT height = 0;
    Thumbnail() = default;
    Thumbnail(const Thumbnail&) = delete;
    Thumbnail& operator=(const Thumbnail&) = delete;
    ~Thumbnail() { if (bitmap) DeleteObject(bitmap); }
};
using ThumbnailPtr = std::shared_ptr<const Thumbnail>;

struct ThumbnailRequest {
    std::uint32_t generation = 0;  // echoed back to OnReady
    std::uint32_t itemId = 0;      // echoed back to OnReady
    std::wstring path;
    FILETIME modified{};           // part of the cache key: a changed file gets a new thumbnail
    std::uint64_t fileSize = 0;
    UINT size = 128;               // long side in pixels
};

// Produces thumbnails on background threads. Lookup order: memory LRU,
// disk cache, Windows shell thumbnail (IShellItemImageFactory), WIC decode.
// WIC results are written to the disk cache as 85% JPEG files named by a
// hash of path, modified time, file size and thumbnail size; the cache is
// pruned to its byte limit by deleting the least recently used files.
// The Windows shell's own thumbnail of a file (Office, video...), long side
// <= size; fails when the file type has no thumbnail provider. COM must be
// initialized on the calling thread.
HRESULT ShellThumbnailImage(const std::wstring& path, UINT size, ThumbnailPtr& out) noexcept;

class ThumbnailService {
public:
    // Called on a worker thread; post a message and return.
    using OnReady = std::function<void(std::uint32_t generation, std::uint32_t itemId, HRESULT result)>;

    struct Options {
        std::wstring cacheDirectory;  // empty: DefaultCacheDirectory(), resolved on a worker
        std::uint64_t diskLimitBytes = 200ull * 1024 * 1024;
        std::size_t memoryLimitBytes = 96u * 1024 * 1024;
        std::size_t threadCount = 0;  // 0: ThreadPool::DefaultThreadCount()
        bool useShellThumbnails = true;
    };

    struct Stats {
        std::atomic<std::uint32_t> memoryHits{0};
        std::atomic<std::uint32_t> diskHits{0};
        std::atomic<std::uint32_t> shellHits{0};
        std::atomic<std::uint32_t> decodes{0};
        std::atomic<std::uint32_t> failures{0};
    };

    ThumbnailService(OnReady onReady, Options options) noexcept;
    ~ThumbnailService();
    ThumbnailService(const ThumbnailService&) = delete;
    ThumbnailService& operator=(const ThumbnailService&) = delete;

    // Queues the request unless it is cached, already queued, or known to
    // fail. Visible items jump ahead of prefetch work.
    void Request(const ThumbnailRequest& request, bool visible) noexcept;

    // Memory-cache lookup for painting. `failed` is set when the image is
    // known not to decode, so the caller keeps the file-type icon.
    ThumbnailPtr TryGetCached(const ThumbnailRequest& request, bool* failed = nullptr) const noexcept;

    // The file at request.path changed (rotated): forget its thumbnails in
    // memory and the disk-cache file for this request's key, and stop using
    // the shell thumbnail for it, since the shell may still have the old one.
    void Invalidate(const ThumbnailRequest& request) noexcept;

    // Drops every request that has not started (scrolling, folder change).
    void CancelQueued() noexcept;

    // Deletes least recently used cache files until the cache fits the
    // limit. Runs on a worker after start-up and periodically; public for tests.
    HRESULT PruneDiskCache() noexcept;

    const Stats& GetStats() const noexcept { return stats_; }
    std::wstring CacheDirectory() const;

    // %LOCALAPPDATA%\LiteView\thumbs, or <exe folder>\thumbs in portable mode.
    static std::wstring DefaultCacheDirectory();
    static std::wstring CacheKey(const ThumbnailRequest& request);

private:
    struct Shared;
    static void Work(const std::shared_ptr<Shared>& shared, ThumbnailRequest request, std::wstring key) noexcept;

    std::shared_ptr<Shared> shared_;
    Stats& stats_;
    std::unique_ptr<ThreadPool> pool_;
};
