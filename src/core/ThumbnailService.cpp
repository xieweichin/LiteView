#include "core/ThumbnailService.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwchar>
#include <utility>
#include <vector>

#include "core/ImageDecoder.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kExtension[] = L".jpg";
constexpr unsigned kPruneEveryWrites = 256;

// Worker threads come from a shared pool that does not initialize COM.
struct ThreadCom {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ThreadCom() { if (SUCCEEDED(result)) CoUninitialize(); }
};

HRESULT EnsureCom() {
    thread_local ThreadCom com;
    // RPC_E_CHANGED_MODE: the thread already has an apartment; COM is usable.
    return (SUCCEEDED(com.result) || com.result == RPC_E_CHANGED_MODE) ? S_OK : com.result;
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    return (dir.back() == L'\\' || dir.back() == L'/') ? dir + name : dir + L'\\' + name;
}

std::wstring ExecutableDirectory() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) {
            std::wstring path(buffer.data(), length);
            const auto slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
        }
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
}

// Premultiplied BGRA over white, then opaque.
void FlattenOntoWhite(BYTE* pixels, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        BYTE* p = pixels + i * 4;
        const int inverse = 255 - p[3];
        p[0] = static_cast<BYTE>(std::min(255, p[0] + inverse));
        p[1] = static_cast<BYTE>(std::min(255, p[1] + inverse));
        p[2] = static_cast<BYTE>(std::min(255, p[2] + inverse));
        p[3] = 255;
    }
}

HBITMAP CreateTopDownDib(UINT width, UINT height, void** bits) {
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, bits, nullptr, 0);
}

HRESULT CreateFactory(ComPtr<IWICImagingFactory>& factory) {
    return CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
}

// Scales `source` down so its long side is <= size; returns it unchanged if it already fits.
HRESULT FitWithin(IWICImagingFactory* factory, IWICBitmapSource* source, UINT size,
                  ComPtr<IWICBitmapSource>& out) {
    UINT width = 0, height = 0;
    HRESULT hr = source->GetSize(&width, &height);
    if (FAILED(hr)) return hr;
    if (width == 0 || height == 0) return WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
    if (width <= size && height <= size) {
        out = source;
        return S_OK;
    }
    const double scale = static_cast<double>(size) / std::max(width, height);
    const UINT w = std::max<UINT>(1, static_cast<UINT>(width * scale + 0.5));
    const UINT h = std::max<UINT>(1, static_cast<UINT>(height * scale + 0.5));
    ComPtr<IWICBitmapScaler> scaler;
    hr = factory->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(hr)) hr = scaler->Initialize(source, w, h, WICBitmapInterpolationModeFant);
    if (SUCCEEDED(hr)) out = scaler;
    return hr;
}

// Copies a 32bppPBGRA source into a new opaque DIB thumbnail.
HRESULT ThumbnailFromSource(IWICBitmapSource* source, ThumbnailPtr& out) {
    UINT width = 0, height = 0;
    HRESULT hr = source->GetSize(&width, &height);
    if (FAILED(hr)) return hr;
    if (width == 0 || height == 0 || width > 4096 || height > 4096) return WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
    void* bits = nullptr;
    HBITMAP bitmap = CreateTopDownDib(width, height, &bits);
    if (!bitmap || !bits) return E_OUTOFMEMORY;
    auto thumbnail = std::make_shared<Thumbnail>();
    thumbnail->bitmap = bitmap;
    thumbnail->width = width;
    thumbnail->height = height;
    const UINT stride = width * 4;
    hr = source->CopyPixels(nullptr, stride, stride * height, static_cast<BYTE*>(bits));
    if (FAILED(hr)) return hr;
    FlattenOntoWhite(static_cast<BYTE*>(bits), static_cast<std::size_t>(width) * height);
    GdiFlush();
    out = std::move(thumbnail);
    return S_OK;
}

HRESULT DecodeWithWic(const std::wstring& path, UINT size, ThumbnailPtr& out) {
    ImageDecoder decoder;
    DecodedImage image;
    HRESULT hr = decoder.Decode(path, size, image);
    if (FAILED(hr)) return hr;
    ComPtr<IWICImagingFactory> factory;
    hr = CreateFactory(factory);
    ComPtr<IWICBitmapSource> fitted;
    if (SUCCEEDED(hr)) hr = FitWithin(factory.Get(), image.bitmap.Get(), size, fitted);
    if (SUCCEEDED(hr)) hr = ThumbnailFromSource(fitted.Get(), out);
    return hr;
}

HRESULT ShellThumbnail(const std::wstring& path, UINT size, ThumbnailPtr& out) {
    ComPtr<IShellItemImageFactory> imageFactory;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&imageFactory));
    if (FAILED(hr)) return hr;
    HBITMAP raw = nullptr;
    hr = imageFactory->GetImage(SIZE{static_cast<LONG>(size), static_cast<LONG>(size)}, SIIGBF_THUMBNAILONLY, &raw);
    if (FAILED(hr)) return hr;
    if (!raw) return E_FAIL;

    BITMAP bm{};
    if (!GetObjectW(raw, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0) {
        DeleteObject(raw);
        return E_FAIL;
    }
    const UINT width = static_cast<UINT>(bm.bmWidth);
    const UINT height = static_cast<UINT>(bm.bmHeight < 0 ? -bm.bmHeight : bm.bmHeight);
    if (std::max(width, height) < size / 4) {
        // Not a real thumbnail (some shells hand back a placeholder);
        // let WIC decode the file instead.
        DeleteObject(raw);
        return E_FAIL;
    }
    std::vector<BYTE> pixels(static_cast<std::size_t>(width) * height * 4);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    const int lines = GetDIBits(dc, raw, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(raw);
    if (lines != static_cast<int>(height)) return E_FAIL;

    // Opaque shell bitmaps often carry alpha 0 everywhere; treat those as
    // opaque. Otherwise the shell hands out premultiplied alpha.
    bool anyAlpha = false;
    for (std::size_t i = 3; i < pixels.size(); i += 4) {
        if (pixels[i] != 0) {
            anyAlpha = true;
            break;
        }
    }
    if (!anyAlpha) {
        for (std::size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    }

    ComPtr<IWICImagingFactory> factory;
    hr = CreateFactory(factory);
    ComPtr<IWICBitmap> bitmap;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA, width * 4,
                                             static_cast<UINT>(pixels.size()), pixels.data(), &bitmap);
    }
    ComPtr<IWICBitmapSource> fitted;
    if (SUCCEEDED(hr)) hr = FitWithin(factory.Get(), bitmap.Get(), size, fitted);
    if (SUCCEEDED(hr)) hr = ThumbnailFromSource(fitted.Get(), out);
    return hr;
}

// Writes the thumbnail as an 85% JPEG. A temporary name keeps other workers
// from reading a half-written file.
HRESULT WriteJpeg(const Thumbnail& thumbnail, const std::wstring& path) {
    DIBSECTION dib{};
    if (GetObjectW(thumbnail.bitmap, sizeof(dib), &dib) != sizeof(dib) || !dib.dsBm.bmBits) return E_FAIL;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CreateFactory(factory);
    ComPtr<IWICBitmap> source;
    const UINT stride = thumbnail.width * 4;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateBitmapFromMemory(thumbnail.width, thumbnail.height, GUID_WICPixelFormat32bppBGR, stride,
                                             stride * thumbnail.height, static_cast<BYTE*>(dib.dsBm.bmBits), &source);
    }
    const std::wstring temp = path + L".tmp" + std::to_wstring(GetCurrentThreadId());
    ComPtr<IWICStream> stream;
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(temp.c_str(), GENERIC_WRITE);
    ComPtr<IWICBitmapEncoder> encoder;
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &options);
    if (SUCCEEDED(hr) && options) {
        PROPBAG2 option = {};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = 0.85f;
        options->Write(1, &option, &value);  // best effort: default quality is acceptable
    }
    if (SUCCEEDED(hr)) hr = frame->Initialize(options.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(thumbnail.width, thumbnail.height);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    ComPtr<IWICBitmapSource> converted = source;
    if (SUCCEEDED(hr) && format != GUID_WICPixelFormat32bppBGR) {
        ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) {
            hr = converter->Initialize(source.Get(), format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom);
        }
        if (SUCCEEDED(hr)) converted = converter;
    }
    if (SUCCEEDED(hr)) hr = frame->WriteSource(converted.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    stream.Reset();
    encoder.Reset();
    frame.Reset();
    if (SUCCEEDED(hr) && !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (FAILED(hr)) DeleteFileW(temp.c_str());
    return hr;
}

void Touch(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    SetFileTime(file, nullptr, nullptr, &now);  // last write time = last used
    CloseHandle(file);
}

}  // namespace

struct ThumbnailService::Shared {
    OnReady onReady;
    Options options;
    Stats stats;

    mutable std::mutex mutex;
    std::list<std::pair<std::wstring, ThumbnailPtr>> lru;  // front = most recent
    std::unordered_map<std::wstring, std::list<std::pair<std::wstring, ThumbnailPtr>>::iterator> index;
    std::size_t memoryBytes = 0;
    std::unordered_set<std::wstring> failed;
    std::unordered_map<std::wstring, std::vector<std::wstring>> keysByPath;  // lower-case path -> keys
    std::unordered_set<std::wstring> skipShell;                             // lower-case paths
    std::unordered_map<std::wstring, ThreadPool::TaskId> queued;
    bool directoryResolved = false;
    std::wstring directory;
    std::atomic<unsigned> writes{0};
    std::mutex pruneMutex;

    std::wstring Directory() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!directoryResolved) {
            directoryResolved = true;
            directory = options.cacheDirectory.empty() ? DefaultCacheDirectory() : options.cacheDirectory;
            if (!directory.empty()) {
                const int created = SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
                if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS && created != ERROR_FILE_EXISTS) {
                    directory.clear();  // no disk cache; memory cache still works
                }
            }
        }
        return directory;
    }

    HRESULT Prune() noexcept {
        try {
            std::lock_guard<std::mutex> pruneLock(pruneMutex);  // one scan at a time
            const std::wstring directory = Directory();
            if (directory.empty()) return S_FALSE;
            struct Entry {
                ULONGLONG lastUsed;
                ULONGLONG size;
                std::wstring name;
            };
            std::vector<Entry> entries;
            ULONGLONG total = 0;
            WIN32_FIND_DATAW data;
            HANDLE find = FindFirstFileExW(Join(directory, L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                           nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (find == INVALID_HANDLE_VALUE) {
                const DWORD error = GetLastError();
                return error == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(error);
            }
            do {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const ULONGLONG size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                const ULONGLONG used = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                                       data.ftLastWriteTime.dwLowDateTime;
                total += size;
                entries.push_back({used, size, data.cFileName});
            } while (FindNextFileW(find, &data));
            FindClose(find);
            const ULONGLONG limit = options.diskLimitBytes;
            if (total <= limit) return S_OK;
            // Delete down to 90% so pruning does not run on every write.
            const ULONGLONG target = limit - limit / 10;
            std::sort(entries.begin(), entries.end(),
                      [](const Entry& a, const Entry& b) { return a.lastUsed < b.lastUsed; });
            for (const Entry& entry : entries) {
                if (total <= target) break;
                if (DeleteFileW(Join(directory, entry.name).c_str())) total -= entry.size;
            }
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    ThumbnailPtr Find(const std::wstring& key) {
        auto it = index.find(key);
        if (it == index.end()) return nullptr;
        lru.splice(lru.begin(), lru, it->second);
        return it->second->second;
    }

    static std::wstring Lower(std::wstring text) {
        if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
        return text;
    }

    void Store(const std::wstring& path, const std::wstring& key, ThumbnailPtr thumbnail) {
        keysByPath[Lower(path)].push_back(key);
        const std::size_t bytes = static_cast<std::size_t>(thumbnail->width) * thumbnail->height * 4;
        auto existing = index.find(key);
        if (existing != index.end()) {
            const auto& old = existing->second->second;
            memoryBytes -= static_cast<std::size_t>(old->width) * old->height * 4;
            lru.erase(existing->second);
            index.erase(existing);
        }
        lru.emplace_front(key, std::move(thumbnail));
        index[key] = lru.begin();
        memoryBytes += bytes;
        while (memoryBytes > options.memoryLimitBytes && lru.size() > 1) {
            const auto& victim = lru.back();
            memoryBytes -= static_cast<std::size_t>(victim.second->width) * victim.second->height * 4;
            index.erase(victim.first);
            lru.pop_back();
        }
    }
};

ThumbnailService::ThumbnailService(OnReady onReady, Options options) noexcept
    : shared_(std::make_shared<Shared>()), stats_(shared_->stats) {
    shared_->onReady = std::move(onReady);
    shared_->options = std::move(options);
    const std::size_t threads =
        shared_->options.threadCount ? shared_->options.threadCount : ThreadPool::DefaultThreadCount();
    pool_ = std::make_unique<ThreadPool>(threads);
    // Keep the disk cache in bounds without delaying the first thumbnails.
    const auto shared = shared_;
    pool_->Submit([shared] { shared->Prune(); }, ThreadPool::Priority::Low);
}

ThumbnailService::~ThumbnailService() {
    if (pool_) pool_->Shutdown();
}

std::wstring ThumbnailService::DefaultCacheDirectory() {
    const std::wstring exeDir = ExecutableDirectory();
    if (!exeDir.empty() && GetFileAttributesW(Join(exeDir, L"portable.ini").c_str()) != INVALID_FILE_ATTRIBUTES) {
        return Join(exeDir, L"thumbs");
    }
    PWSTR local = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local)) && local) {
        result = Join(Join(local, L"LiteView"), L"thumbs");
    }
    CoTaskMemFree(local);
    return result;
}

std::wstring ThumbnailService::CacheKey(const ThumbnailRequest& request) {
    // FNV-1a over the lower-cased path, modified time, file size and thumbnail size.
    std::uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    std::wstring path = request.path;
    if (!path.empty()) CharLowerBuffW(path.data(), static_cast<DWORD>(path.size()));
    mix(path.data(), path.size() * sizeof(wchar_t));
    mix(&request.modified, sizeof(request.modified));
    mix(&request.fileSize, sizeof(request.fileSize));
    mix(&request.size, sizeof(request.size));
    wchar_t text[17] = {};
    swprintf(text, 17, L"%016llx", static_cast<unsigned long long>(hash));
    return text;
}

std::wstring ThumbnailService::CacheDirectory() const { return shared_->Directory(); }

ThumbnailPtr ThumbnailService::TryGetCached(const ThumbnailRequest& request, bool* failed) const noexcept {
    try {
        const std::wstring key = CacheKey(request);
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (failed) *failed = shared_->failed.count(key) != 0;
        return shared_->Find(key);
    } catch (...) {
        if (failed) *failed = false;
        return nullptr;
    }
}

void ThumbnailService::Request(const ThumbnailRequest& request, bool visible) noexcept {
    try {
        std::wstring key = CacheKey(request);
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            if (shared_->index.count(key) || shared_->failed.count(key) || shared_->queued.count(key)) return;
        }
        ThreadPool::TaskId id = 0;
        const auto shared = shared_;
        std::lock_guard<std::mutex> lock(shared_->mutex);
        const HRESULT hr = pool_->Submit([shared, request, key] { Work(shared, request, key); },
                                         visible ? ThreadPool::Priority::High : ThreadPool::Priority::Normal, &id);
        if (SUCCEEDED(hr)) shared_->queued.emplace(std::move(key), id);
    } catch (...) {
    }
}

void ThumbnailService::Invalidate(const ThumbnailRequest& request) noexcept {
    try {
        const std::wstring key = CacheKey(request);
        const std::wstring lower = Shared::Lower(request.path);
        std::wstring directory;
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            auto keys = shared_->keysByPath.find(lower);
            if (keys != shared_->keysByPath.end()) {
                for (const auto& k : keys->second) {
                    auto it = shared_->index.find(k);
                    if (it == shared_->index.end()) continue;
                    const auto& old = it->second->second;
                    shared_->memoryBytes -= static_cast<std::size_t>(old->width) * old->height * 4;
                    shared_->lru.erase(it->second);
                    shared_->index.erase(it);
                }
                shared_->keysByPath.erase(keys);
            }
            shared_->failed.erase(key);
            shared_->skipShell.insert(lower);
            directory = shared_->directoryResolved ? shared_->directory : std::wstring();
        }
        if (directory.empty()) directory = shared_->Directory();
        if (!directory.empty()) DeleteFileW(Join(directory, key + kExtension).c_str());
    } catch (...) {
    }
}

void ThumbnailService::CancelQueued() noexcept {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    for (const auto& entry : shared_->queued) pool_->Cancel(entry.second);
    shared_->queued.clear();
}

void ThumbnailService::Work(const std::shared_ptr<Shared>& shared, ThumbnailRequest request,
                            std::wstring key) noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->queued.erase(key);
            if (shared->Find(key)) {
                ++shared->stats.memoryHits;
                if (shared->onReady) shared->onReady(request.generation, request.itemId, S_OK);
                return;
            }
        }
        HRESULT hr = EnsureCom();
        ThumbnailPtr thumbnail;
        const std::wstring directory = SUCCEEDED(hr) ? shared->Directory() : std::wstring();
        const std::wstring cacheFile = directory.empty() ? std::wstring() : Join(directory, key + kExtension);

        if (SUCCEEDED(hr) && !cacheFile.empty() && GetFileAttributesW(cacheFile.c_str()) != INVALID_FILE_ATTRIBUTES &&
            SUCCEEDED(DecodeWithWic(cacheFile, request.size, thumbnail))) {
            Touch(cacheFile);
            ++shared->stats.diskHits;
        }
        bool skipShell = false;
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            skipShell = shared->skipShell.count(Shared::Lower(request.path)) != 0;
        }
        if (SUCCEEDED(hr) && !thumbnail && shared->options.useShellThumbnails && !skipShell &&
            SUCCEEDED(ShellThumbnail(request.path, request.size, thumbnail))) {
            ++shared->stats.shellHits;
        }
        if (SUCCEEDED(hr) && !thumbnail) {
            hr = DecodeWithWic(request.path, request.size, thumbnail);
            if (SUCCEEDED(hr)) {
                ++shared->stats.decodes;
                if (!cacheFile.empty() && SUCCEEDED(WriteJpeg(*thumbnail, cacheFile)) &&
                    ++shared->writes % kPruneEveryWrites == 0) {
                    shared->Prune();  // already off the UI thread
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (thumbnail) {
                shared->Store(request.path, key, thumbnail);
            } else {
                shared->failed.insert(key);
                ++shared->stats.failures;
            }
        }
        if (shared->onReady) shared->onReady(request.generation, request.itemId, thumbnail ? S_OK : hr);
    } catch (...) {
        if (shared->onReady) shared->onReady(request.generation, request.itemId, E_FAIL);
    }
}

HRESULT ThumbnailService::PruneDiskCache() noexcept { return shared_->Prune(); }
