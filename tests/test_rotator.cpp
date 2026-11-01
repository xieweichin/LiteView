#include "doctest.h"

#include "core/ImageDecoder.h"
#include "core/Rotator.h"

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

std::wstring ExeDir() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    std::wstring executable(path.data(), length);
    return executable.substr(0, executable.find_last_of(L"\\/"));
}

std::wstring TempFile(const std::wstring& name) {
    wchar_t base[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, base);
    return std::wstring(base) + L"LiteViewRotate-" + std::to_wstring(GetCurrentProcessId()) + L"-" + name;
}

std::vector<std::uint8_t> ReadBytes(const std::wstring& path) {
    std::vector<std::uint8_t> bytes;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    return bytes;
}

FILETIME ModifiedTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    REQUIRE(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data));
    return data.ftLastWriteTime;
}

void SetOldModifiedTime(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    SYSTEMTIME st{2020, 5, 0, 17, 8, 30, 0, 0};
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    SetFileTime(file, nullptr, nullptr, &ft);
    CloseHandle(file);
}

// Writes a w x h image with four colored quadrants (no metadata).
HRESULT WriteQuadrants(const std::wstring& path, const GUID& container, UINT width, UINT height) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
    const BYTE colors[4][4] = {{20, 20, 240, 255}, {20, 220, 20, 255}, {240, 20, 20, 255}, {20, 220, 240, 255}};  // BGRA
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const BYTE* c = colors[(y >= height / 2 ? 2 : 0) + (x >= width / 2 ? 1 : 0)];
            std::copy(c, c + 4, &pixels[(static_cast<size_t>(y) * width + x) * 4]);
        }
    }
    ComPtr<IWICBitmap> bitmap;
    hr = factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, width * 4,
                                         static_cast<UINT>(pixels.size()), pixels.data(), &bitmap);
    ComPtr<IWICStream> stream;
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    ComPtr<IWICBitmapEncoder> encoder;
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(container, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
    WICPixelFormatGUID format = container == GUID_ContainerFormatJpeg ? GUID_WICPixelFormat24bppBGR
                                                                       : GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    ComPtr<IWICBitmapSource> converted;
    if (SUCCEEDED(hr)) hr = WICConvertBitmapSource(format, bitmap.Get(), &converted);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(converted.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    return hr;
}

std::array<BYTE, 4> Pixel(IWICBitmapSource* bitmap, UINT x, UINT y) {
    WICRect rect{static_cast<INT>(x), static_cast<INT>(y), 1, 1};
    std::array<BYTE, 4> p{};
    CHECK(bitmap->CopyPixels(&rect, 4, 4, p.data()) == S_OK);
    return p;
}

}  // namespace

TEST_CASE("four quarter turns return to the starting orientation") {
    for (int o = 1; o <= 8; ++o) {
        int cw = o, ccw = o;
        for (int i = 0; i < 4; ++i) {
            cw = Rotator::NextOrientation(cw, true);
            ccw = Rotator::NextOrientation(ccw, false);
        }
        CHECK(cw == o);
        CHECK(ccw == o);
        CHECK(Rotator::NextOrientation(Rotator::NextOrientation(o, true), false) == o);
    }
    CHECK(Rotator::NextOrientation(1, true) == 6);
    CHECK(Rotator::NextOrientation(1, false) == 8);
}

TEST_CASE("rotating a JPEG changes only the orientation tag and keeps the modified time") {
    ComApartment apartment;
    const std::wstring path = TempFile(L"tagged.jpg");
    REQUIRE(CopyFileW((ExeDir() + L"\\fixtures\\orientation-1.jpg").c_str(), path.c_str(), FALSE));
    SetOldModifiedTime(path);
    const FILETIME before = ModifiedTime(path);
    const std::vector<std::uint8_t> original = ReadBytes(path);
    CHECK(Rotator::ReadJpegOrientation(original) == 1);

    REQUIRE(Rotator::RotateFile(path, true) == S_OK);
    const std::vector<std::uint8_t> rotated = ReadBytes(path);
    CHECK(Rotator::ReadJpegOrientation(rotated) == 6);
    REQUIRE(rotated.size() == original.size());
    std::size_t differences = 0;
    for (std::size_t i = 0; i < original.size(); ++i) differences += original[i] != rotated[i] ? 1 : 0;
    CHECK(differences >= 1);
    CHECK(differences <= 2);  // the 16-bit orientation value and nothing else
    const FILETIME after = ModifiedTime(path);
    CHECK(CompareFileTime(&before, &after) == 0);

    REQUIRE(Rotator::RotateFile(path, false) == S_OK);
    CHECK(ReadBytes(path) == original);
    for (int i = 0; i < 4; ++i) REQUIRE(Rotator::RotateFile(path, true) == S_OK);
    CHECK(ReadBytes(path) == original);
    DeleteFileW(path.c_str());
}

TEST_CASE("a JPEG without EXIF gets an orientation block and identical image data") {
    ComApartment apartment;
    const std::wstring path = TempFile(L"plain.jpg");
    REQUIRE(WriteQuadrants(path, GUID_ContainerFormatJpeg, 32, 16) == S_OK);
    std::vector<std::uint8_t> original = ReadBytes(path);
    if (Rotator::ReadJpegOrientation(original) != -1) {
        MESSAGE("encoder wrote EXIF; inserting path not exercised");
        DeleteFileW(path.c_str());
        return;
    }
    REQUIRE(Rotator::RotateFile(path, false) == S_OK);
    const std::vector<std::uint8_t> rotated = ReadBytes(path);
    CHECK(Rotator::ReadJpegOrientation(rotated) == 8);
    REQUIRE(rotated.size() == original.size() + 36);  // marker + 34-byte segment
    // Everything from the end of the original's first segments on is unchanged.
    CHECK(std::equal(original.end() - static_cast<std::ptrdiff_t>(original.size() / 2), original.end(),
                     rotated.end() - static_cast<std::ptrdiff_t>(original.size() / 2)));
    // And it still decodes, now upright as portrait.
    DecodedImage decoded;
    REQUIRE(ImageDecoder().Decode(path, 0, decoded) == S_OK);
    DeleteFileW(path.c_str());
}

TEST_CASE("SetJpegOrientation patches bytes in place") {
    std::vector<std::uint8_t> jpeg = ReadBytes(ExeDir() + L"\\fixtures\\orientation-3.jpg");
    const std::size_t size = jpeg.size();
    REQUIRE(Rotator::ReadJpegOrientation(jpeg) == 3);
    REQUIRE(Rotator::SetJpegOrientation(jpeg, 5) == S_OK);
    CHECK(jpeg.size() == size);
    CHECK(Rotator::ReadJpegOrientation(jpeg) == 5);
    std::vector<std::uint8_t> notJpeg{'P', 'N', 'G', 0};
    CHECK(Rotator::SetJpegOrientation(notJpeg, 6) == WINCODEC_ERR_UNKNOWNIMAGEFORMAT);
}

TEST_CASE("PNG and BMP are re-encoded with their pixels turned") {
    ComApartment apartment;
    for (const auto& container : {GUID_ContainerFormatPng, GUID_ContainerFormatBmp}) {
        const std::wstring path = TempFile(container == GUID_ContainerFormatPng ? L"q.png" : L"q.bmp");
        REQUIRE(WriteQuadrants(path, container, 40, 20) == S_OK);
        SetOldModifiedTime(path);
        const FILETIME before = ModifiedTime(path);
        REQUIRE(Rotator::RotateFile(path, true) == S_OK);
        const FILETIME after = ModifiedTime(path);
        CHECK(CompareFileTime(&before, &after) == 0);

        DecodedImage decoded;
        REQUIRE(ImageDecoder().Decode(path, 0, decoded) == S_OK);
        CHECK(decoded.width == 20);
        CHECK(decoded.height == 40);
        // Clockwise: the old bottom-left quadrant is now top-left.
        const auto topLeft = Pixel(decoded.bitmap.Get(), 2, 2);
        CHECK(topLeft[0] == 240);  // old bottom-left was blue-ish BGRA (240,20,20)
        const auto topRight = Pixel(decoded.bitmap.Get(), 17, 2);
        CHECK(topRight[2] == 240);  // old top-left (20,20,240): red
        DeleteFileW(path.c_str());
    }
}

TEST_CASE("unsupported formats are refused without touching the file") {
    ComApartment apartment;
    const std::wstring path = TempFile(L"anim.gif");
    REQUIRE(CopyFileW((ExeDir() + L"\\fixtures\\two-frames.gif").c_str(), path.c_str(), FALSE));
    const std::vector<std::uint8_t> before = ReadBytes(path);
    CHECK(Rotator::RotateFile(path, true) == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    CHECK(ReadBytes(path) == before);
    DeleteFileW(path.c_str());
}
