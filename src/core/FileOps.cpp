#include "core/FileOps.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace FileOps {

bool IsValidFileName(const std::wstring& name) noexcept {
    if (name.empty() || name.size() > 255) return false;
    for (wchar_t c : name) {
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) return false;
    }
    if (name.back() == L'.' || name.back() == L' ') return false;
    // Reserved device names, with or without an extension.
    std::wstring stem = name.substr(0, name.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    for (auto& c : stem) c = static_cast<wchar_t>(std::towupper(c));
    static const wchar_t* const reserved[] = {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4",
                                              L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2",
                                              L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    for (const wchar_t* r : reserved) {
        if (stem == r) return false;
    }
    return true;
}

std::wstring UniqueChildName(const std::wstring& folder, const std::wstring& base) {
    const std::wstring prefix = (!folder.empty() && folder.back() != L'\\') ? folder + L'\\' : folder;
    if (GetFileAttributesW((prefix + base).c_str()) == INVALID_FILE_ATTRIBUTES) return base;
    for (int i = 2; i < 10000; ++i) {
        const std::wstring candidate = base + L" (" + std::to_wstring(i) + L")";
        if (GetFileAttributesW((prefix + candidate).c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;
    }
    return base;
}

HRESULT Rename(const std::wstring& from, const std::wstring& to) noexcept {
    if (from == to) return S_FALSE;
    // MoveFileEx without REPLACE_EXISTING refuses to overwrite; a case-only
    // change refers to the same file, which the file system allows.
    if (!MoveFileExW(from.c_str(), to.c_str(), 0)) return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

namespace {

HRESULT Run(HWND owner, const std::vector<std::wstring>& paths, const std::wstring* destination, bool move,
            bool recycle) {
    ComPtr<IFileOperation> op;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
    if (FAILED(hr)) return hr;
    hr = op->SetOwnerWindow(owner);
    DWORD flags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;
    if (recycle) flags |= FOFX_RECYCLEONDELETE | FOF_WANTNUKEWARNING;
    if (SUCCEEDED(hr)) hr = op->SetOperationFlags(flags);
    ComPtr<IShellItem> target;
    if (SUCCEEDED(hr) && destination) {
        hr = SHCreateItemFromParsingName(destination->c_str(), nullptr, IID_PPV_ARGS(&target));
    }
    for (const auto& path : paths) {
        if (FAILED(hr)) break;
        ComPtr<IShellItem> item;
        hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
        if (FAILED(hr)) break;
        if (recycle) {
            hr = op->DeleteItem(item.Get(), nullptr);
        } else if (move) {
            hr = op->MoveItem(item.Get(), target.Get(), nullptr, nullptr);
        } else {
            hr = op->CopyItem(item.Get(), target.Get(), nullptr, nullptr);
        }
    }
    if (SUCCEEDED(hr)) hr = op->PerformOperations();
    if (SUCCEEDED(hr)) {
        BOOL aborted = FALSE;
        if (SUCCEEDED(op->GetAnyOperationsAborted(&aborted)) && aborted) hr = S_FALSE;
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) hr = S_FALSE;
    return hr;
}

// SHFileOperation fallback for systems without a working IFileOperation.
HRESULT RunLegacy(HWND owner, const std::vector<std::wstring>& paths, const std::wstring* destination, bool move,
                  bool recycle) {
    std::wstring from;
    for (const auto& path : paths) {
        from += path;
        from.push_back(L'\0');
    }
    from.push_back(L'\0');
    std::wstring to;
    if (destination) {
        to = *destination;
        to.push_back(L'\0');
        to.push_back(L'\0');
    }
    SHFILEOPSTRUCTW op = {};
    op.hwnd = owner;
    op.wFunc = recycle ? FO_DELETE : (move ? FO_MOVE : FO_COPY);
    op.pFrom = from.c_str();
    op.pTo = destination ? to.c_str() : nullptr;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | (recycle ? FOF_WANTNUKEWARNING : 0);
    const int result = SHFileOperationW(&op);
    if (op.fAnyOperationsAborted) return S_FALSE;
    return result == 0 ? S_OK : HRESULT_FROM_WIN32(static_cast<DWORD>(result));
}

}  // namespace

HRESULT CopyOrMove(HWND owner, const std::vector<std::wstring>& paths, const std::wstring& destination,
                   bool move) noexcept {
    if (paths.empty()) return S_FALSE;
    try {
        const HRESULT hr = Run(owner, paths, &destination, move, false);
        return (hr == E_NOTIMPL || hr == REGDB_E_CLASSNOTREG) ? RunLegacy(owner, paths, &destination, move, false) : hr;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HRESULT Recycle(HWND owner, const std::vector<std::wstring>& paths) noexcept {
    if (paths.empty()) return S_FALSE;
    try {
        const HRESULT hr = Run(owner, paths, nullptr, false, true);
        return (hr == E_NOTIMPL || hr == REGDB_E_CLASSNOTREG) ? RunLegacy(owner, paths, nullptr, false, true) : hr;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

}  // namespace FileOps
