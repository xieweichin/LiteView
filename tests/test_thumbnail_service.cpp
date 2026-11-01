#include "doctest.h"

#include "core/ThumbnailService.h"

#include <objbase.h>

#include <algorithm>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

std::wstring ExeDir() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    std::wstring executable(path.data(), length);
    return executable.substr(0, executable.find_last_of(L"\\/"));
}

std::wstring Fixture(const std::wstring& name) { return ExeDir() + L"\\fixtures\\" + name; }

struct TempDir {
    std::wstring path;
    TempDir() {
        wchar_t base[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, base);
        path = std::wstring(base) + L"LiteViewThumbs-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(GetTickCount64());
        CreateDirectoryW(path.c_str(), nullptr);
    }
    ~TempDir() {
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((path + L"\\" + data.cFileName).c_str());
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        RemoveDirectoryW(path.c_str());
    }
    std::size_t CountFiles() const {
        std::size_t count = 0;
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW((path + L"\\*.jpg").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) return 0;
        do ++count; while (FindNextFileW(find, &data));
        FindClose(find);
        return count;
    }
};

struct Waiter {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<HRESULT> results;
    ThumbnailService::OnReady Callback() {
        return [this](std::uint32_t, std::uint32_t, HRESULT hr) {
            std::lock_guard<std::mutex> lock(mutex);
            results.push_back(hr);
            cv.notify_all();
        };
    }
    bool WaitFor(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(10), [&] { return results.size() >= count; });
    }
};

ThumbnailRequest RequestFor(const std::wstring& path, UINT size) {
    ThumbnailRequest request;
    request.path = path;
    request.size = size;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    REQUIRE(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data));
    request.modified = data.ftLastWriteTime;
    request.fileSize = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return request;
}

ThumbnailService::Options Options(const std::wstring& dir) {
    ThumbnailService::Options options;
    options.cacheDirectory = dir;
    options.threadCount = 2;
    options.useShellThumbnails = false;  // exercise the WIC path deterministically
    return options;
}

}  // namespace

TEST_CASE("thumbnails are decoded, fit the requested size and land in the disk cache") {
    ComApartment apartment;
    TempDir dir;
    const ThumbnailRequest request = RequestFor(Fixture(L"orientation-1.jpg"), 32);
    {
        Waiter waiter;
        ThumbnailService service(waiter.Callback(), Options(dir.path));
        service.Request(request, true);
        REQUIRE(waiter.WaitFor(1));
        CHECK(waiter.results[0] == S_OK);
        ThumbnailPtr thumbnail = service.TryGetCached(request);
        REQUIRE(static_cast<bool>(thumbnail));
        CHECK(thumbnail->bitmap != nullptr);
        CHECK(std::max(thumbnail->width, thumbnail->height) <= 32u);
        CHECK(std::max(thumbnail->width, thumbnail->height) >= 31u);
        CHECK(service.GetStats().decodes == 1);
        // Asking again is answered from memory without new work.
        service.Request(request, true);
        CHECK(service.GetStats().decodes == 1);
    }
    CHECK(dir.CountFiles() == 1);

    // A fresh service (empty memory cache) is served from disk.
    Waiter waiter;
    ThumbnailService service(waiter.Callback(), Options(dir.path));
    service.Request(request, true);
    REQUIRE(waiter.WaitFor(1));
    CHECK(waiter.results[0] == S_OK);
    CHECK(service.GetStats().diskHits == 1);
    CHECK(service.GetStats().decodes == 0);
    CHECK(static_cast<bool>(service.TryGetCached(request)));
}

TEST_CASE("a changed modified time or size makes a different cache key") {
    ThumbnailRequest a;
    a.path = L"C:\\Photos\\a.jpg";
    a.modified = FILETIME{1, 2};
    a.fileSize = 100;
    a.size = 128;
    ThumbnailRequest b = a;
    b.path = L"c:\\photos\\A.JPG";
    CHECK(ThumbnailService::CacheKey(a) == ThumbnailService::CacheKey(b));  // paths are case-insensitive
    b.modified = FILETIME{2, 2};
    CHECK(ThumbnailService::CacheKey(a) != ThumbnailService::CacheKey(b));
    b = a;
    b.fileSize = 101;
    CHECK(ThumbnailService::CacheKey(a) != ThumbnailService::CacheKey(b));
    b = a;
    b.size = 256;
    CHECK(ThumbnailService::CacheKey(a) != ThumbnailService::CacheKey(b));
    CHECK(ThumbnailService::CacheKey(a).size() == 16);
}

TEST_CASE("undecodable files are remembered as failures") {
    ComApartment apartment;
    TempDir dir;
    Waiter waiter;
    ThumbnailService service(waiter.Callback(), Options(dir.path));
    const ThumbnailRequest request = RequestFor(Fixture(L"broken.jpg"), 64);
    service.Request(request, true);
    REQUIRE(waiter.WaitFor(1));
    CHECK(FAILED(waiter.results[0]));
    bool failed = false;
    CHECK(!service.TryGetCached(request, &failed));
    CHECK(failed);
    service.Request(request, true);  // not queued again
    Sleep(200);
    {
        std::lock_guard<std::mutex> lock(waiter.mutex);
        CHECK(waiter.results.size() == 1);
    }
    CHECK(service.GetStats().failures == 1);
}

TEST_CASE("transparent pixels are flattened onto white") {
    ComApartment apartment;
    TempDir dir;
    Waiter waiter;
    ThumbnailService service(waiter.Callback(), Options(dir.path));
    const ThumbnailRequest request = RequestFor(Fixture(L"alpha.png"), 64);
    service.Request(request, true);
    REQUIRE(waiter.WaitFor(1));
    ThumbnailPtr thumbnail = service.TryGetCached(request);
    REQUIRE(static_cast<bool>(thumbnail));
    DIBSECTION dib{};
    REQUIRE(GetObjectW(thumbnail->bitmap, sizeof(dib), &dib) == sizeof(dib));
    const auto* pixels = static_cast<const BYTE*>(dib.dsBm.bmBits);
    const std::size_t count = static_cast<std::size_t>(thumbnail->width) * thumbnail->height;
    for (std::size_t i = 0; i < count; ++i) REQUIRE(pixels[i * 4 + 3] == 255);
}

TEST_CASE("pruning deletes the least recently used files down to the limit") {
    TempDir dir;
    // Ten 10 KB files, oldest first.
    std::vector<char> data(10 * 1024, 'x');
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER base;
    base.LowPart = time.dwLowDateTime;
    base.HighPart = time.dwHighDateTime;
    for (int i = 0; i < 10; ++i) {
        const std::wstring name = dir.path + L"\\" + std::to_wstring(i) + L".jpg";
        HANDLE file = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(file != INVALID_HANDLE_VALUE);
        DWORD written = 0;
        WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
        ULARGE_INTEGER stamp;
        stamp.QuadPart = base.QuadPart - static_cast<ULONGLONG>(10 - i) * 10000000ull * 60;  // i minutes apart
        FILETIME ft{stamp.LowPart, stamp.HighPart};
        SetFileTime(file, nullptr, nullptr, &ft);
        CloseHandle(file);
    }
    ThumbnailService::Options options = Options(dir.path);
    options.diskLimitBytes = 50 * 1024;
    ThumbnailService service(nullptr, options);
    REQUIRE(SUCCEEDED(service.PruneDiskCache()));
    // Down to 90% of 50 KB: four files remain, and they are the newest.
    CHECK(dir.CountFiles() == 4);
    CHECK(GetFileAttributesW((dir.path + L"\\0.jpg").c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW((dir.path + L"\\9.jpg").c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW((dir.path + L"\\6.jpg").c_str()) != INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("the memory cache stays within its byte limit") {
    ComApartment apartment;
    TempDir dir;
    Waiter waiter;
    ThumbnailService::Options options = Options(dir.path);
    options.memoryLimitBytes = 64 * 64 * 4 * 2;  // about two 64-px thumbnails
    ThumbnailService service(waiter.Callback(), options);
    const wchar_t* names[] = {L"orientation-1.jpg", L"orientation-2.jpg", L"orientation-3.jpg", L"basic.bmp"};
    for (const wchar_t* name : names) service.Request(RequestFor(Fixture(name), 64), true);
    REQUIRE(waiter.WaitFor(4));
    int cached = 0;
    for (const wchar_t* name : names) cached += service.TryGetCached(RequestFor(Fixture(name), 64)) ? 1 : 0;
    CHECK(cached >= 1);
    CHECK(cached <= 3);
}

TEST_CASE("invalidating a file drops its memory and disk thumbnails") {
    ComApartment apartment;
    TempDir dir;
    Waiter waiter;
    ThumbnailService service(waiter.Callback(), Options(dir.path));
    const ThumbnailRequest request = RequestFor(Fixture(L"orientation-1.jpg"), 32);
    service.Request(request, true);
    REQUIRE(waiter.WaitFor(1));
    REQUIRE(static_cast<bool>(service.TryGetCached(request)));
    REQUIRE(dir.CountFiles() == 1);
    service.Invalidate(request);
    CHECK(!service.TryGetCached(request));
    CHECK(dir.CountFiles() == 0);
    service.Request(request, true);
    REQUIRE(waiter.WaitFor(2));
    CHECK(service.GetStats().decodes == 2);
}
