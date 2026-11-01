#include "doctest.h"

#include "core/FolderModel.h"
#include "core/FormatSupport.h"

#include <objbase.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

std::atomic<unsigned> folderSequence{0};

struct FolderFixture {
    std::wstring root;
    std::wstring first;
    std::wstring second;
    std::vector<std::wstring> files;
    std::vector<std::wstring> directories;

    FolderFixture() {
        wchar_t temp[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        REQUIRE(length > 0);
        REQUIRE(length < MAX_PATH);
        root = std::wstring(temp) + L"LiteView-資料夾-" + std::to_wstring(GetCurrentProcessId()) +
               L"-" + std::to_wstring(folderSequence.fetch_add(1));
        REQUIRE(CreateDirectoryW(root.c_str(), nullptr));
        first = root + L"\\第一個";
        second = root + L"\\second";
        REQUIRE(CreateDirectoryW(first.c_str(), nullptr));
        REQUIRE(CreateDirectoryW(second.c_str(), nullptr));
    }

    ~FolderFixture() {
        for (const auto& file : files) DeleteFileW(file.c_str());
        for (const auto& directory : directories) RemoveDirectoryW(directory.c_str());
        RemoveDirectoryW(first.c_str());
        RemoveDirectoryW(second.c_str());
        RemoveDirectoryW(root.c_str());
    }

    void AddFile(const std::wstring& directory, const std::wstring& name,
                 DWORD size, std::uint64_t modified) {
        const std::wstring path = directory + L"\\" + name;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(file != INVALID_HANDLE_VALUE);
        files.push_back(path);
        const std::vector<char> bytes(size, 'x');
        DWORD written = 0;
        const BOOL wrote = size == 0 || WriteFile(file, bytes.data(), size, &written, nullptr);
        FILETIME time{static_cast<DWORD>(modified), static_cast<DWORD>(modified >> 32)};
        const BOOL setTime = SetFileTime(file, nullptr, nullptr, &time);
        CloseHandle(file);
        REQUIRE(wrote);
        REQUIRE(written == size);
        REQUIRE(setTime);
    }

    void AddDirectory(const std::wstring& path) {
        REQUIRE(CreateDirectoryW(path.c_str(), nullptr));
        directories.push_back(path);
    }
};

struct Notifications {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::uint32_t> generations;

    void Signal(std::uint32_t generation) {
        std::lock_guard<std::mutex> lock(mutex);
        generations.push_back(generation);
        changed.notify_all();
    }
    bool Wait(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return generations.size() >= count; });
    }
};

struct Gate {
    std::promise<void> release;
    std::shared_future<void> ready = release.get_future().share();
    bool opened = false;
    void Open() { if (!opened) { opened = true; release.set_value(); } }
    ~Gate() { Open(); }
};

}  // namespace

TEST_CASE("folder listing filters WIC extensions and sorts in the background") {
    FolderFixture fixture;
    constexpr std::uint64_t baseTime = 132537600000000000ULL;
    fixture.AddFile(fixture.first, L"img2.JPG", 3, baseTime + 30000000);
    fixture.AddFile(fixture.first, L"img10.jpg", 1, baseTime + 10000000);
    fixture.AddFile(fixture.first, L"alpha.png", 2, baseTime + 20000000);
    fixture.AddFile(fixture.first, L"ignore.txt", 4, baseTime + 40000000);
    fixture.AddDirectory(fixture.first + L"\\folder.jpg");

    Notifications notifications;
    FolderModel model;
    const auto generation = model.Open(fixture.first, [&](std::uint32_t value) { notifications.Signal(value); });
    REQUIRE(generation != 0);
    REQUIRE(notifications.Wait(1));
    REQUIRE(model.Commit(generation) == S_OK);
    REQUIRE(model.Count() == 3);
    CHECK(model.At(0).name == L"alpha.png");
    CHECK(model.At(1).name == L"img2.JPG");
    CHECK(model.At(2).name == L"img10.jpg");
    CHECK(model.FullPath(1) == fixture.first + L"\\img2.JPG");
    CHECK(model.At(1).id != model.At(2).id);

    model.Sort(SortKey::Size, true);
    REQUIRE(notifications.Wait(2));
    REQUIRE(model.Commit(generation) == S_OK);
    CHECK(model.At(0).name == L"img10.jpg");
    CHECK(model.At(1).name == L"alpha.png");
    CHECK(model.At(2).name == L"img2.JPG");

    model.Sort(SortKey::Type, true);
    REQUIRE(notifications.Wait(3));
    REQUIRE(model.Commit(generation) == S_OK);
    CHECK(model.At(0).name == L"img2.JPG");
    CHECK(model.At(1).name == L"img10.jpg");
    CHECK(model.At(2).name == L"alpha.png");

    model.Sort(SortKey::Modified, true);
    REQUIRE(notifications.Wait(4));
    REQUIRE(model.Commit(generation) == S_OK);
    CHECK(model.At(0).name == L"img10.jpg");
    CHECK(model.At(1).name == L"alpha.png");
    CHECK(model.At(2).name == L"img2.JPG");

    model.Sort(SortKey::Name, false);
    REQUIRE(notifications.Wait(5));
    REQUIRE(model.Commit(generation) == S_OK);
    CHECK(model.At(0).name == L"img10.jpg");
    CHECK(model.At(1).name == L"img2.JPG");
    CHECK(model.At(2).name == L"alpha.png");
}

TEST_CASE("files that need a missing codec extension are counted, not listed") {
    FolderFixture fixture;
    fixture.AddFile(fixture.first, L"a.jpg", 1, 132537600000000000ULL);
    fixture.AddFile(fixture.first, L"b.heic", 1, 132537600000000000ULL);
    fixture.AddFile(fixture.first, L"c.CR3", 1, 132537600000000000ULL);
    fixture.AddFile(fixture.first, L"d.txt", 1, 132537600000000000ULL);

    // Expectations depend on the codecs installed on the test machine.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<std::wstring> extensions;
    REQUIRE(DecoderExtensions(extensions) == S_OK);
    if (SUCCEEDED(init)) CoUninitialize();
    const bool heic = HasExtension(extensions, L".heic");
    const bool cr3 = HasExtension(extensions, L".cr3");

    Notifications notifications;
    FolderModel model;
    const auto generation = model.Open(fixture.first, [&](std::uint32_t value) { notifications.Signal(value); });
    REQUIRE(generation != 0);
    REQUIRE(notifications.Wait(1));
    REQUIRE(model.Commit(generation) == S_OK);
    CHECK(model.Count() == 1u + (heic ? 1u : 0u) + (cr3 ? 1u : 0u));
    CodecGap gap = model.MissingCodecs();
    CHECK(gap.files == (heic ? 0u : 1u) + (cr3 ? 0u : 1u));
    CHECK(((gap.packs & CodecPackBit(CodecPack::Heif)) != 0) == !heic);
    CHECK(((gap.packs & CodecPackBit(CodecPack::Raw)) != 0) == !cr3);
    CHECK((gap.packs & CodecPackBit(CodecPack::WebP)) == 0);

    // Re-sorting keeps the count.
    model.Sort(SortKey::Size, true);
    REQUIRE(notifications.Wait(2));
    REQUIRE(model.Commit(generation) == S_OK);
    const CodecGap sorted = model.MissingCodecs();
    CHECK(sorted.files == gap.files);
    CHECK(sorted.packs == gap.packs);
}

TEST_CASE("opening a newer folder suppresses an older queued callback") {
    FolderFixture fixture;
    fixture.AddFile(fixture.first, L"old.jpg", 1, 132537600000000000ULL);
    fixture.AddFile(fixture.second, L"new.jpg", 1, 132537600000000000ULL);
    auto pool = std::make_shared<ThreadPool>(1);
    Gate gate;
    std::promise<void> started;
    auto startedFuture = started.get_future();
    REQUIRE(pool->Submit([&] { started.set_value(); gate.ready.wait(); }) == S_OK);
    REQUIRE(startedFuture.wait_for(5s) == std::future_status::ready);

    Notifications notifications;
    std::atomic<unsigned> oldCallbacks{0};
    FolderModel model(pool);
    const auto oldGeneration = model.Open(fixture.first, [&](std::uint32_t) { ++oldCallbacks; });
    const auto newGeneration = model.Open(fixture.second, [&](std::uint32_t value) { notifications.Signal(value); });
    REQUIRE(oldGeneration != 0);
    REQUIRE(newGeneration > oldGeneration);
    gate.Open();
    REQUIRE(notifications.Wait(1));
    CHECK(oldCallbacks == 0);
    CHECK(model.Commit(oldGeneration) == HRESULT_FROM_WIN32(ERROR_CANCELLED));
    REQUIRE(model.Commit(newGeneration) == S_OK);
    REQUIRE(model.Count() == 1);
    CHECK(model.At(0).name == L"new.jpg");
}

TEST_CASE("an inaccessible folder reports its failure on commit") {
    FolderFixture fixture;
    Notifications notifications;
    FolderModel model;
    const auto generation = model.Open(fixture.root + L"\\does-not-exist",
        [&](std::uint32_t value) { notifications.Signal(value); });
    REQUIRE(generation != 0);
    REQUIRE(notifications.Wait(1));
    CHECK(FAILED(model.Commit(generation)));
    CHECK(FAILED(model.LastResult()));
    CHECK(model.Count() == 0);
}

TEST_CASE("the latest sort request wins while the worker is busy") {
    FolderFixture fixture;
    fixture.AddFile(fixture.first, L"img2.jpg", 3, 132537600000000000ULL);
    fixture.AddFile(fixture.first, L"img10.jpg", 1, 132537600000000000ULL);
    auto pool = std::make_shared<ThreadPool>(1);
    Notifications notifications;
    FolderModel model(pool);
    const auto generation = model.Open(fixture.first, [&](std::uint32_t value) { notifications.Signal(value); });
    REQUIRE(generation != 0);
    REQUIRE(notifications.Wait(1));
    REQUIRE(model.Commit(generation) == S_OK);

    Gate gate;
    std::promise<void> started;
    auto startedFuture = started.get_future();
    REQUIRE(pool->Submit([&] { started.set_value(); gate.ready.wait(); }) == S_OK);
    REQUIRE(startedFuture.wait_for(5s) == std::future_status::ready);
    model.Sort(SortKey::Size, true);
    model.Sort(SortKey::Name, false);
    gate.Open();
    REQUIRE(notifications.Wait(2));
    REQUIRE(model.Commit(generation) == S_OK);
    REQUIRE(model.Count() == 2);
    CHECK(model.At(0).name == L"img10.jpg");
    CHECK(model.At(1).name == L"img2.jpg");
}

TEST_CASE("a completion callback can safely open another folder") {
    FolderFixture fixture;
    fixture.AddFile(fixture.first, L"old.jpg", 1, 132537600000000000ULL);
    fixture.AddFile(fixture.second, L"new.jpg", 1, 132537600000000000ULL);
    Notifications notifications;
    std::atomic<std::uint32_t> secondGeneration{0};
    FolderModel model(std::make_shared<ThreadPool>(1));
    const auto firstGeneration = model.Open(fixture.first, [&](std::uint32_t) {
        secondGeneration = model.Open(fixture.second, [&](std::uint32_t value) { notifications.Signal(value); });
    });
    REQUIRE(firstGeneration != 0);
    REQUIRE(notifications.Wait(1));
    REQUIRE(secondGeneration > firstGeneration);
    REQUIRE(model.Commit(secondGeneration) == S_OK);
    REQUIRE(model.Count() == 1);
    CHECK(model.At(0).name == L"new.jpg");
}

TEST_CASE("destroying the model suppresses a queued callback") {
    FolderFixture fixture;
    auto pool = std::make_shared<ThreadPool>(1);
    Gate gate;
    std::promise<void> started;
    auto startedFuture = started.get_future();
    REQUIRE(pool->Submit([&] { started.set_value(); gate.ready.wait(); }) == S_OK);
    REQUIRE(startedFuture.wait_for(5s) == std::future_status::ready);
    std::atomic<unsigned> callbacks{0};
    {
        FolderModel model(pool);
        REQUIRE(model.Open(fixture.first, [&](std::uint32_t) { ++callbacks; }) != 0);
    }
    std::promise<void> drained;
    auto drainedFuture = drained.get_future();
    REQUIRE(pool->Submit([&] { drained.set_value(); }) == S_OK);
    gate.Open();
    REQUIRE(drainedFuture.wait_for(5s) == std::future_status::ready);
    CHECK(callbacks == 0);
}
