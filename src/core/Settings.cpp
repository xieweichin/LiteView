#include "core/Settings.h"

#include <shlobj.h>

#include <atomic>
#include <cerrno>
#include <climits>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <new>
#include <vector>

namespace {

class FileHandle {
public:
    explicit FileHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~FileHandle() { Close(); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    HANDLE Get() const { return value_; }
    bool Valid() const { return value_ != INVALID_HANDLE_VALUE; }
    void Close() {
        if (Valid()) CloseHandle(value_);
        value_ = INVALID_HANDLE_VALUE;
    }
private:
    HANDLE value_;
};

HRESULT LastError() {
    const DWORD error = GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

std::wstring Join(const std::wstring& directory, const wchar_t* name) {
    if (directory.empty()) return name;
    const wchar_t last = directory.back();
    return directory + ((last == L'\\' || last == L'/') ? L"" : L"\\") + name;
}

bool ValidKey(const std::wstring& section, const std::wstring& key) {
    return !section.empty() && !key.empty() &&
           section.find_first_of(L"[]\r\n") == std::wstring::npos &&
           key.find_first_of(L"=\r\n") == std::wstring::npos &&
           section.find(L'\0') == std::wstring::npos && key.find(L'\0') == std::wstring::npos;
}

HRESULT WriteBytes(HANDLE handle, const void* buffer, DWORD length) {
    const auto* data = static_cast<const BYTE*>(buffer);
    DWORD offset = 0;
    while (offset < length) {
        DWORD written = 0;
        if (!WriteFile(handle, data + offset, length - offset, &written, nullptr)) return LastError();
        if (written == 0) return E_FAIL;
        offset += written;
    }
    return S_OK;
}

HRESULT DecodeExistingFile(const std::vector<BYTE>& bytes, std::wstring& text) {
    if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        if ((bytes.size() % 2) != 0) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        text.clear();
        text.reserve((bytes.size() - 2) / 2);
        for (std::size_t i = 2; i < bytes.size(); i += 2) {
            text.push_back(static_cast<wchar_t>((static_cast<unsigned>(bytes[i]) << 8) | bytes[i + 1]));
        }
        return S_OK;
    }
    const bool utf8Bom = bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF;
    const std::size_t start = utf8Bom ? 3 : 0;
    const int length = static_cast<int>(bytes.size() - start);
    if (length == 0) {
        text.clear();
        return S_OK;
    }
    const UINT codePage = utf8Bom ? CP_UTF8 : CP_ACP;
    const DWORD flags = utf8Bom ? MB_ERR_INVALID_CHARS : 0;
    const char* input = reinterpret_cast<const char*>(bytes.data() + start);
    const int needed = MultiByteToWideChar(codePage, flags, input, length, nullptr, 0);
    if (needed == 0) return LastError();
    text.resize(static_cast<std::size_t>(needed));
    if (MultiByteToWideChar(codePage, flags, input, length, text.data(), needed) == 0) return LastError();
    return S_OK;
}

std::wstring TrimIni(const std::wstring& text) {
    const auto first = text.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    const auto last = text.find_last_not_of(L" \t");
    return text.substr(first, last - first + 1);
}

std::wstring ReadDecodedIni(const std::wstring& text, const std::wstring& section,
                            const std::wstring& key, const std::wstring& fallback) {
    bool inSection = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find_first_of(L"\r\n", start);
        const std::wstring line = TrimIni(text.substr(start, end == std::wstring::npos ? end : end - start));
        if (!line.empty() && line.front() != L';' && line.front() != L'#') {
            if (line.front() == L'[') {
                const auto close = line.find(L']');
                inSection = close != std::wstring::npos &&
                            _wcsicmp(TrimIni(line.substr(1, close - 1)).c_str(), section.c_str()) == 0;
            } else if (inSection) {
                const auto equals = line.find(L'=');
                if (equals != std::wstring::npos &&
                    _wcsicmp(TrimIni(line.substr(0, equals)).c_str(), key.c_str()) == 0) {
                    std::wstring value = TrimIni(line.substr(equals + 1));
                    if (value.size() >= 2 && (value.front() == L'\'' || value.front() == L'"') &&
                        value.back() == value.front()) {
                        value = value.substr(1, value.size() - 2);
                    }
                    return value;
                }
            }
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
        if (text[end] == L'\r' && start < text.size() && text[start] == L'\n') ++start;
    }
    return fallback;
}

std::atomic<unsigned long> temporarySequence{0};

}  // namespace

HRESULT Settings::ForCurrentProcess(Settings& out) noexcept {
    try {
        std::vector<wchar_t> executable(512);
        for (;;) {
            SetLastError(ERROR_SUCCESS);
            const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                                    static_cast<DWORD>(executable.size()));
            if (length == 0) return LastError();
            if (length < executable.size() - 1) {
                std::wstring path(executable.data(), length);
                const auto slash = path.find_last_of(L"\\/");
                if (slash == std::wstring::npos) return E_FAIL;
                const std::wstring directory = path.substr(0, slash);
                const DWORD portable = GetFileAttributesW(Join(directory, L"portable.ini").c_str());
                if (portable != INVALID_FILE_ATTRIBUTES) {
                    return FromDirectories(directory, L"", out);
                }
                const DWORD portableError = GetLastError();
                if (portableError != ERROR_FILE_NOT_FOUND && portableError != ERROR_PATH_NOT_FOUND) {
                    return HRESULT_FROM_WIN32(portableError);
                }
                PWSTR raw = nullptr;
                const HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw);
                if (FAILED(hr)) return hr;
                std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> roaming(raw, &CoTaskMemFree);
                return FromDirectories(directory, roaming.get(), out);
            }
            if (executable.size() >= 32768) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
            executable.resize(executable.size() * 2);
        }
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT Settings::FromDirectories(const std::wstring& executableDirectory,
                                  const std::wstring& roamingDirectory,
                                  Settings& out) noexcept {
    if (executableDirectory.empty()) return E_INVALIDARG;
    try {
        const std::wstring portablePath = Join(executableDirectory, L"portable.ini");
        const DWORD attributes = GetFileAttributesW(portablePath.c_str());
        const bool portable = attributes != INVALID_FILE_ATTRIBUTES;
        if (portable && (attributes & FILE_ATTRIBUTE_DIRECTORY)) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
        if (!portable) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return HRESULT_FROM_WIN32(error);
            if (roamingDirectory.empty()) return E_INVALIDARG;
        }
        std::lock_guard<std::mutex> lock(out.mutex_);
        out.portable_ = portable;
        out.directory_ = portable ? executableDirectory : Join(roamingDirectory, L"LiteView");
        out.path_ = portable ? portablePath : Join(out.directory_, L"settings.ini");
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT Settings::EnsureUnicodeFile() const {
    if (path_.empty()) return E_UNEXPECTED;
    if (!portable_ && !CreateDirectoryW(directory_.c_str(), nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) return HRESULT_FROM_WIN32(error);
        const DWORD attributes = GetFileAttributesW(directory_.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
        }
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        FileHandle source(CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!source.Valid()) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND) return HRESULT_FROM_WIN32(error);
            FileHandle created(CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                           nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!created.Valid()) {
                if (GetLastError() == ERROR_FILE_EXISTS) continue;
                return LastError();
            }
            const BYTE bom[] = {0xFF, 0xFE};
            const HRESULT hr = WriteBytes(created.Get(), bom, sizeof(bom));
            created.Close();
            if (FAILED(hr)) DeleteFileW(path_.c_str());
            return hr;
        }

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(source.Get(), &size)) return LastError();
        if (size.QuadPart < 0 || size.QuadPart > 8 * 1024 * 1024) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        std::vector<BYTE> bytes(static_cast<std::size_t>(size.QuadPart));
        DWORD offset = 0;
        while (offset < bytes.size()) {
            DWORD read = 0;
            if (!ReadFile(source.Get(), bytes.data() + offset,
                          static_cast<DWORD>(bytes.size() - offset), &read, nullptr)) return LastError();
            if (read == 0) return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
            offset += read;
        }
        if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
            return (bytes.size() % 2 == 0) ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }

        std::wstring text;
        HRESULT hr = DecodeExistingFile(bytes, text);
        if (FAILED(hr)) return hr;
        std::wstring temporary;
        HANDLE temporaryHandle = INVALID_HANDLE_VALUE;
        for (int suffix = 0; suffix < 128; ++suffix) {
            temporary = path_ + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                        std::to_wstring(temporarySequence.fetch_add(1));
            temporaryHandle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
            if (temporaryHandle != INVALID_HANDLE_VALUE) break;
            if (GetLastError() != ERROR_FILE_EXISTS) return LastError();
        }
        if (temporaryHandle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
        FileHandle output(temporaryHandle);
        const BYTE bom[] = {0xFF, 0xFE};
        hr = WriteBytes(output.Get(), bom, sizeof(bom));
        if (SUCCEEDED(hr) && !text.empty()) {
            hr = WriteBytes(output.Get(), text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)));
        }
        if (SUCCEEDED(hr) && !FlushFileBuffers(output.Get())) hr = LastError();
        output.Close();
        // ReplaceFile needs to open the original with write access. Close our
        // read handle first; it deliberately denied writes while decoding.
        source.Close();
        if (SUCCEEDED(hr) && !ReplaceFileW(path_.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr)) {
            hr = LastError();
        }
        if (FAILED(hr)) DeleteFileW(temporary.c_str());
        if (SUCCEEDED(hr)) WritePrivateProfileStringW(nullptr, nullptr, nullptr, path_.c_str());
        return hr;
    }
    return HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
}

HRESULT Settings::ReadString(const std::wstring& section, const std::wstring& key,
                             const std::wstring& fallback, std::wstring& value) const noexcept {
    if (!ValidKey(section, key)) return E_INVALIDARG;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (path_.empty()) return E_UNEXPECTED;
        FileHandle source(CreateFileW(path_.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!source.Valid()) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                value = fallback;
                return S_OK;
            }
            return HRESULT_FROM_WIN32(error);
        }
        BYTE prefix[3]{};
        DWORD prefixSize = 0;
        if (!ReadFile(source.Get(), prefix, sizeof(prefix), &prefixSize, nullptr)) return LastError();
        const bool utf8 = prefixSize >= 3 && prefix[0] == 0xEF && prefix[1] == 0xBB && prefix[2] == 0xBF;
        const bool utf16be = prefixSize >= 2 && prefix[0] == 0xFE && prefix[1] == 0xFF;
        if (utf8 || utf16be) {
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(source.Get(), &size)) return LastError();
            if (size.QuadPart < 0 || size.QuadPart > 8 * 1024 * 1024) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            LARGE_INTEGER zero{};
            if (!SetFilePointerEx(source.Get(), zero, nullptr, FILE_BEGIN)) return LastError();
            std::vector<BYTE> bytes(static_cast<std::size_t>(size.QuadPart));
            DWORD offset = 0;
            while (offset < bytes.size()) {
                DWORD read = 0;
                if (!ReadFile(source.Get(), bytes.data() + offset,
                              static_cast<DWORD>(bytes.size() - offset), &read, nullptr)) return LastError();
                if (read == 0) return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
                offset += read;
            }
            std::wstring text;
            const HRESULT hr = DecodeExistingFile(bytes, text);
            if (FAILED(hr)) return hr;
            value = ReadDecodedIni(text, section, key, fallback);
            return S_OK;
        }
        source.Close();
        std::vector<wchar_t> buffer(256);
        for (;;) {
            const DWORD length = GetPrivateProfileStringW(section.c_str(), key.c_str(), fallback.c_str(),
                                                           buffer.data(), static_cast<DWORD>(buffer.size()),
                                                           path_.c_str());
            if (length < buffer.size() - 1) {
                value.assign(buffer.data(), length);
                return S_OK;
            }
            if (buffer.size() >= 1024 * 1024) return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
            buffer.resize(buffer.size() * 2);
        }
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT Settings::WriteString(const std::wstring& section, const std::wstring& key,
                              const std::wstring& value) noexcept {
    if (!ValidKey(section, key) || value.find_first_of(L"\r\n") != std::wstring::npos ||
        value.find(L'\0') != std::wstring::npos) return E_INVALIDARG;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const HRESULT hr = EnsureUnicodeFile();
        if (FAILED(hr)) return hr;
        SetLastError(ERROR_SUCCESS);
        if (!WritePrivateProfileStringW(section.c_str(), key.c_str(), value.c_str(), path_.c_str())) return LastError();
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT Settings::ReadInt(const std::wstring& section, const std::wstring& key,
                          int fallback, int& value) const noexcept {
    try {
        value = fallback;
        std::wstring text;
        const HRESULT hr = ReadString(section, key, std::to_wstring(fallback), text);
        if (FAILED(hr)) return hr;
        wchar_t* end = nullptr;
        errno = 0;
        const long long parsed = std::wcstoll(text.c_str(), &end, 10);
        if (errno == ERANGE || end == text.c_str() || *end != L'\0' || parsed < INT_MIN || parsed > INT_MAX) {
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        value = static_cast<int>(parsed);
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT Settings::WriteInt(const std::wstring& section, const std::wstring& key, int value) noexcept {
    try { return WriteString(section, key, std::to_wstring(value)); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}

HRESULT Settings::ReadBool(const std::wstring& section, const std::wstring& key,
                           bool fallback, bool& value) const noexcept {
    value = fallback;
    std::wstring text;
    const HRESULT hr = ReadString(section, key, fallback ? L"1" : L"0", text);
    if (FAILED(hr)) return hr;
    for (auto& character : text) character = static_cast<wchar_t>(std::towlower(character));
    if (text == L"1" || text == L"true") { value = true; return S_OK; }
    if (text == L"0" || text == L"false") { value = false; return S_OK; }
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
}

HRESULT Settings::WriteBool(const std::wstring& section, const std::wstring& key, bool value) noexcept {
    return WriteString(section, key, value ? L"1" : L"0");
}
