#include "core/ViewerCache.h"

#include <objbase.h>

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "core/ImageDecoder.h"

namespace {

struct ThreadCom {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ThreadCom() { if (SUCCEEDED(result)) CoUninitialize(); }
};

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// Does `image` serve a request for `maxSide`?
bool Satisfies(const ViewerImage& image, UINT maxSide) {
    if (image.IsFullSize()) return true;
    if (maxSide == 0) return false;
    return std::max(image.width, image.height) >= maxSide;
}

}  // namespace

struct ViewerCache::Shared {
    OnReady onReady;
    std::size_t capacity = 4;
    mutable std::mutex mutex;
    std::list<ViewerImagePtr> images;  // front = most recently used
    struct Pending {
        std::wstring path;
        UINT maxSide;
        ThreadPool::TaskId id;
    };
    std::vector<Pending> pending;
    std::vector<std::wstring> failed;
    std::vector<std::wstring> keep;  // from the last Retain

    void Touch(std::list<ViewerImagePtr>::iterator it) { images.splice(images.begin(), images, it); }

    ViewerImagePtr Find(const std::wstring& path, UINT maxSide) {
        // Prefer the smallest image that is good enough.
        auto best = images.end();
        for (auto it = images.begin(); it != images.end(); ++it) {
            if (!SamePath((*it)->path, path) || !Satisfies(**it, maxSide)) continue;
            if (best == images.end() || (*it)->pixels.size() < (*best)->pixels.size()) best = it;
        }
        if (best == images.end()) return nullptr;
        ViewerImagePtr image = *best;
        Touch(best);
        return image;
    }

    void Trim() {
        while (images.size() > capacity) {
            // Evict the least recently used picture that is not being kept.
            auto victim = images.end();
            for (auto it = images.end(); it != images.begin();) {
                --it;
                const bool kept = std::any_of(keep.begin(), keep.end(), [&](const std::wstring& p) {
                    return SamePath(p, (*it)->path);
                });
                if (!kept) {
                    victim = it;
                    break;
                }
            }
            if (victim == images.end()) victim = std::prev(images.end());
            images.erase(victim);
        }
    }
};

ViewerCache::ViewerCache(OnReady onReady, std::size_t capacity, std::size_t threads) noexcept
    : shared_(std::make_shared<Shared>()), pool_(std::make_unique<ThreadPool>(std::max<std::size_t>(1, threads))) {
    shared_->onReady = std::move(onReady);
    shared_->capacity = std::max<std::size_t>(1, capacity);
}

ViewerCache::~ViewerCache() {
    if (pool_) pool_->Shutdown();
}

ViewerImagePtr ViewerCache::Get(const std::wstring& path, UINT maxSide) const noexcept {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->Find(path, maxSide);
}

ViewerImagePtr ViewerCache::GetAny(const std::wstring& path) const noexcept {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    ViewerImagePtr best;
    for (const auto& image : shared_->images) {
        if (SamePath(image->path, path) && (!best || image->pixels.size() > best->pixels.size())) best = image;
    }
    return best;
}

bool ViewerCache::Failed(const std::wstring& path) const noexcept {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return std::any_of(shared_->failed.begin(), shared_->failed.end(),
                       [&](const std::wstring& p) { return SamePath(p, path); });
}

void ViewerCache::Request(const std::wstring& path, UINT maxSide, bool urgent) noexcept {
    try {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->Find(path, maxSide)) return;
        if (std::any_of(shared_->failed.begin(), shared_->failed.end(),
                        [&](const std::wstring& p) { return SamePath(p, path); })) {
            return;
        }
        for (auto& pending : shared_->pending) {
            if (!SamePath(pending.path, path)) continue;
            const bool enough = pending.maxSide == 0 || (maxSide != 0 && pending.maxSide >= maxSide);
            if (enough) {
                // Already queued; an urgent request moves it to the front.
                if (urgent && pool_->Cancel(pending.id)) {
                    const auto shared = shared_;
                    const UINT side = pending.maxSide;
                    const std::wstring p = pending.path;
                    pool_->Submit([shared, p, side] { Decode(shared, p, side); }, ThreadPool::Priority::High,
                                  &pending.id);
                }
                return;
            }
        }
        ThreadPool::TaskId id = 0;
        const auto shared = shared_;
        if (SUCCEEDED(pool_->Submit([shared, path, maxSide] { Decode(shared, path, maxSide); },
                                    urgent ? ThreadPool::Priority::High : ThreadPool::Priority::Low, &id))) {
            shared_->pending.push_back({path, maxSide, id});
        }
    } catch (...) {
    }
}

void ViewerCache::Retain(const std::vector<std::wstring>& keep) noexcept {
    try {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        auto& pending = shared_->pending;
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [&](const Shared::Pending& p) {
                                         const bool kept = std::any_of(keep.begin(), keep.end(), [&](const std::wstring& k) {
                                             return SamePath(k, p.path);
                                         });
                                         return !kept && pool_->Cancel(p.id);
                                     }),
                      pending.end());
        shared_->keep = keep;
        shared_->Trim();
    } catch (...) {
    }
}

void ViewerCache::Invalidate(const std::wstring& path) noexcept {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->images.remove_if([&](const ViewerImagePtr& image) { return SamePath(image->path, path); });
    auto& failed = shared_->failed;
    failed.erase(std::remove_if(failed.begin(), failed.end(), [&](const std::wstring& p) { return SamePath(p, path); }),
                 failed.end());
    auto& pending = shared_->pending;
    for (auto& p : pending) {
        if (SamePath(p.path, path)) pool_->Cancel(p.id);
    }
    pending.erase(std::remove_if(pending.begin(), pending.end(),
                                 [&](const Shared::Pending& p) { return SamePath(p.path, path); }),
                  pending.end());
}

void ViewerCache::Clear() noexcept {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    for (auto& p : shared_->pending) pool_->Cancel(p.id);
    shared_->pending.clear();
    shared_->images.clear();
    shared_->failed.clear();
}

void ViewerCache::Decode(const std::shared_ptr<Shared>& shared, std::wstring path, UINT maxSide) noexcept {
    HRESULT hr = S_OK;
    std::shared_ptr<ViewerImage> image;
    try {
        thread_local ThreadCom com;
        hr = (SUCCEEDED(com.result) || com.result == RPC_E_CHANGED_MODE) ? S_OK : com.result;
        DecodedImage decoded;
        if (SUCCEEDED(hr)) hr = ImageDecoder().Decode(path, maxSide, decoded);
        if (SUCCEEDED(hr)) {
            image = std::make_shared<ViewerImage>();
            image->path = path;
            image->width = decoded.width;
            image->height = decoded.height;
            image->originalWidth = decoded.originalWidth;
            image->originalHeight = decoded.originalHeight;
            image->maxSide = maxSide;
            image->pageCount = decoded.pageCount;
            const UINT stride = decoded.width * 4;
            image->pixels.resize(static_cast<std::size_t>(stride) * decoded.height);
            hr = decoded.bitmap->CopyPixels(nullptr, stride, static_cast<UINT>(image->pixels.size()),
                                            image->pixels.data());
        }
    } catch (const std::bad_alloc&) {
        hr = E_OUTOFMEMORY;
    } catch (...) {
        hr = E_FAIL;
    }
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        auto& pending = shared->pending;
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [&](const Shared::Pending& p) {
                                         return SamePath(p.path, path) && p.maxSide == maxSide;
                                     }),
                      pending.end());
        if (SUCCEEDED(hr) && image) {
            shared->images.push_front(std::move(image));
            shared->Trim();
        } else {
            shared->failed.push_back(path);
        }
    }
    if (shared->onReady) shared->onReady(path, hr);
}
