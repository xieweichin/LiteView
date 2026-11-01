#include "core/Rotator.h"

#include <propvarutil.h>
#include <wincodec.h>
#include <wincodecsdk.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

#include "core/ImageDecoder.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::uint16_t kOrientationTag = 0x0112;

struct ExifLocation {
    bool found = false;           // an Exif APP1 segment exists
    std::size_t tiff = 0;         // offset of the TIFF header
    bool bigEndian = false;
    std::size_t valueOffset = 0;  // offset of the orientation SHORT, 0 if absent
};

std::uint16_t Read16(const std::vector<std::uint8_t>& b, std::size_t at, bool be) {
    return be ? static_cast<std::uint16_t>(b[at] << 8 | b[at + 1])
              : static_cast<std::uint16_t>(b[at + 1] << 8 | b[at]);
}

std::uint32_t Read32(const std::vector<std::uint8_t>& b, std::size_t at, bool be) {
    return be ? (static_cast<std::uint32_t>(b[at]) << 24 | static_cast<std::uint32_t>(b[at + 1]) << 16 |
                 static_cast<std::uint32_t>(b[at + 2]) << 8 | b[at + 3])
              : (static_cast<std::uint32_t>(b[at + 3]) << 24 | static_cast<std::uint32_t>(b[at + 2]) << 16 |
                 static_cast<std::uint32_t>(b[at + 1]) << 8 | b[at]);
}

// Walks the JPEG markers up to the start of scan looking for Exif APP1.
bool LocateExif(const std::vector<std::uint8_t>& b, ExifLocation& loc) {
    if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8) return false;  // not a JPEG
    std::size_t at = 2;
    while (at + 4 <= b.size()) {
        if (b[at] != 0xFF) return true;  // corrupt marker stream: treat as "no EXIF"
        const std::uint8_t marker = b[at + 1];
        if (marker == 0xFF) {  // fill byte
            ++at;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) return true;  // start of scan / end of image
        const std::size_t length = static_cast<std::size_t>(b[at + 2]) << 8 | b[at + 3];
        if (length < 2 || at + 2 + length > b.size()) return true;
        const std::size_t data = at + 4;
        if (marker == 0xE1 && length >= 2 + 6 + 8 && std::memcmp(&b[data], "Exif\0\0", 6) == 0) {
            const std::size_t tiff = data + 6;
            const std::size_t end = at + 2 + length;
            const bool be = b[tiff] == 'M' && b[tiff + 1] == 'M';
            if (!be && !(b[tiff] == 'I' && b[tiff + 1] == 'I')) return true;
            if (Read16(b, tiff + 2, be) != 42) return true;
            loc.found = true;
            loc.tiff = tiff;
            loc.bigEndian = be;
            const std::size_t ifd = tiff + Read32(b, tiff + 4, be);
            if (ifd + 2 > end) return true;
            const std::size_t count = Read16(b, ifd, be);
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t entry = ifd + 2 + i * 12;
                if (entry + 12 > end) break;
                if (Read16(b, entry, be) == kOrientationTag && Read16(b, entry + 2, be) == 3 &&
                    Read32(b, entry + 4, be) == 1) {
                    loc.valueOffset = entry + 8;
                    break;
                }
            }
            return true;
        }
        at += 2 + length;
    }
    return true;
}

// A minimal big-endian Exif APP1 segment holding only the orientation tag.
std::vector<std::uint8_t> MinimalExif(int orientation) {
    const std::uint8_t o = static_cast<std::uint8_t>(orientation);
    return {0xFF, 0xE1, 0x00, 0x22,                     // APP1, length 34
            'E', 'x', 'i', 'f', 0x00, 0x00,             // Exif header
            'M', 'M', 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08,  // TIFF header, IFD0 at 8
            0x00, 0x01,                                 // one entry
            0x01, 0x12, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, o, 0x00, 0x00,  // Orientation SHORT
            0x00, 0x00, 0x00, 0x00};                    // no next IFD
}

HRESULT ReadAll(const std::wstring& path, std::vector<std::uint8_t>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    LARGE_INTEGER size{};
    HRESULT hr = S_OK;
    if (!GetFileSizeEx(file, &size)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
    } else if (size.QuadPart > 512ll * 1024 * 1024) {
        hr = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    } else {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        if (!bytes.empty() && (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
                               read != bytes.size())) {
            hr = HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_READ_FAULT);
        }
    }
    CloseHandle(file);
    return hr;
}

HRESULT WriteAll(const std::wstring& path, const std::vector<std::uint8_t>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    DWORD written = 0;
    HRESULT hr = S_OK;
    if (!WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size()) {
        hr = HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_WRITE_FAULT);
    }
    if (SUCCEEDED(hr) && !FlushFileBuffers(file)) hr = HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(file);
    return hr;
}

std::wstring TempNameFor(const std::wstring& path) {
    return path + L".lv" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".tmp";
}

// Swaps `temp` in for `path`, keeping the original times and attributes.
HRESULT Commit(const std::wstring& path, const std::wstring& temp) {
    FILETIME created{}, accessed{}, written{};
    HANDLE original = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
    const bool haveTimes = original != INVALID_HANDLE_VALUE && GetFileTime(original, &created, &accessed, &written);
    if (original != INVALID_HANDLE_VALUE) CloseHandle(original);

    if (!ReplaceFileW(path.c_str(), temp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS,
                      nullptr, nullptr)) {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        DeleteFileW(temp.c_str());
        return hr;
    }
    if (haveTimes) {
        HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            SetFileTime(file, &created, nullptr, &written);
            CloseHandle(file);
        }
    }
    return S_OK;
}

HRESULT CreateFactory(ComPtr<IWICImagingFactory>& factory) {
    return CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
}

// JPEG with EXIF but no orientation tag: let WIC rewrite the metadata.
// IWICFastMetadataEncoder works in place when the block has padding;
// otherwise the frame is re-encoded from the decoder, which WIC does
// without recompressing when nothing but metadata changes.
HRESULT WriteOrientationWithWic(const std::wstring& source, const std::wstring& temp, int orientation) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CreateFactory(factory);
    if (FAILED(hr)) return hr;
    PROPVARIANT value;
    PropVariantInit(&value);
    value.vt = VT_UI2;
    value.uiVal = static_cast<USHORT>(orientation);

    // In-place attempt on a copy.
    if (CopyFileW(source.c_str(), temp.c_str(), FALSE)) {
        ComPtr<IWICBitmapDecoder> decoder;
        hr = factory->CreateDecoderFromFilename(temp.c_str(), nullptr, GENERIC_READ | GENERIC_WRITE,
                                                WICDecodeMetadataCacheOnDemand, &decoder);
        ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
        ComPtr<IWICFastMetadataEncoder> fast;
        if (SUCCEEDED(hr)) hr = factory->CreateFastMetadataEncoderFromFrameDecode(frame.Get(), &fast);
        ComPtr<IWICMetadataQueryWriter> writer;
        if (SUCCEEDED(hr)) hr = fast->GetMetadataQueryWriter(&writer);
        if (SUCCEEDED(hr)) hr = writer->SetMetadataByName(L"/app1/ifd/{ushort=274}", &value);
        if (SUCCEEDED(hr)) hr = fast->Commit();
        writer.Reset();
        fast.Reset();
        frame.Reset();
        decoder.Reset();
        if (SUCCEEDED(hr)) return S_OK;
    }

    // Re-encode with the metadata copied over.
    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(source.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    ComPtr<IWICStream> stream;
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(temp.c_str(), GENERIC_WRITE);
    ComPtr<IWICBitmapEncoder> encoder;
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frameEncode;
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frameEncode, nullptr);
    if (SUCCEEDED(hr)) hr = frameEncode->Initialize(nullptr);
    ComPtr<IWICMetadataBlockReader> blockReader;
    ComPtr<IWICMetadataBlockWriter> blockWriter;
    if (SUCCEEDED(hr) && SUCCEEDED(frame.As(&blockReader)) && SUCCEEDED(frameEncode.As(&blockWriter))) {
        blockWriter->InitializeFromBlockReader(blockReader.Get());  // best effort
    }
    ComPtr<IWICMetadataQueryWriter> writer;
    if (SUCCEEDED(hr)) hr = frameEncode->GetMetadataQueryWriter(&writer);
    if (SUCCEEDED(hr)) hr = writer->SetMetadataByName(L"/app1/ifd/{ushort=274}", &value);
    if (SUCCEEDED(hr)) hr = frameEncode->WriteSource(frame.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frameEncode->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    return hr;
}

// Turns a top-down 32bpp buffer a quarter turn.
std::vector<BYTE> RotatePixels(const std::vector<BYTE>& src, UINT width, UINT height, bool clockwise) {
    std::vector<BYTE> dst(src.size());
    const auto* in = reinterpret_cast<const std::uint32_t*>(src.data());
    auto* out = reinterpret_cast<std::uint32_t*>(dst.data());
    // Output is height x width.
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const UINT nx = clockwise ? height - 1 - y : y;
            const UINT ny = clockwise ? x : width - 1 - x;
            out[static_cast<std::size_t>(ny) * height + nx] = in[static_cast<std::size_t>(y) * width + x];
        }
    }
    return dst;
}

HRESULT ReencodeRotated(const std::wstring& path, const std::wstring& temp, const GUID& container, bool clockwise) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CreateFactory(factory);
    if (FAILED(hr)) return hr;
    // Decode upright (EXIF/TIFF orientation applied) at full size.
    DecodedImage image;
    hr = ImageDecoder().Decode(path, 0, image);
    if (FAILED(hr)) return hr;
    std::vector<BYTE> pixels(static_cast<std::size_t>(image.width) * image.height * 4);
    hr = image.bitmap->CopyPixels(nullptr, image.width * 4, static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(hr)) return hr;
    std::vector<BYTE> rotated = RotatePixels(pixels, image.width, image.height, clockwise);
    const UINT width = image.height, height = image.width;

    ComPtr<IWICBitmap> bitmap;
    hr = factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA, width * 4,
                                         static_cast<UINT>(rotated.size()), rotated.data(), &bitmap);
    ComPtr<IWICStream> stream;
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(temp.c_str(), GENERIC_WRITE);
    ComPtr<IWICBitmapEncoder> encoder;
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(container, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &options);
    if (SUCCEEDED(hr)) hr = frame->Initialize(options.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
    // Ask for straight BGRA; the encoder answers with what it supports.
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    ComPtr<IWICBitmapSource> converted;
    if (SUCCEEDED(hr)) hr = WICConvertBitmapSource(format, bitmap.Get(), &converted);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(converted.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    return hr;
}

}  // namespace

int Rotator::NextOrientation(int orientation, bool clockwise) noexcept {
    static constexpr int cw[9] = {1, 6, 7, 8, 5, 2, 3, 4, 1};
    static constexpr int ccw[9] = {1, 8, 5, 6, 7, 4, 1, 2, 3};
    if (orientation < 1 || orientation > 8) orientation = 1;
    return clockwise ? cw[orientation] : ccw[orientation];
}

int Rotator::ReadJpegOrientation(const std::vector<std::uint8_t>& jpeg) noexcept {
    ExifLocation loc;
    if (!LocateExif(jpeg, loc) || !loc.found) return -1;
    if (!loc.valueOffset) return 0;
    return Read16(jpeg, loc.valueOffset, loc.bigEndian);
}

HRESULT Rotator::SetJpegOrientation(std::vector<std::uint8_t>& jpeg, int orientation) noexcept {
    if (orientation < 1 || orientation > 8) return E_INVALIDARG;
    try {
        ExifLocation loc;
        if (!LocateExif(jpeg, loc)) return WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
        if (loc.found && loc.valueOffset) {
            const auto v = static_cast<std::uint16_t>(orientation);
            jpeg[loc.valueOffset] = static_cast<std::uint8_t>(loc.bigEndian ? v >> 8 : v & 0xFF);
            jpeg[loc.valueOffset + 1] = static_cast<std::uint8_t>(loc.bigEndian ? v & 0xFF : v >> 8);
            return S_OK;
        }
        if (loc.found) return S_FALSE;
        // No EXIF: insert one after SOI, or after a leading JFIF APP0.
        std::size_t insertAt = 2;
        if (jpeg.size() > 6 && jpeg[2] == 0xFF && jpeg[3] == 0xE0) {
            insertAt = 4 + (static_cast<std::size_t>(jpeg[4]) << 8 | jpeg[5]);
            if (insertAt > jpeg.size()) insertAt = 2;
        }
        const std::vector<std::uint8_t> segment = MinimalExif(orientation);
        jpeg.insert(jpeg.begin() + static_cast<std::ptrdiff_t>(insertAt), segment.begin(), segment.end());
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HRESULT Rotator::RotateFile(const std::wstring& path, bool clockwise) noexcept {
    try {
        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CreateFactory(factory);
        GUID container{};
        UINT frames = 0;
        {
            ComPtr<IWICBitmapDecoder> decoder;
            if (SUCCEEDED(hr)) {
                hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                        &decoder);
            }
            if (SUCCEEDED(hr)) hr = decoder->GetContainerFormat(&container);
            if (SUCCEEDED(hr)) hr = decoder->GetFrameCount(&frames);
        }
        if (FAILED(hr)) return hr;
        const std::wstring temp = TempNameFor(path);

        if (container == GUID_ContainerFormatJpeg) {
            std::vector<std::uint8_t> bytes;
            hr = ReadAll(path, bytes);
            if (FAILED(hr)) return hr;
            const int current = ReadJpegOrientation(bytes);
            const int next = NextOrientation(current > 0 ? current : 1, clockwise);
            hr = SetJpegOrientation(bytes, next);
            if (hr == S_OK) {
                hr = WriteAll(temp, bytes);
            } else if (hr == S_FALSE) {
                hr = WriteOrientationWithWic(path, temp, next);
            }
        } else if ((container == GUID_ContainerFormatPng || container == GUID_ContainerFormatBmp ||
                    container == GUID_ContainerFormatTiff) && frames == 1) {
            hr = ReencodeRotated(path, temp, container, clockwise);
        } else {
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);  // GIF, ICO, multi-page TIFF...
        }
        if (FAILED(hr)) {
            DeleteFileW(temp.c_str());
            return hr;
        }
        return Commit(path, temp);
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}
