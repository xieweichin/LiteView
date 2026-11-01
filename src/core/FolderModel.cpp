#include "core/FolderModel.h"

#include "core/ModelLoader.h"

#include "core/FormatSupport.h"
#include "core/TextDocument.h"

#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace {

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

struct FindHandle {
    explicit FindHandle(HANDLE handle) : handle(handle) {}
    ~FindHandle() { if (handle != INVALID_HANDLE_VALUE) FindClose(handle); }
    HANDLE handle;
};

std::wstring Join(const std::wstring& path, const std::wstring& name) {
    if (path.empty()) return name;
    const wchar_t last = path.back();
    return path + ((last == L'\\' || last == L'/') ? L"" : L"\\") + name;
}

const wchar_t* Extension(const std::wstring& name) {
    const auto dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? L"" : name.c_str() + dot;
}

int Compare(const FileItem& left, const FileItem& right, SortKey key) {
    int result = 0;
    switch (key) {
    case SortKey::Name:
        result = StrCmpLogicalW(left.name.c_str(), right.name.c_str());
        break;
    case SortKey::Size:
        result = (left.size > right.size) - (left.size < right.size);
        break;
    case SortKey::Type:
        result = StrCmpLogicalW(Extension(left.name), Extension(right.name));
        break;
    case SortKey::Modified:
        result = CompareFileTime(&left.modified, &right.modified);
        break;
    }
    if (result == 0) result = StrCmpLogicalW(left.name.c_str(), right.name.c_str());
    if (result == 0) result = left.name.compare(right.name);
    if (result == 0) result = (left.id > right.id) - (left.id < right.id);
    return result;
}

}  // namespace

struct FolderModel::State {
    std::recursive_mutex callbackMutex;
    mutable std::mutex dataMutex;
    std::atomic<std::uint32_t> generation{0};
    std::atomic<std::uint32_t> nextItemId{1};
    std::atomic<bool> stopping{false};
    std::shared_ptr<const std::vector<FileItem>> active = std::make_shared<const std::vector<FileItem>>();
    std::wstring activePath;
    std::uint32_t activeGeneration = 0;
    CodecGap activeGap;
    std::shared_ptr<const std::vector<FileItem>> pending;
    std::wstring pendingPath;
    CodecGap pendingGap;
    HRESULT pendingResult = E_PENDING;
    HRESULT lastResult = S_OK;
    bool hasPending = false;
    bool loading = false;
    SortKey sortKey = SortKey::Name;
    bool ascending = true;
    std::uint64_t sortRevision = 0;
    OnLoaded callback;
};

FolderModel::FolderModel(std::shared_ptr<ThreadPool> pool) noexcept {
    try {
        state_ = std::make_shared<State>();
        if (pool) {
            pool_ = std::move(pool);
        } else {
            pool_ = std::make_shared<ThreadPool>(ThreadPool::DefaultThreadCount());
        }
    } catch (...) {
        state_.reset();
        pool_.reset();
    }
}

FolderModel::~FolderModel() {
    if (state_) {
        std::lock_guard<std::recursive_mutex> lock(state_->callbackMutex);
        state_->stopping = true;
        ++state_->generation;
    }
}

std::uint32_t FolderModel::Open(const std::wstring& path, OnLoaded callback) noexcept {
    if (!state_ || !pool_) return 0;
    if (path.empty()) {
        std::lock_guard<std::mutex> lock(state_->dataMutex);
        state_->lastResult = E_INVALIDARG;
        return 0;
    }
    try {
        const auto state = state_;
        std::lock_guard<std::recursive_mutex> callbackLock(state->callbackMutex);
        if (state->stopping) return 0;
        std::uint32_t generation = ++state->generation;
        if (generation == 0) generation = ++state->generation;
        {
            std::lock_guard<std::mutex> lock(state->dataMutex);
            state->callback = std::move(callback);
            state->pending.reset();
            state->hasPending = false;
            state->loading = true;
            state->lastResult = E_PENDING;
        }
        const HRESULT hr = pool_->Submit([state, path, generation] { Load(state, path, generation); });
        if (FAILED(hr)) {
            std::lock_guard<std::mutex> lock(state->dataMutex);
            state->loading = false;
            state->lastResult = hr;
            return 0;
        }
        return generation;
    } catch (...) {
        if (state_) {
            std::lock_guard<std::mutex> lock(state_->dataMutex);
            state_->loading = false;
            state_->lastResult = E_OUTOFMEMORY;
        }
        return 0;
    }
}

void FolderModel::Load(const std::shared_ptr<State>& state, std::wstring path,
                       std::uint32_t generation) noexcept {
    HRESULT result = S_OK;
    std::vector<FileItem> items;
    CodecGap gap;
    try {
        if (state->stopping || state->generation != generation) return;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            result = HRESULT_FROM_WIN32(GetLastError());
        } else if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            result = HRESULT_FROM_WIN32(ERROR_DIRECTORY);
        }
        if (SUCCEEDED(result)) {
            ComApartment apartment;
            result = apartment.result;
            if (SUCCEEDED(result)) {
                std::vector<std::wstring> extensions;
                result = DecoderExtensions(extensions);
                if (SUCCEEDED(result)) {
                    WIN32_FIND_DATAW data{};
                    FindHandle search(FindFirstFileExW(Join(path, L"*").c_str(), FindExInfoBasic, &data,
                                                       FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH));
                    if (search.handle == INVALID_HANDLE_VALUE) {
                        const DWORD error = GetLastError();
                        if (error != ERROR_FILE_NOT_FOUND) result = HRESULT_FROM_WIN32(error);
                    } else {
                        do {
                            if (state->stopping || state->generation != generation) return;
                            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                            const wchar_t* extension = PathFindExtensionW(data.cFileName);
                            CodecPack pack;
                            FileKind kind = FileKind::Image;
                            const bool image = HasExtension(extensions, extension);
                            if (!image && IsTextExtension(extension)) kind = FileKind::Text;
                            if (!image && (_wcsicmp(extension, L".pdf") == 0 || _wcsicmp(extension, L".ai") == 0))
                                kind = FileKind::Pdf;
                            if (!image && (_wcsicmp(extension, L".svg") == 0 || _wcsicmp(extension, L".emf") == 0 ||
                                           _wcsicmp(extension, L".wmf") == 0))
                                kind = FileKind::Vector;
                            if (!image && IsDocumentExtension(extension)) kind = FileKind::Document;
                            if (!image && ModelFormatForExtension(extension) != ModelFormat::None) kind = FileKind::Model;
                            if (!image && kind == FileKind::Image && IsVideoExtension(extension)) kind = FileKind::Video;
                            if (!image && kind == FileKind::Image) {
                                // A known format whose codec extension is not installed.
                                if (CodecPackForExtension(extension, pack)) {
                                    ++gap.files;
                                    gap.packs |= CodecPackBit(pack);
                                }
                            } else {
                                std::uint32_t id = state->nextItemId.fetch_add(1);
                                if (id == 0) id = state->nextItemId.fetch_add(1);
                                items.push_back(FileItem{data.cFileName,
                                    (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
                                    data.ftLastWriteTime, id, kind});
                            }
                        } while (FindNextFileW(search.handle, &data));
                        const DWORD error = GetLastError();
                        if (error != ERROR_NO_MORE_FILES) result = HRESULT_FROM_WIN32(error);
                    }
                }
            }
        }
    } catch (const std::bad_alloc&) {
        result = E_OUTOFMEMORY;
    } catch (...) {
        result = E_FAIL;
    }
    Finish(state, std::move(path), generation, result, std::move(items), gap);
}

void FolderModel::SortCopy(const std::shared_ptr<State>& state,
                           std::shared_ptr<const std::vector<FileItem>> source,
                           std::wstring path, std::uint32_t generation, CodecGap gap) noexcept {
    try {
        if (state->stopping || state->generation != generation) return;
        std::vector<FileItem> items(*source);
        Finish(state, std::move(path), generation, S_OK, std::move(items), gap);
    } catch (const std::bad_alloc&) {
        Finish(state, std::move(path), generation, E_OUTOFMEMORY, {}, {});
    } catch (...) {
        Finish(state, std::move(path), generation, E_FAIL, {}, {});
    }
}

void FolderModel::Finish(const std::shared_ptr<State>& state, std::wstring path,
                         std::uint32_t generation, HRESULT result,
                         std::vector<FileItem> items, CodecGap gap) noexcept {
    try {
        for (;;) {
            if (state->stopping || state->generation != generation) return;
            SortKey key;
            bool ascending;
            std::uint64_t revision;
            {
                std::lock_guard<std::mutex> lock(state->dataMutex);
                key = state->sortKey;
                ascending = state->ascending;
                revision = state->sortRevision;
            }
            if (SUCCEEDED(result)) {
                std::sort(items.begin(), items.end(), [key, ascending](const FileItem& a, const FileItem& b) {
                    const int comparison = Compare(a, b, key);
                    return ascending ? comparison < 0 : comparison > 0;
                });
            }
            std::lock_guard<std::recursive_mutex> callbackLock(state->callbackMutex);
            if (state->stopping || state->generation != generation) return;
            {
                std::lock_guard<std::mutex> lock(state->dataMutex);
                if (revision != state->sortRevision) continue;
                state->pending = SUCCEEDED(result) ? std::make_shared<const std::vector<FileItem>>(std::move(items)) : nullptr;
                state->pendingPath = std::move(path);
                state->pendingGap = SUCCEEDED(result) ? gap : CodecGap{};
                state->pendingResult = result;
                state->hasPending = true;
                state->loading = false;
                state->lastResult = result;
            }
            // A callback may reenter Open, so keep its captures alive separately.
            const OnLoaded callback = state->callback;
            if (callback) {
                try { callback(generation); } catch (...) { /* Do not kill a worker. */ }
            }
            return;
        }
    } catch (...) {
        std::lock_guard<std::recursive_mutex> callbackLock(state->callbackMutex);
        if (!state->stopping && state->generation == generation) {
            {
                std::lock_guard<std::mutex> lock(state->dataMutex);
                state->pending.reset();
                state->pendingResult = E_OUTOFMEMORY;
                state->hasPending = true;
                state->loading = false;
                state->lastResult = E_OUTOFMEMORY;
            }
            try {
                const OnLoaded callback = state->callback;
                if (callback) callback(generation);
            } catch (...) { /* Do not kill a worker. */ }
        }
    }
}

void FolderModel::Sort(SortKey key, bool ascending) noexcept {
    if (!state_ || !pool_) return;
    try {
        const auto state = state_;
        std::lock_guard<std::recursive_mutex> callbackLock(state->callbackMutex);
        if (state->stopping) return;
        std::shared_ptr<const std::vector<FileItem>> source;
        std::wstring path;
        CodecGap gap;
        const std::uint32_t generation = state->generation;
        {
            std::lock_guard<std::mutex> lock(state->dataMutex);
            state->sortKey = key;
            state->ascending = ascending;
            ++state->sortRevision;
            if (state->loading) return;
            if (state->hasPending && SUCCEEDED(state->pendingResult)) {
                source = state->pending;
                path = state->pendingPath;
                gap = state->pendingGap;
            } else if (state->activeGeneration == generation) {
                source = state->active;
                path = state->activePath;
                gap = state->activeGap;
            }
            if (!source) return;
            state->loading = true;
            state->lastResult = E_PENDING;
        }
        const HRESULT hr = pool_->Submit([state, source, path, generation, gap] {
            SortCopy(state, source, path, generation, gap);
        });
        if (FAILED(hr)) {
            std::lock_guard<std::mutex> lock(state->dataMutex);
            state->loading = false;
            state->lastResult = hr;
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(state_->dataMutex);
        state_->loading = false;
        state_->lastResult = E_OUTOFMEMORY;
    }
}

HRESULT FolderModel::Commit(std::uint32_t generation) noexcept {
    if (!state_) return E_OUTOFMEMORY;
    std::lock_guard<std::recursive_mutex> callbackLock(state_->callbackMutex);
    std::lock_guard<std::mutex> lock(state_->dataMutex);
    if (generation == 0 || state_->generation != generation) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (state_->loading || !state_->hasPending) return E_PENDING;
    const HRESULT hr = state_->pendingResult;
    if (SUCCEEDED(hr)) {
        state_->active = std::move(state_->pending);
        state_->activePath = std::move(state_->pendingPath);
        state_->activeGap = state_->pendingGap;
        state_->activeGeneration = generation;
    }
    state_->hasPending = false;
    state_->lastResult = hr;
    return hr;
}

HRESULT FolderModel::LastResult() const noexcept {
    if (!state_) return E_OUTOFMEMORY;
    std::lock_guard<std::mutex> lock(state_->dataMutex);
    return state_->lastResult;
}

CodecGap FolderModel::MissingCodecs() const noexcept {
    if (!state_) return {};
    std::lock_guard<std::mutex> lock(state_->dataMutex);
    return state_->activeGap;
}

std::size_t FolderModel::Count() const noexcept {
    if (!state_) return 0;
    std::lock_guard<std::mutex> lock(state_->dataMutex);
    return state_->active->size();
}

const FileItem& FolderModel::At(std::size_t index) const noexcept {
    static const FileItem empty;
    if (!state_) return empty;
    std::lock_guard<std::mutex> lock(state_->dataMutex);
    return index < state_->active->size() ? (*state_->active)[index] : empty;
}

std::wstring FolderModel::FullPath(std::size_t index) const noexcept {
    if (!state_) return {};
    try {
        std::lock_guard<std::mutex> lock(state_->dataMutex);
        if (index >= state_->active->size()) return {};
        return Join(state_->activePath, (*state_->active)[index].name);
    } catch (...) {
        return {};
    }
}
