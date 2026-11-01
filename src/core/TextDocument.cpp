#include "core/TextDocument.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cwchar>
#include <new>
#include <vector>

namespace {

const wchar_t* const kTextExtensions[] = {
    L".txt", L".text", L".md", L".markdown", L".log", L".csv", L".tsv", L".json", L".xml", L".yaml",
    L".yml", L".toml", L".ini", L".cfg", L".conf", L".properties", L".reg", L".srt", L".nfo",
    L".c", L".h", L".cpp", L".hpp", L".cc", L".cxx", L".hh", L".cs", L".java", L".kt", L".swift",
    L".go", L".rs", L".py", L".rb", L".php", L".pl", L".lua", L".js", L".mjs", L".ts", L".jsx",
    L".tsx", L".css", L".scss", L".html", L".htm", L".vue", L".sql", L".sh", L".bat", L".cmd",
    L".ps1", L".vb", L".m", L".r", L".cmake", L".gradle", L".diff", L".patch", L".tex"};

struct Handle {
    explicit Handle(HANDLE value) : value(value) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    HANDLE value;
};

// Length of the longest prefix of a UTF-8 buffer that does not end inside a
// multi-byte sequence.
std::size_t Utf8Boundary(const char* bytes, std::size_t size) {
    std::size_t back = 0;
    while (back < size && back < 4) {
        const auto c = static_cast<unsigned char>(bytes[size - 1 - back]);
        if ((c & 0xC0) != 0x80) {
            // c starts a sequence (or is ASCII): is that sequence complete?
            const std::size_t need = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
            return back + 1 >= need ? size : size - back - 1;
        }
        ++back;
    }
    return size;
}

HRESULT Widen(UINT codePage, DWORD flags, const char* bytes, std::size_t size, std::wstring& out) {
    out.clear();
    if (size == 0) return S_OK;
    if (size > INT_MAX) return E_INVALIDARG;
    const int needed = MultiByteToWideChar(codePage, flags, bytes, static_cast<int>(size), nullptr, 0);
    if (needed <= 0) return HRESULT_FROM_WIN32(GetLastError());
    out.resize(static_cast<std::size_t>(needed));
    if (MultiByteToWideChar(codePage, flags, bytes, static_cast<int>(size), &out[0], needed) != needed)
        return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

void FromUtf16(const char* bytes, std::size_t size, bool bigEndian, bool cut, std::wstring& out) {
    std::size_t units = size / 2;
    out.resize(units);
    for (std::size_t i = 0; i < units; ++i) {
        const auto a = static_cast<unsigned char>(bytes[2 * i]);
        const auto b = static_cast<unsigned char>(bytes[2 * i + 1]);
        out[i] = static_cast<wchar_t>(bigEndian ? (a << 8) | b : (b << 8) | a);
    }
    // Drop a high surrogate whose partner was cut off.
    if (cut && !out.empty() && out.back() >= 0xD800 && out.back() <= 0xDBFF) out.pop_back();
}

// UTF-16 without a BOM: many NULs in one byte position of each pair.
bool LooksUtf16(const char* bytes, std::size_t size, bool& bigEndian) {
    const std::size_t sample = std::min<std::size_t>(size, 4096) & ~std::size_t{1};
    if (sample < 4) return false;
    std::size_t evenZero = 0, oddZero = 0;
    for (std::size_t i = 0; i < sample; i += 2) {
        if (bytes[i] == 0) ++evenZero;
        if (bytes[i + 1] == 0) ++oddZero;
    }
    const std::size_t pairs = sample / 2;
    if (oddZero * 10 >= pairs * 4 && evenZero * 10 < pairs) { bigEndian = false; return true; }
    if (evenZero * 10 >= pairs * 4 && oddZero * 10 < pairs) { bigEndian = true; return true; }
    return false;
}

// CRLF line ends, NULs replaced so an EDIT control shows the whole text.
std::wstring Normalize(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size() + text.size() / 32);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'\r') {
            out += L"\r\n";
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
        } else if (c == L'\n') {
            out += L"\r\n";
        } else if (c == L'\0') {
            out += L'\x2400';  // ␀
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace

bool IsTextExtension(const wchar_t* extension) noexcept {
    if (!extension || !*extension) return false;
    for (const wchar_t* known : kTextExtensions) {
        if (_wcsicmp(extension, known) == 0) return true;
    }
    return false;
}

HRESULT DecodeText(const char* bytes, std::size_t size, bool cut, TextDocument& out) noexcept {
    try {
        const auto* u = reinterpret_cast<const unsigned char*>(bytes);
        std::wstring wide;
        TextEncoding encoding = TextEncoding::Utf8;
        bool bigEndian = false;
        if (size >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) {
            encoding = TextEncoding::Utf8Bom;
            const std::size_t end = cut ? 3 + Utf8Boundary(bytes + 3, size - 3) : size;
            HRESULT hr = Widen(CP_UTF8, 0, bytes + 3, end - 3, wide);
            if (FAILED(hr)) return hr;
        } else if (size >= 2 && ((u[0] == 0xFF && u[1] == 0xFE) || (u[0] == 0xFE && u[1] == 0xFF))) {
            bigEndian = u[0] == 0xFE;
            encoding = bigEndian ? TextEncoding::Utf16Be : TextEncoding::Utf16Le;
            FromUtf16(bytes + 2, size - 2, bigEndian, cut, wide);
        } else if (LooksUtf16(bytes, size, bigEndian)) {
            encoding = bigEndian ? TextEncoding::Utf16Be : TextEncoding::Utf16Le;
            FromUtf16(bytes, size, bigEndian, cut, wide);
        } else {
            const std::size_t end = cut ? Utf8Boundary(bytes, size) : size;
            if (SUCCEEDED(Widen(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, end, wide))) {
                encoding = TextEncoding::Utf8;
            } else {
                // Legacy files: Big5 on Traditional Chinese Windows, etc. A
                // cut lead byte at the end becomes one stray character.
                encoding = TextEncoding::Ansi;
                HRESULT hr = Widen(CP_ACP, 0, bytes, size, wide);
                if (FAILED(hr)) return hr;
            }
        }
        out.text = Normalize(wide);
        out.encoding = encoding;
        out.truncated = cut;
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT LoadTextFile(const std::wstring& path, std::size_t maxBytes, TextDocument& out) noexcept {
    out = {};
    if (path.empty() || maxBytes == 0 || maxBytes > 0x40000000) return E_INVALIDARG;
    try {
        Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (file.value == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.value, &size)) return HRESULT_FROM_WIN32(GetLastError());
        const auto fileSize = static_cast<std::uint64_t>(size.QuadPart);
        const std::size_t want = fileSize > maxBytes ? maxBytes : static_cast<std::size_t>(fileSize);
        std::vector<char> bytes(want);
        std::size_t got = 0;
        while (got < want) {
            DWORD read = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(want - got, 1u << 20));
            if (!ReadFile(file.value, bytes.data() + got, chunk, &read, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
            if (read == 0) break;
            got += read;
        }
        const HRESULT hr = DecodeText(bytes.data(), got, fileSize > got, out);
        out.fileSize = fileSize;
        return hr;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

const wchar_t* TextEncodingName(TextEncoding encoding) noexcept {
    switch (encoding) {
    case TextEncoding::Utf8: return L"UTF-8";
    case TextEncoding::Utf8Bom: return L"UTF-8 BOM";
    case TextEncoding::Utf16Le: return L"UTF-16 LE";
    case TextEncoding::Utf16Be: return L"UTF-16 BE";
    case TextEncoding::Ansi: return L"ANSI";
    }
    return L"";
}
