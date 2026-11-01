#include "doctest.h"

#include "core/ViewerCache.h"

#include <objbase.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::wstring Fixture(const std::wstring& name) {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    std::wstring executable(path.data(), length);
    return executable.substr(0, executable.find_last_of(L"\\/")) + L"\\fixtures\\" + name;
}

struct Waiter {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::pair<std::wstring, HRESULT>> results;
    ViewerCache::OnReady Callback() {
        return [this](const std::wstring& path, HRESULT hr) {
            std::lock_guard<std::mutex> lock(mutex);
            results.emplace_back(path, hr);
            cv.notify_all();
        };
    }
    bool WaitFor(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(10), [&] { return results.size() >= count; });
    }
};

}  // namespace

TEST_CASE("viewer cache decodes at screen size and upgrades to full size on request") {
    Waiter waiter;
    ViewerCache cache(waiter.Callback());
    const std::wstring path = Fixture(L"basic.bmp");
    cache.Request(path, 8, true);
    REQUIRE(waiter.WaitFor(1));
    CHECK(waiter.results[0].second == S_OK);

    ViewerImagePtr reduced = cache.Get(path, 8);
    REQUIRE(static_cast<bool>(reduced));
    CHECK(std::max(reduced->width, reduced->height) <= 8u);
    CHECK(reduced->pixels.size() == static_cast<std::size_t>(reduced->width) * reduced->height * 4);
    const bool alreadyFull = reduced->IsFullSize();

    // The reduced decode does not satisfy a full-size request unless the picture is tiny.
    CHECK(static_cast<bool>(cache.Get(path, 0)) == alreadyFull);
    cache.Request(path, 0, true);
    if (!alreadyFull) REQUIRE(waiter.WaitFor(2));
    ViewerImagePtr full = cache.Get(path, 0);
    REQUIRE(static_cast<bool>(full));
    CHECK(full->IsFullSize());
    CHECK(full->width == reduced->originalWidth);
    // GetAny prefers the sharpest picture.
    CHECK(cache.GetAny(path)->pixels.size() == full->pixels.size());
}

TEST_CASE("viewer cache remembers failures until invalidated") {
    Waiter waiter;
    ViewerCache cache(waiter.Callback());
    const std::wstring path = Fixture(L"broken.jpg");
    cache.Request(path, 64, true);
    REQUIRE(waiter.WaitFor(1));
    CHECK(FAILED(waiter.results[0].second));
    CHECK(cache.Failed(path));
    cache.Request(path, 64, true);  // ignored
    Sleep(200);
    {
        std::lock_guard<std::mutex> lock(waiter.mutex);
        CHECK(waiter.results.size() == 1);
    }
    cache.Invalidate(path);
    CHECK_FALSE(cache.Failed(path));
}

TEST_CASE("viewer cache keeps the retained pictures when over capacity") {
    Waiter waiter;
    ViewerCache cache(waiter.Callback(), 2, 1);
    const std::wstring a = Fixture(L"orientation-1.jpg");
    const std::wstring b = Fixture(L"orientation-2.jpg");
    const std::wstring c = Fixture(L"orientation-3.jpg");
    cache.Request(a, 16, true);
    REQUIRE(waiter.WaitFor(1));
    cache.Request(b, 16, true);
    REQUIRE(waiter.WaitFor(2));
    cache.Get(a, 16);  // a is now more recent than b
    cache.Request(c, 16, true);
    REQUIRE(waiter.WaitFor(3));
    // Capacity 2: the least recently used (b) went away.
    CHECK(static_cast<bool>(cache.Get(a, 16)));
    CHECK_FALSE(static_cast<bool>(cache.Get(b, 16)));
    CHECK(static_cast<bool>(cache.Get(c, 16)));

    cache.Retain({c});
    cache.Request(b, 16, true);
    REQUIRE(waiter.WaitFor(4));
    CHECK(static_cast<bool>(cache.Get(c, 16)));
    CHECK(static_cast<bool>(cache.Get(b, 16)));
}
