#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/ThreadPool.h"

struct FileItem {
    std::wstring name;
    std::uint64_t size = 0;
    FILETIME modified{};
    std::uint32_t id = 0;
};

// Files in a folder that are a known image format whose Windows codec
// extension is not installed (see core/FormatSupport.h). They are not listed.
struct CodecGap {
    std::size_t files = 0;
    std::uint32_t packs = 0;  // CodecPackBit flags
};

enum class SortKey { Name, Size, Type, Modified };

// Open and Sort perform filesystem work and sorting on worker threads. The
// callback is also called on a worker under an internal lock and must return
// promptly. A UI must post a notification (not synchronously send a message)
// and call Commit on its own thread. Sort completes asynchronously and invokes
// the same callback with the same generation; commit again after that callback.
// Count, At, FullPath,
// and Commit must be used from that same consumer thread. An At reference is
// valid only until the next successful Commit or destruction of this model.
class FolderModel {
public:
    using OnLoaded = std::function<void(std::uint32_t generation)>;

    explicit FolderModel(std::shared_ptr<ThreadPool> pool = nullptr) noexcept;
    ~FolderModel();
    FolderModel(const FolderModel&) = delete;
    FolderModel& operator=(const FolderModel&) = delete;

    // Returns zero if the request could not be queued; see LastResult().
    std::uint32_t Open(const std::wstring& path, OnLoaded callback) noexcept;
    void Sort(SortKey key, bool ascending) noexcept;
    HRESULT Commit(std::uint32_t generation) noexcept;
    HRESULT LastResult() const noexcept;

    CodecGap MissingCodecs() const noexcept;  // of the committed folder
    std::size_t Count() const noexcept;
    const FileItem& At(std::size_t index) const noexcept;
    std::wstring FullPath(std::size_t index) const noexcept;

private:
    struct State;
    static void Load(const std::shared_ptr<State>& state, std::wstring path,
                     std::uint32_t generation) noexcept;
    static void SortCopy(const std::shared_ptr<State>& state,
                         std::shared_ptr<const std::vector<FileItem>> source,
                         std::wstring path, std::uint32_t generation, CodecGap gap) noexcept;
    static void Finish(const std::shared_ptr<State>& state, std::wstring path,
                       std::uint32_t generation, HRESULT result,
                       std::vector<FileItem> items, CodecGap gap) noexcept;

    std::shared_ptr<State> state_;
    std::shared_ptr<ThreadPool> pool_;
};
