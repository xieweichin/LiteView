#include "core/Metadata.h"

#include <propvarutil.h>
#include <wrl/client.h>

#include <initializer_list>
#include <new>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {

struct Property {
    PROPVARIANT value{};
    ~Property() { PropVariantClear(&value); }
};

bool Query(IWICMetadataQueryReader* reader, std::initializer_list<const wchar_t*> paths,
           Property& property) {
    for (const wchar_t* path : paths) {
        PropVariantClear(&property.value);
        if (SUCCEEDED(reader->GetMetadataByName(path, &property.value))) return true;
    }
    return false;
}

std::wstring Text(const PROPVARIANT& property) {
    if (property.vt == VT_LPWSTR && property.pwszVal) return property.pwszVal;
    if (property.vt != VT_LPSTR || !property.pszVal) return {};
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int length = MultiByteToWideChar(codePage, flags, property.pszVal, -1, nullptr, 0);
    if (!length) {
        codePage = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(codePage, flags, property.pszVal, -1, nullptr, 0);
    }
    if (length <= 1) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (!MultiByteToWideChar(codePage, flags, property.pszVal, -1, result.data(), length)) return {};
    result.pop_back();
    return result;
}

std::optional<UINT> Integer(const PROPVARIANT& property) {
    switch (property.vt) {
    case VT_UI1: return property.bVal;
    case VT_UI2: return property.uiVal;
    case VT_UI4: return property.ulVal;
    case VT_VECTOR | VT_UI2:
        if (property.caui.cElems && property.caui.pElems) return property.caui.pElems[0];
        break;
    default: break;
    }
    return {};
}

std::optional<double> Rational(const PROPVARIANT& property) {
    if (property.vt == VT_R8) return property.dblVal;
    if (property.vt == VT_R4) return property.fltVal;
    if (property.vt != VT_UI8 || property.uhVal.HighPart == 0) return {};
    // Native WIC EXIF query results expose the numerator in LowPart and the
    // denominator in HighPart on Windows, verified with both EXIF byte orders.
    return static_cast<double>(property.uhVal.LowPart) / property.uhVal.HighPart;
}

}  // namespace

HRESULT Metadata::Read(const std::wstring& path, PhotoMetadata& out) const noexcept {
    out = {};
    if (path.empty()) return E_INVALIDARG;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                             WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    return FAILED(hr) ? hr : ReadFrame(frame.Get(), out);
}

HRESULT Metadata::ReadFrame(IWICBitmapFrameDecode* frame, PhotoMetadata& out) const noexcept {
    out = {};
    if (!frame) return E_INVALIDARG;
    try {
        PhotoMetadata result;
        HRESULT hr = frame->GetSize(&result.width, &result.height);
        if (FAILED(hr)) return hr;
        ComPtr<IWICMetadataQueryReader> reader;
        if (FAILED(frame->GetMetadataQueryReader(reader.GetAddressOf()))) {
            out = std::move(result);
            return S_OK;
        }
        Property p;
        if (Query(reader.Get(), {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}",
                                 L"System.Photo.Orientation"}, p))
            result.orientation = Integer(p.value);
        if (Query(reader.Get(), {L"/app1/ifd/{ushort=271}", L"/ifd/{ushort=271}",
                                 L"System.Photo.CameraManufacturer"}, p))
            result.cameraMake = Text(p.value);
        if (Query(reader.Get(), {L"/app1/ifd/{ushort=272}", L"/ifd/{ushort=272}",
                                 L"System.Photo.CameraModel"}, p))
            result.cameraModel = Text(p.value);
        // DateTimeOriginal, falling back to the IFD0 DateTime.
        if (Query(reader.Get(), {L"/app1/ifd/exif/{ushort=36867}", L"/ifd/exif/{ushort=36867}",
                                 L"/app1/ifd/{ushort=306}", L"/ifd/{ushort=306}"}, p))
            result.dateTaken = Text(p.value);
        if (Query(reader.Get(), {L"/app1/ifd/exif/{ushort=33434}", L"/ifd/exif/{ushort=33434}",
                                 L"System.Photo.ExposureTime"}, p))
            result.exposureSeconds = Rational(p.value);
        if (Query(reader.Get(), {L"/app1/ifd/exif/{ushort=33437}", L"/ifd/exif/{ushort=33437}",
                                 L"System.Photo.FNumber"}, p))
            result.fNumber = Rational(p.value);
        if (Query(reader.Get(), {L"/app1/ifd/exif/{ushort=34855}", L"/ifd/exif/{ushort=34855}",
                                 L"System.Photo.ISOSpeed"}, p))
            result.iso = Integer(p.value);
        if (Query(reader.Get(), {L"/app1/ifd/exif/{ushort=37386}", L"/ifd/exif/{ushort=37386}",
                                 L"System.Photo.FocalLength"}, p))
            result.focalLengthMm = Rational(p.value);
        out = std::move(result);
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}
