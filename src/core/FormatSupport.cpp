#include "core/FormatSupport.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <cwchar>
#include <new>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {

struct PackInfo {
    const wchar_t* name;
    const wchar_t* storeUri;
    const wchar_t* const* extensions;
};

const wchar_t* const kHeif[] = {L".heic", L".heif", L".hif", nullptr};
const wchar_t* const kWebP[] = {L".webp", nullptr};
const wchar_t* const kAvif[] = {L".avif", nullptr};
// Camera RAW formats handled by Microsoft's Raw Image Extension.
const wchar_t* const kRaw[] = {
    L".3fr", L".ari", L".arw", L".bay", L".cap", L".cr2", L".cr3", L".crw", L".dcr", L".dcs",
    L".dng", L".drf", L".eip", L".erf", L".fff", L".iiq", L".k25", L".kdc", L".mef", L".mos",
    L".mrw", L".nef", L".nrw", L".orf", L".ori", L".pef", L".ptx", L".raf", L".rw2", L".rwl",
    L".sr2", L".srf", L".srw", L".x3f", nullptr};
const wchar_t* const kJpegXl[] = {L".jxl", nullptr};

// Order matches CodecPack.
const PackInfo kPacks[] = {
    {L"HEIF", L"ms-windows-store://pdp/?ProductId=9PMMSR1CGPWG", kHeif},
    {L"WebP", L"ms-windows-store://pdp/?ProductId=9PG2DK419DRG", kWebP},
    {L"AVIF", L"ms-windows-store://pdp/?ProductId=9MVZQVXJBQ9V", kAvif},
    {L"RAW", L"ms-windows-store://pdp/?ProductId=9NCTDW2W1BH8", kRaw},
    {L"JPEG XL", L"ms-windows-store://search/?query=JPEG%20XL%20Image%20Extension", kJpegXl},
};
static_assert(sizeof(kPacks) / sizeof(kPacks[0]) == static_cast<std::size_t>(CodecPack::Count),
              "kPacks must list every CodecPack");

const PackInfo* Info(CodecPack pack) noexcept {
    const auto index = static_cast<std::size_t>(pack);
    return index < static_cast<std::size_t>(CodecPack::Count) ? &kPacks[index] : nullptr;
}

}  // namespace

HRESULT DecoderExtensions(std::vector<std::wstring>& extensions) noexcept {
    try {
        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(hr)) return hr;
        ComPtr<IEnumUnknown> enumerator;
        hr = factory->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault,
                                                 enumerator.GetAddressOf());
        if (FAILED(hr)) return hr;
        for (;;) {
            ComPtr<IUnknown> unknown;
            ULONG fetched = 0;
            hr = enumerator->Next(1, unknown.GetAddressOf(), &fetched);
            if (FAILED(hr)) return hr;
            if (hr == S_FALSE || fetched == 0) break;
            ComPtr<IWICBitmapDecoderInfo> decoder;
            if (FAILED(unknown.As(&decoder))) continue;
            UINT needed = 0;
            hr = decoder->GetFileExtensions(0, nullptr, &needed);
            if (FAILED(hr) && hr != WINCODEC_ERR_INSUFFICIENTBUFFER) continue;
            if (needed == 0 || needed > 65536) continue;
            std::vector<wchar_t> buffer(needed);
            if (FAILED(decoder->GetFileExtensions(needed, buffer.data(), &needed))) continue;
            std::wstring values(buffer.data());
            std::size_t start = 0;
            while (start <= values.size()) {
                const std::size_t comma = values.find_first_of(L",;", start);
                std::wstring extension = values.substr(start, comma == std::wstring::npos ? comma : comma - start);
                const auto first = extension.find_first_not_of(L" \t\r\n");
                const auto last = extension.find_last_not_of(L" \t\r\n");
                if (first != std::wstring::npos) {
                    extension = extension.substr(first, last - first + 1);
                    if (!extension.empty() && extension.front() == L'.') extensions.push_back(std::move(extension));
                }
                if (comma == std::wstring::npos) break;
                start = comma + 1;
            }
        }
        return extensions.empty() ? WINCODEC_ERR_COMPONENTNOTFOUND : S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

bool HasExtension(const std::vector<std::wstring>& extensions, const wchar_t* extension) noexcept {
    if (!extension || !*extension) return false;
    for (const auto& supported : extensions) {
        if (_wcsicmp(extension, supported.c_str()) == 0) return true;
    }
    return false;
}

bool CodecPackForExtension(const wchar_t* extension, CodecPack& pack) noexcept {
    if (!extension || !*extension) return false;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(CodecPack::Count); ++i) {
        for (const wchar_t* const* known = kPacks[i].extensions; *known; ++known) {
            if (_wcsicmp(extension, *known) == 0) {
                pack = static_cast<CodecPack>(i);
                return true;
            }
        }
    }
    return false;
}

std::uint32_t MissingCodecPacks(const std::vector<std::wstring>& extensions) noexcept {
    std::uint32_t mask = 0;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(CodecPack::Count); ++i) {
        bool any = false;
        for (const wchar_t* const* known = kPacks[i].extensions; *known && !any; ++known)
            any = HasExtension(extensions, *known);
        if (!any) mask |= CodecPackBit(static_cast<CodecPack>(i));
    }
    return mask;
}

CodecPack FirstCodecPack(std::uint32_t mask) noexcept {
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(CodecPack::Count); ++i) {
        if (mask & CodecPackBit(static_cast<CodecPack>(i))) return static_cast<CodecPack>(i);
    }
    return CodecPack::Count;
}

bool IsDocumentExtension(const wchar_t* extension) noexcept {
    static const wchar_t* const kDocuments[] = {
        L".doc", L".docx", L".docm", L".dot", L".dotx", L".rtf", L".xls", L".xlsx", L".xlsm", L".xlsb", L".xlt",
        L".xltx", L".ppt", L".pptx", L".pptm", L".pps", L".ppsx", L".pot", L".potx", L".odt", L".ods", L".odp",
        L".vsd", L".vsdx", L".pub", L".msg", L".eml", L".xps", L".oxps", L".eps", L".ps"};
    if (!extension || !*extension) return false;
    for (const wchar_t* known : kDocuments) {
        if (_wcsicmp(extension, known) == 0) return true;
    }
    return false;
}

bool IsVideoExtension(const wchar_t* extension) noexcept {
    static const wchar_t* const kVideos[] = {L".mp4", L".m4v", L".mov", L".avi", L".wmv", L".asf", L".mkv",
                                            L".webm", L".mpg", L".mpeg", L".m2ts", L".mts", L".3gp", L".3g2"};
    if (!extension || !*extension) return false;
    for (const wchar_t* known : kVideos) {
        if (_wcsicmp(extension, known) == 0) return true;
    }
    return false;
}

std::wstring FormatMediaTime(long long hundredNanoseconds) {
    long long seconds = hundredNanoseconds > 0 ? hundredNanoseconds / 10000000 : 0;
    const long long hours = seconds / 3600;
    const long long minutes = seconds / 60 % 60;
    seconds %= 60;
    wchar_t text[32] = {};
    if (hours > 0) swprintf(text, 32, L"%lld:%02lld:%02lld", hours, minutes, seconds);
    else swprintf(text, 32, L"%lld:%02lld", minutes, seconds);
    return text;
}

const wchar_t* CodecPackName(CodecPack pack) noexcept {
    const PackInfo* info = Info(pack);
    return info ? info->name : L"";
}

const wchar_t* CodecPackStoreUri(CodecPack pack) noexcept {
    const PackInfo* info = Info(pack);
    return info ? info->storeUri : L"";
}
