#include "ui/DocumentView.h"

#include <propsys.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwchar>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "res/resource.h"
#include "ui/Messages.h"
#include "ui/Theme.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClassName[] = L"LiteViewDocumentView";
// Shell extension key of preview handlers (IPreviewHandler's IID).
constexpr wchar_t kPreviewHandlerKey[] = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";

void AddClsid(const wchar_t* text, std::vector<CLSID>& out) {
    CLSID clsid{};
    if (!text || !*text || FAILED(CLSIDFromString(text, &clsid))) return;
    if (std::find(out.begin(), out.end(), clsid) == out.end()) out.push_back(clsid);
}

// Default value of HKCR\<key> as text, or empty.
std::wstring ClassesValue(const std::wstring& key, const wchar_t* value = nullptr) {
    wchar_t text[256] = {};
    DWORD size = sizeof(text);
    if (RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), value, RRF_RT_REG_SZ, nullptr, text, &size) != ERROR_SUCCESS)
        return std::wstring();
    return text;
}

// Every preview handler registered for the file type, in the order Explorer
// looks: the association (the default app's ProgID), then the extension, the
// system file associations, the ProgID named by the extension and the other
// ProgIDs that can open it. Office registers on its ProgIDs, and the default
// app for .docx may be another program, so the first lookup alone can miss.
std::vector<CLSID> HandlersFor(const std::wstring& path) {
    std::vector<CLSID> handlers;
    const wchar_t* extension = PathFindExtensionW(path.c_str());
    if (!*extension) return handlers;
    wchar_t text[64] = {};
    DWORD length = ARRAYSIZE(text);
    if (SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_DEFAULTTOSTAR, ASSOCSTR_SHELLEXTENSION, extension, kPreviewHandlerKey,
                                    text, &length)))
        AddClsid(text, handlers);
    const std::wstring shellEx = std::wstring(L"\\ShellEx\\") + kPreviewHandlerKey;
    const std::wstring ext = extension;
    AddClsid(ClassesValue(ext + shellEx).c_str(), handlers);
    AddClsid(ClassesValue(L"SystemFileAssociations\\" + ext + shellEx).c_str(), handlers);
    const std::wstring progId = ClassesValue(ext);
    if (!progId.empty()) AddClsid(ClassesValue(progId + shellEx).c_str(), handlers);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, (ext + L"\\OpenWithProgids").c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < 64; ++i) {
            wchar_t name[256] = {};
            DWORD nameLength = ARRAYSIZE(name);
            if (RegEnumValueW(key, i, name, &nameLength, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            if (*name) AddClsid(ClassesValue(std::wstring(name) + shellEx).c_str(), handlers);
        }
        RegCloseKey(key);
    }
    const std::wstring perceived = ClassesValue(ext, L"PerceivedType");
    if (!perceived.empty()) AddClsid(ClassesValue(L"SystemFileAssociations\\" + perceived + shellEx).c_str(), handlers);
    return handlers;
}

// Where a load failed, shown after the error code so that a report from a
// user's PC says which call refused.
enum Step : int {
    kStepCreate = 1,        // CoCreateInstance
    kStepMemoryStream = 2,  // IInitializeWithStream with the in-memory copy
    kStepFileStream = 3,    // IInitializeWithStream with a stream on the file
    kStepFile = 4,          // IInitializeWithFile
    kStepItem = 5,          // IInitializeWithItem
    kStepWindow = 6,        // SetWindow
    kStepPreview = 7,       // DoPreview
};

// A read-only in-memory copy of the file. Unlike SHCreateMemStream's stream it
// answers Stat with the file's name and size: Office's handlers read the name
// to tell a .docx from a .xlsx and fail with E_NOTIMPL without it.
class FileCopyStream final : public IStream {
public:
    static HRESULT Create(const std::wstring& path, ComPtr<IStream>& stream);

    IFACEMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IStream || riid == IID_ISequentialStream) {
            *object = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const LONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs);
    }

    IFACEMETHODIMP Read(void* buffer, ULONG count, ULONG* read) override {
        if (!buffer && count) return STG_E_INVALIDPOINTER;
        const std::size_t size = data_->size();
        const std::size_t available = position_ < size ? size - static_cast<std::size_t>(position_) : 0;
        const ULONG n = static_cast<ULONG>(std::min<std::size_t>(available, count));
        if (n) memcpy(buffer, data_->data() + position_, n);
        position_ += n;
        if (read) *read = n;
        return n < count ? S_FALSE : S_OK;
    }
    IFACEMETHODIMP Write(const void*, ULONG, ULONG* written) override {
        if (written) *written = 0;
        return STG_E_ACCESSDENIED;
    }
    IFACEMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* position) override {
        LONGLONG base = 0;
        switch (origin) {
        case STREAM_SEEK_SET: base = 0; break;
        case STREAM_SEEK_CUR: base = static_cast<LONGLONG>(position_); break;
        case STREAM_SEEK_END: base = static_cast<LONGLONG>(data_->size()); break;
        default: return STG_E_INVALIDFUNCTION;
        }
        const LONGLONG target = base + move.QuadPart;
        if (target < 0) return STG_E_INVALIDFUNCTION;
        position_ = static_cast<ULONGLONG>(target);
        if (position) position->QuadPart = position_;
        return S_OK;
    }
    IFACEMETHODIMP SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    IFACEMETHODIMP CopyTo(IStream* target, ULARGE_INTEGER count, ULARGE_INTEGER* read,
                          ULARGE_INTEGER* written) override {
        if (!target) return STG_E_INVALIDPOINTER;
        const std::size_t size = data_->size();
        const ULONGLONG available = position_ < size ? size - position_ : 0;
        ULONGLONG left = std::min<ULONGLONG>(available, count.QuadPart);
        ULONGLONG total = 0;
        HRESULT hr = S_OK;
        while (left && SUCCEEDED(hr)) {
            const ULONG chunk = static_cast<ULONG>(std::min<ULONGLONG>(left, 1u << 20));
            ULONG done = 0;
            hr = target->Write(data_->data() + position_, chunk, &done);
            position_ += done;
            total += done;
            left -= done;
            if (!done) break;
        }
        if (read) read->QuadPart = total;
        if (written) written->QuadPart = total;
        return hr;
    }
    IFACEMETHODIMP Commit(DWORD) override { return S_OK; }
    IFACEMETHODIMP Revert() override { return S_OK; }
    IFACEMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    IFACEMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    IFACEMETHODIMP Stat(STATSTG* stat, DWORD flags) override {
        if (!stat) return STG_E_INVALIDPOINTER;
        *stat = {};
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = data_->size();
        stat->mtime = modified_;
        stat->ctime = created_;
        stat->atime = accessed_;
        stat->grfMode = STGM_READ | STGM_SHARE_DENY_NONE;
        stat->clsid = CLSID_NULL;
        if (!(flags & STATFLAG_NONAME)) {
            const std::size_t bytes = (name_.size() + 1) * sizeof(wchar_t);
            stat->pwcsName = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
            if (!stat->pwcsName) return STG_E_INSUFFICIENTMEMORY;
            memcpy(stat->pwcsName, name_.c_str(), bytes);
        }
        return S_OK;
    }
    IFACEMETHODIMP Clone(IStream** clone) override {
        if (!clone) return STG_E_INVALIDPOINTER;
        auto* copy = new (std::nothrow) FileCopyStream(*this);
        if (!copy) return E_OUTOFMEMORY;
        *clone = copy;
        return S_OK;
    }

private:
    FileCopyStream() = default;
    FileCopyStream(const FileCopyStream& other)
        : data_(other.data_), position_(other.position_), name_(other.name_), created_(other.created_),
          accessed_(other.accessed_), modified_(other.modified_) {}
    ~FileCopyStream() = default;

    LONG refs_ = 1;
    std::shared_ptr<const std::vector<BYTE>> data_;
    ULONGLONG position_ = 0;
    std::wstring name_;
    FILETIME created_{};
    FILETIME accessed_{};
    FILETIME modified_{};
};

// Files up to this size are copied into memory so that the preview does not
// keep the file open: an open file cannot be renamed, moved or deleted.
constexpr ULONGLONG kMemoryStreamBytes = 256ull * 1024 * 1024;

HRESULT FileCopyStream::Create(const std::wstring& path, ComPtr<IStream>& stream) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    BY_HANDLE_FILE_INFORMATION info{};
    HRESULT hr = GetFileInformationByHandle(file, &info) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    const ULONGLONG size = (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (SUCCEEDED(hr) && size > kMemoryStreamBytes) hr = E_OUTOFMEMORY;
    auto data = std::make_shared<std::vector<BYTE>>();
    if (SUCCEEDED(hr)) {
        try {
            data->resize(static_cast<std::size_t>(size));
        } catch (...) {
            hr = E_OUTOFMEMORY;
        }
    }
    std::size_t done = 0;
    while (SUCCEEDED(hr) && done < data->size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data->size() - done, 16u * 1024 * 1024));
        if (!ReadFile(file, data->data() + done, chunk, &read, nullptr)) hr = HRESULT_FROM_WIN32(GetLastError());
        else if (read == 0) break;
        done += read;
    }
    CloseHandle(file);
    if (FAILED(hr)) return hr;
    data->resize(done);
    auto* copy = new (std::nothrow) FileCopyStream();
    if (!copy) return E_OUTOFMEMORY;
    copy->data_ = std::move(data);
    copy->name_ = PathFindFileNameW(path.c_str());
    copy->created_ = info.ftCreationTime;
    copy->accessed_ = info.ftLastAccessTime;
    copy->modified_ = info.ftLastWriteTime;
    stream.Attach(copy);
    return S_OK;
}

// A failure and the step it happened at; the deepest step is the most telling.
struct Failure {
    HRESULT hr = E_FAIL;
    int step = 0;
    void Note(HRESULT result, int at) {
        if (at >= step) {
            hr = result;
            step = at;
        }
    }
};

// Check on the host thread only: the path may be on a slow network drive.
// A zone marker is a clue, not proof that it caused the preview failure.
bool HasInternetZone(const std::wstring& path) {
    const std::wstring stream = path + L":Zone.Identifier";
    const UINT zone = GetPrivateProfileIntW(L"ZoneTransfer", L"ZoneId", 0, stream.c_str());
    return zone >= 3 && zone <= 4;
}

// Creates the handler and gives it the file in the way `step` names.
// Handlers support one or more of the initialisation interfaces and some
// refuse one they also implement, so the caller tries each on a fresh
// instance: the in-memory copy first, then a stream on the file itself, the
// path and the shell item. E_NOINTERFACE: the handler lacks that interface.
HRESULT CreateHandler(const CLSID& clsid, const std::wstring& path, int step, ComPtr<IPreviewHandler>& handler,
                      ComPtr<IStream>& stream, Failure& failure, bool& cannotStart) {
    cannotStart = false;
    ComPtr<IPreviewHandler> candidate;
    // Handlers normally run isolated in a surrogate process (prevhost.exe).
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(candidate.GetAddressOf()));
    if (FAILED(hr))
        hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(candidate.GetAddressOf()));
    if (FAILED(hr)) {
        failure.Note(hr, kStepCreate);
        cannotStart = true;
        return hr;
    }
    ComPtr<IStream> candidateStream;
    if (step == kStepMemoryStream || step == kStepFileStream) {
        ComPtr<IInitializeWithStream> withStream;
        if (FAILED(candidate.As(&withStream))) return E_NOINTERFACE;
        if (step == kStepMemoryStream) {
            hr = FileCopyStream::Create(path, candidateStream);
            if (FAILED(hr)) return E_NOINTERFACE;  // too large: the file stream follows
        } else {
            hr = SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE,
                                        nullptr, candidateStream.GetAddressOf());
        }
        if (SUCCEEDED(hr)) hr = withStream->Initialize(candidateStream.Get(), STGM_READ);
    } else if (step == kStepFile) {
        ComPtr<IInitializeWithFile> withFile;
        if (FAILED(candidate.As(&withFile))) return E_NOINTERFACE;
        hr = withFile->Initialize(path.c_str(), STGM_READ);
    } else {
        ComPtr<IInitializeWithItem> withItem;
        if (FAILED(candidate.As(&withItem))) return E_NOINTERFACE;
        ComPtr<IShellItem> item;
        hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.GetAddressOf()));
        if (SUCCEEDED(hr)) hr = withItem->Initialize(item.Get(), STGM_READ);
    }
    if (FAILED(hr)) {
        failure.Note(hr, step);
        return hr;
    }
    handler = candidate;
    stream = candidateStream;
    return S_OK;
}

std::wstring LoadStr(UINT id) {
    const wchar_t* text = nullptr;
    const int length = LoadStringW(GetModuleHandleW(nullptr), id, reinterpret_cast<LPWSTR>(&text), 0);
    return length > 0 ? std::wstring(text, static_cast<std::size_t>(length)) : std::wstring();
}

// Messages to the host thread's window.
constexpr wchar_t kHostClassName[] = L"LiteViewDocumentHost";
constexpr UINT kHostLoad = WM_APP + 1;    // wParam: generation
constexpr UINT kHostLayout = WM_APP + 2;  // the rectangle changed
constexpr UINT kHostColors = WM_APP + 3;  // the colours changed
constexpr UINT kHostUnload = WM_APP + 4;  // lParam: Signal* to set once unloaded, or null
constexpr UINT kHostQuit = WM_APP + 5;

// An event that the UI thread may stop waiting for before it is set.
struct Signal {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Signal() { if (event) CloseHandle(event); }
};

// Waits for `handle` while still answering messages sent to this thread
// (the host forwards keys to the UI thread with SendMessage).
bool WaitAnsweringSent(HANDLE handle, DWORD timeoutMs) {
    const ULONGLONG end = GetTickCount64() + timeoutMs;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= end) return false;
        const DWORD result =
            MsgWaitForMultipleObjectsEx(1, &handle, static_cast<DWORD>(end - now), QS_SENDMESSAGE, 0);
        if (result == WAIT_OBJECT_0) return true;
        if (result != WAIT_OBJECT_0 + 1) return false;
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
}

}  // namespace

// State shared by the UI thread and the host thread.
struct DocumentHost {
    std::mutex mutex;
    HWND owner = nullptr;   // the DocumentView window; null once it is gone
    HWND window = nullptr;  // the host thread's message window
    HACCEL accelerators = nullptr;
    unsigned generation = 0;  // latest request; older loads are dropped
    std::wstring path;
    RECT rect{};
    COLORREF background = 0;
    COLORREF text = 0;
    int failedStep = 0;  // where the last failed load stopped (see Step)
    bool failedInternetZone = false;

    bool Current(unsigned g) {
        std::lock_guard<std::mutex> lock(mutex);
        return generation == g;
    }
    bool Post(UINT msg, WPARAM wParam, LPARAM lParam) {
        std::lock_guard<std::mutex> lock(mutex);
        return window && PostMessageW(window, msg, wParam, lParam);
    }
};

namespace {

// The site given to the preview handler: lets keys the handler does not use
// (Esc, PageDown, Ctrl+F...) reach LiteView's accelerators on the UI thread.
class Frame final : public IPreviewHandlerFrame {
public:
    explicit Frame(std::shared_ptr<DocumentHost> host) : host_(std::move(host)) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPreviewHandlerFrame) {
            *object = static_cast<IPreviewHandlerFrame*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const LONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs);
    }

    IFACEMETHODIMP GetWindowContext(PREVIEWHANDLERFRAMEINFO* info) override {
        if (!info) return E_POINTER;
        const HACCEL accelerators = host_->accelerators;  // set before the thread starts
        info->haccel = accelerators;
        info->cAccelEntries = accelerators ? static_cast<UINT>(CopyAcceleratorTableW(accelerators, nullptr, 0)) : 0;
        return S_OK;
    }

    IFACEMETHODIMP TranslateAccelerator(MSG* msg) override {
        if (!msg) return S_FALSE;
        HWND owner;
        {
            std::lock_guard<std::mutex> lock(host_->mutex);
            owner = host_->owner;
        }
        DWORD_PTR handled = 0;
        if (!owner || !SendMessageTimeoutW(owner, WM_APP_DOCUMENT_ACCEL, 0, reinterpret_cast<LPARAM>(msg),
                                           SMTO_NORMAL, 1000, &handled))
            return S_FALSE;
        return handled ? S_OK : S_FALSE;
    }

private:
    ~Frame() = default;
    LONG refs_ = 1;
    std::shared_ptr<DocumentHost> host_;
};

// Runs on the host thread: owns the preview handler, which may take a second
// or two to start (Office's), so the UI thread never waits for it.
class HostWorker {
public:
    explicit HostWorker(std::shared_ptr<DocumentHost> host) : host_(std::move(host)) {}

    void Run(HINSTANCE instance, HANDLE ready) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        WNDCLASSEXW wc = {sizeof(wc)};
        if (!GetClassInfoExW(instance, kHostClassName, &wc)) {
            wc = {sizeof(wc)};
            wc.lpfnWndProc = &HostWorker::WndProc;
            wc.hInstance = instance;
            wc.lpszClassName = kHostClassName;
            RegisterClassExW(&wc);
        }
        if (SUCCEEDED(com))
            window_ = CreateWindowExW(0, kHostClassName, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
        try {
            frame_.Attach(new Frame(host_));  // starts with one reference
        } catch (...) {
        }
        {
            std::lock_guard<std::mutex> lock(host_->mutex);
            host_->window = window_;
        }
        SetEvent(ready);
        if (window_) {
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            {
                std::lock_guard<std::mutex> lock(host_->mutex);
                host_->window = nullptr;
            }
            // Requests posted before the window was forgotten: set their signals.
            quitting_ = true;
            while (PeekMessageW(&msg, window_, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            Unload();
            DestroyWindow(window_);
        }
        frame_.Reset();
        if (SUCCEEDED(com)) CoUninitialize();
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        }
        auto* self = reinterpret_cast<HostWorker*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
        switch (msg) {
        case kHostLoad:
            if (!self->quitting_) self->Load(static_cast<unsigned>(wParam));
            return 0;
        case kHostLayout:
            if (!self->quitting_) self->Layout();
            return 0;
        case kHostColors:
            if (!self->quitting_) self->Colors();
            return 0;
        case kHostUnload: {
            self->Unload();
            std::unique_ptr<std::shared_ptr<Signal>> signal(reinterpret_cast<std::shared_ptr<Signal>*>(lParam));
            if (signal && *signal && (*signal)->event) SetEvent((*signal)->event);
            return 0;
        }
        case kHostQuit:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void Report(unsigned generation, HRESULT hr) {
        std::lock_guard<std::mutex> lock(host_->mutex);
        if (host_->owner && host_->generation == generation)
            PostMessageW(host_->owner, WM_APP_DOCUMENT_LOADED, generation, static_cast<LPARAM>(hr));
    }

    void Load(unsigned generation) {
        Unload();
        std::wstring path;
        HWND owner;
        {
            std::lock_guard<std::mutex> lock(host_->mutex);
            if (host_->generation != generation || !host_->owner) return;
            path = host_->path;
            owner = host_->owner;
        }
        const std::vector<CLSID> handlers = HandlersFor(path);
        if (handlers.empty()) {
            Report(generation, S_FALSE);
            return;
        }
        // Try each registered handler, and each way of giving it the file,
        // until one shows it.
        Failure failure;
        HRESULT hr = E_FAIL;
        for (const CLSID& clsid : handlers) {
            for (int step = kStepMemoryStream; step <= kStepItem; ++step) {
                if (!host_->Current(generation)) return;  // a newer request is queued
                ComPtr<IPreviewHandler> handler;
                ComPtr<IStream> stream;
                bool cannotStart = false;
                hr = CreateHandler(clsid, path, step, handler, stream, failure, cannotStart);
                if (cannotStart) break;
                if (FAILED(hr)) continue;
                ComPtr<IObjectWithSite> site;
                if (frame_ && SUCCEEDED(handler.As(&site))) site->SetSite(frame_.Get());
                RECT rect;
                {
                    std::lock_guard<std::mutex> lock(host_->mutex);
                    rect = host_->rect;
                }
                hr = handler->SetWindow(owner, &rect);
                if (FAILED(hr)) {
                    failure.Note(hr, kStepWindow);
                    if (site) site->SetSite(nullptr);
                    continue;
                }
                handler_ = handler;
                stream_ = stream;
                Colors();
                hr = handler->DoPreview();
                if (SUCCEEDED(hr)) break;
                failure.Note(hr, kStepPreview);
                Unload();
            }
            if (SUCCEEDED(hr)) break;
        }
        if (FAILED(hr)) {
            Unload();
            hr = failure.step ? failure.hr : hr;
            const bool internetZone = HasInternetZone(path);
            std::lock_guard<std::mutex> lock(host_->mutex);
            host_->failedStep = failure.step;
            host_->failedInternetZone = internetZone;
        }
        if (!host_->Current(generation)) {
            Unload();
            return;
        }
        Layout();  // the window may have been resized meanwhile
        Report(generation, hr);
    }

    void Unload() {
        if (handler_) {
            handler_->Unload();
            ComPtr<IObjectWithSite> site;
            if (SUCCEEDED(handler_.As(&site))) site->SetSite(nullptr);
            handler_.Reset();
        }
        stream_.Reset();
    }

    void Layout() {
        if (!handler_) return;
        RECT rect;
        {
            std::lock_guard<std::mutex> lock(host_->mutex);
            rect = host_->rect;
        }
        handler_->SetRect(&rect);
    }

    void Colors() {
        ComPtr<IPreviewHandlerVisuals> visuals;
        if (!handler_ || FAILED(handler_.As(&visuals))) return;
        COLORREF background, text;
        {
            std::lock_guard<std::mutex> lock(host_->mutex);
            background = host_->background;
            text = host_->text;
        }
        visuals->SetBackgroundColor(background);
        visuals->SetTextColor(text);
    }

    std::shared_ptr<DocumentHost> host_;
    HWND window_ = nullptr;
    bool quitting_ = false;
    ComPtr<IPreviewHandler> handler_;
    ComPtr<IStream> stream_;  // kept alive while a stream-based handler previews
    ComPtr<IPreviewHandlerFrame> frame_;
};

struct HostStart {
    std::shared_ptr<DocumentHost> host;
    HINSTANCE instance;
    HANDLE ready;
};

DWORD WINAPI HostThread(void* parameter) {
    std::unique_ptr<HostStart> start(static_cast<HostStart*>(parameter));
    HostWorker worker(start->host);
    worker.Run(start->instance, start->ready);
    return 0;
}

}  // namespace

DocumentView::~DocumentView() { StopHost(); }

HRESULT DocumentView::Create(HWND parent, HINSTANCE instance, HACCEL accelerators, Callbacks callbacks) {
    instance_ = instance;
    accelerators_ = accelerators;
    callbacks_ = std::move(callbacks);
    WNDCLASSEXW wc = {sizeof(wc)};
    if (!GetClassInfoExW(instance, kClassName, &wc)) {
        wc = {sizeof(wc)};
        wc.lpfnWndProc = &DocumentView::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return HRESULT_FROM_WIN32(GetLastError());
    }
    hwnd_ = CreateWindowExW(0, kClassName, nullptr, WS_CHILD | WS_CLIPCHILDREN | WS_TABSTOP, 0, 0, 0, 0, parent,
                            nullptr, instance, this);
    if (!hwnd_) return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

bool DocumentView::StartHost() {
    if (host_ && thread_) return true;
    if (!hwnd_) return false;
    try {
        host_ = std::make_shared<DocumentHost>();
    } catch (...) {
        return false;
    }
    host_->owner = hwnd_;
    host_->accelerators = accelerators_;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) return false;
    HostStart* start = new (std::nothrow) HostStart{host_, instance_, ready};
    if (start) thread_ = CreateThread(nullptr, 0, &HostThread, start, 0, nullptr);
    if (!thread_) delete start;
    else WaitForSingleObject(ready, 5000);  // the thread only creates its window first
    CloseHandle(ready);
    return thread_ != nullptr;
}

void DocumentView::StopHost() {
    if (!host_) return;
    {
        std::lock_guard<std::mutex> lock(host_->mutex);
        host_->owner = nullptr;
        ++host_->generation;
    }
    if (thread_) {
        host_->Post(kHostQuit, 0, 0);
        // A handler that hangs is left to finish on its own.
        WaitAnsweringSent(thread_, 2000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    host_.reset();
}

LRESULT CALLBACK DocumentView::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DocumentView* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<DocumentView*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DocumentView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (msg == WM_NCDESTROY && self) {
        self->StopHost();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return self ? self->HandleMessage(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT DocumentView::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT:
        OnPaint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (host_) {
            {
                std::lock_guard<std::mutex> lock(host_->mutex);
                GetClientRect(hwnd_, &host_->rect);
            }
            host_->Post(kHostLayout, 0, 0);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_SETFOCUS:
        // Keep keyboard focus here so the viewer shortcuts work; a click in
        // the document moves it into the handler.
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd_);
        return 0;
    case WM_APP_DOCUMENT_LOADED:
        OnLoaded(static_cast<unsigned>(wParam), static_cast<HRESULT>(lParam));
        return 0;
    case WM_APP_DOCUMENT_ACCEL:
        return (callbacks_.translateAccelerator && lParam &&
                callbacks_.translateAccelerator(reinterpret_cast<MSG*>(lParam)))
                   ? 1
                   : 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void DocumentView::Show(const std::wstring& path) {
    path_ = path;
    state_ = State::Loading;
    errorInternetZone_ = false;
    focusBefore_ = GetFocus();
    if (StartHost()) {
        const auto& p = theme::Current();
        {
            std::lock_guard<std::mutex> lock(host_->mutex);
            generation_ = ++host_->generation;
            host_->path = path;
            GetClientRect(hwnd_, &host_->rect);
            host_->background = fullscreen_ ? RGB(0, 0, 0) : p.surface;
            host_->text = fullscreen_ ? RGB(220, 220, 220) : p.text;
        }
        if (!host_->Post(kHostLoad, generation_, 0)) {
            state_ = State::Failed;
            error_ = E_FAIL;
            errorStep_ = 0;
        }
    } else {
        state_ = State::Failed;
        error_ = E_OUTOFMEMORY;
        errorStep_ = 0;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (callbacks_.changed) callbacks_.changed();
}

void DocumentView::OnLoaded(unsigned generation, HRESULT hr) {
    if (generation != generation_ || state_ != State::Loading) return;
    if (hr == S_FALSE) {
        state_ = State::NoHandler;
    } else if (FAILED(hr)) {
        state_ = State::Failed;
        error_ = hr;
        {
            std::lock_guard<std::mutex> lock(host_->mutex);
            errorStep_ = host_->failedStep;
            errorInternetZone_ = host_->failedInternetZone;
        }
    } else {
        state_ = State::Shown;
        // The handler may have taken the focus; give it back.
        const HWND now = GetFocus();
        if (takeFocus_) {
            if (IsWindowVisible(hwnd_)) SetFocus(hwnd_);
        } else if (focusBefore_ && IsWindow(focusBefore_) && (!now || now == hwnd_ || IsChild(hwnd_, now))) {
            SetFocus(focusBefore_);
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (callbacks_.changed) callbacks_.changed();
}

void DocumentView::Clear() {
    path_.clear();
    state_ = State::Empty;
    if (host_) {
        {
            std::lock_guard<std::mutex> lock(host_->mutex);
            generation_ = ++host_->generation;
        }
        host_->Post(kHostUnload, 0, 0);
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void DocumentView::Release() {
    const bool had = state_ != State::Empty;
    Clear();
    if (!had || !host_ || !thread_) return;
    // Wait until the handler has let go of the file, but not for long.
    std::shared_ptr<Signal> signal = std::make_shared<Signal>();
    if (!signal->event) return;
    auto* payload = new (std::nothrow) std::shared_ptr<Signal>(signal);
    if (!payload) return;
    if (!host_->Post(kHostUnload, 0, reinterpret_cast<LPARAM>(payload))) {
        delete payload;
        return;
    }
    WaitAnsweringSent(signal->event, 3000);
}

void DocumentView::ApplyColors() {
    if (!host_) return;
    const auto& p = theme::Current();
    {
        std::lock_guard<std::mutex> lock(host_->mutex);
        host_->background = fullscreen_ ? RGB(0, 0, 0) : p.surface;
        host_->text = fullscreen_ ? RGB(220, 220, 220) : p.text;
    }
    host_->Post(kHostColors, 0, 0);
}

void DocumentView::SetFullscreen(bool fullscreen) {
    fullscreen_ = fullscreen;
    ApplyColors();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void DocumentView::OnThemeChanged() {
    ApplyColors();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void DocumentView::OnPaint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const auto& p = theme::Current();
    HBRUSH brush = CreateSolidBrush(fullscreen_ ? RGB(0, 0, 0) : p.surface);
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
    UINT message = 0;
    switch (state_) {
    case State::Loading: message = IDS_VIEWER_LOADING; break;
    case State::NoHandler: message = IDS_DOC_NO_HANDLER; break;
    case State::Failed: message = IDS_DOC_ERROR; break;
    default: break;
    }
    if (message) {
        std::wstring text = LoadStr(message);
        if (state_ == State::Failed) {
            // The code helps tell a missing component from a damaged file.
            wchar_t code[32] = {};
            swprintf(code, 32, L"0x%08lX #%d", static_cast<unsigned long>(error_), errorStep_);
            const auto at = text.find(L"%s");
            if (at != std::wstring::npos) text.replace(at, 2, code);
            if (errorInternetZone_) {
                text += L"\n\n";
                text += LoadStr(IDS_DOC_INTERNET_ZONE);
            }
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, fullscreen_ ? RGB(220, 220, 220) : p.text);
        HGDIOBJ old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        RECT box = rc;
        InflateRect(&box, -24, -24);
        RECT measured = box;
        DrawTextW(dc, text.c_str(), -1, &measured, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        OffsetRect(&box, 0, std::max(0L, ((box.bottom - box.top) - (measured.bottom - measured.top)) / 2));
        DrawTextW(dc, text.c_str(), -1, &box, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
    }
    EndPaint(hwnd_, &ps);
}
