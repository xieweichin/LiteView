#include "doctest.h"

#include "core/ImageDecoder.h"
#include "core/Metadata.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <string>
#include <vector>

namespace {

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

std::wstring Fixture(const std::wstring& name) {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    REQUIRE(length > 0);
    REQUIRE(length < path.size());
    std::wstring executable(path.data(), length);
    const auto slash = executable.find_last_of(L"\\/");
    REQUIRE(slash != std::wstring::npos);
    return executable.substr(0, slash) + L"\\fixtures\\" + name;
}

using Pixel = std::array<BYTE, 4>;  // premultiplied BGRA

Pixel PixelAt(IWICBitmapSource* bitmap, UINT x, UINT y) {
    WICRect rect{static_cast<INT>(x), static_cast<INT>(y), 1, 1};
    Pixel pixel{};
    CHECK(bitmap->CopyPixels(&rect, 4, 4, pixel.data()) == S_OK);
    return pixel;
}

void CheckColor(const Pixel& actual, const std::array<int, 3>& rgb) {
    CHECK(std::abs(static_cast<int>(actual[2]) - rgb[0]) <= 30);
    CHECK(std::abs(static_cast<int>(actual[1]) - rgb[1]) <= 30);
    CHECK(std::abs(static_cast<int>(actual[0]) - rgb[2]) <= 30);
    CHECK(actual[3] == 255);
}

}  // namespace

TEST_CASE("EXIF orientations produce the correct size and four corners") {
    ComApartment apartment;
    REQUIRE(SUCCEEDED(apartment.result));
    constexpr std::array<std::array<int, 3>, 4> colors{{
        {{240, 20, 20}}, {{20, 220, 20}}, {{20, 20, 240}}, {{240, 220, 20}}
    }};
    constexpr int corners[8][4] = {
        {0, 1, 2, 3}, {1, 0, 3, 2}, {3, 2, 1, 0}, {2, 3, 0, 1},
        {0, 2, 1, 3}, {2, 0, 3, 1}, {3, 1, 2, 0}, {1, 3, 0, 2}
    };
    ImageDecoder decoder;
    for (UINT orientation = 1; orientation <= 8; ++orientation) {
        CAPTURE(orientation);
        for (UINT maxSide : {0u, 16u}) {
            CAPTURE(maxSide);
            DecodedImage decoded;
            REQUIRE(decoder.Decode(Fixture(L"orientation-" + std::to_wstring(orientation) + L".jpg"),
                                   maxSide, decoded) == S_OK);
            REQUIRE(decoded.bitmap);
            WICPixelFormatGUID format{};
            REQUIRE(decoded.bitmap->GetPixelFormat(&format) == S_OK);
            CHECK(IsEqualGUID(format, GUID_WICPixelFormat32bppPBGRA));
            const bool swapped = orientation >= 5;
            CHECK(decoded.originalWidth == (swapped ? 40u : 64u));
            CHECK(decoded.originalHeight == (swapped ? 64u : 40u));
            CHECK(decoded.width == (maxSide ? (swapped ? 10u : 16u) : (swapped ? 40u : 64u)));
            CHECK(decoded.height == (maxSide ? (swapped ? 16u : 10u) : (swapped ? 64u : 40u)));
            const UINT x1 = decoded.width / 8, x2 = decoded.width - 1 - x1;
            const UINT y1 = decoded.height / 8, y2 = decoded.height - 1 - y1;
            CheckColor(PixelAt(decoded.bitmap.Get(), x1, y1), colors[corners[orientation - 1][0]]);
            CheckColor(PixelAt(decoded.bitmap.Get(), x2, y1), colors[corners[orientation - 1][1]]);
            CheckColor(PixelAt(decoded.bitmap.Get(), x1, y2), colors[corners[orientation - 1][2]]);
            CheckColor(PixelAt(decoded.bitmap.Get(), x2, y2), colors[corners[orientation - 1][3]]);
        }
    }
}

TEST_CASE("metadata reads known EXIF values and leaves absent values empty") {
    ComApartment apartment;
    REQUIRE(SUCCEEDED(apartment.result));
    Metadata reader;
    PhotoMetadata photo;
    REQUIRE(reader.Read(Fixture(L"orientation-6.jpg"), photo) == S_OK);
    CHECK(photo.width == 64);
    CHECK(photo.height == 40);
    REQUIRE(photo.orientation.has_value());
    CHECK(*photo.orientation == 6);
    CHECK(photo.cameraMake == L"LiteView Test");
    CHECK(photo.cameraModel == L"Fixture Camera");
    CHECK(photo.dateTaken == L"2026:11:01 12:34:56");
    REQUIRE(photo.exposureSeconds.has_value());
    CHECK(*photo.exposureSeconds == doctest::Approx(1.0 / 125));
    REQUIRE(photo.fNumber.has_value());
    CHECK(*photo.fNumber == doctest::Approx(2.8));
    REQUIRE(photo.iso.has_value());
    CHECK(*photo.iso == 200);
    REQUIRE(photo.focalLengthMm.has_value());
    CHECK(*photo.focalLengthMm == doctest::Approx(35.0));

    REQUIRE(reader.Read(Fixture(L"orientation-1.jpg"), photo) == S_OK);
    REQUIRE(photo.exposureSeconds.has_value());
    CHECK(*photo.exposureSeconds == doctest::Approx(1.0 / 125));
    REQUIRE(photo.fNumber.has_value());
    CHECK(*photo.fNumber == doctest::Approx(2.8));
    REQUIRE(photo.focalLengthMm.has_value());
    CHECK(*photo.focalLengthMm == doctest::Approx(35.0));

    REQUIRE(reader.Read(Fixture(L"oriented.tiff"), photo) == S_OK);
    REQUIRE(photo.orientation.has_value());
    CHECK(*photo.orientation == 6);
    CHECK(photo.cameraMake == L"LiteView TIFF");
    REQUIRE(photo.exposureSeconds.has_value());
    CHECK(*photo.exposureSeconds == doctest::Approx(1.0 / 125));
    REQUIRE(photo.fNumber.has_value());
    CHECK(*photo.fNumber == doctest::Approx(2.8));
    REQUIRE(photo.iso.has_value());
    CHECK(*photo.iso == 200);
    REQUIRE(photo.focalLengthMm.has_value());
    CHECK(*photo.focalLengthMm == doctest::Approx(35.0));

    REQUIRE(reader.Read(Fixture(L"alpha.png"), photo) == S_OK);
    CHECK(photo.width == 2);
    CHECK(photo.height == 1);
    CHECK_FALSE(photo.orientation.has_value());
    CHECK(photo.cameraMake.empty());
    CHECK_FALSE(photo.exposureSeconds.has_value());
}

TEST_CASE("WIC formats decode the first frame and premultiply alpha") {
    ComApartment apartment;
    REQUIRE(SUCCEEDED(apartment.result));
    ImageDecoder decoder;
    DecodedImage decoded;
    REQUIRE(decoder.Decode(Fixture(L"alpha.png"), 0, decoded) == S_OK);
    CHECK(PixelAt(decoded.bitmap.Get(), 0, 0) == Pixel{25, 50, 100, 128});
    CHECK(PixelAt(decoded.bitmap.Get(), 1, 0) == Pixel{0, 0, 0, 0});
    REQUIRE(decoder.Decode(Fixture(L"two-frames.gif"), 0, decoded) == S_OK);
    CheckColor(PixelAt(decoded.bitmap.Get(), 0, 0), {240, 20, 20});
    REQUIRE(decoder.Decode(Fixture(L"oriented.tiff"), 0, decoded) == S_OK);
    CHECK(decoded.width == 40);
    CHECK(decoded.height == 64);
    CheckColor(PixelAt(decoded.bitmap.Get(), 3, 3), {20, 20, 240});
    for (const wchar_t* name : {L"basic.bmp", L"basic.tiff", L"basic.ico"}) {
        INFO(std::string(name[6] == L'b' ? "BMP" : name[6] == L't' ? "TIFF" : "ICO"));
        REQUIRE(decoder.Decode(Fixture(name), 16, decoded) == S_OK);
        CHECK(std::max(decoded.width, decoded.height) <= 16);
    }
}

TEST_CASE("invalid and unsupported images return failure without stale output") {
    ComApartment apartment;
    REQUIRE(SUCCEEDED(apartment.result));
    ImageDecoder decoder;
    DecodedImage decoded;
    REQUIRE(decoder.Decode(Fixture(L"alpha.png"), 0, decoded) == S_OK);
    CHECK(FAILED(decoder.Decode(Fixture(L"broken.jpg"), 0, decoded)));
    CHECK_FALSE(decoded.bitmap);
    CHECK(decoded.width == 0);
    CHECK(FAILED(decoder.Decode(Fixture(L"missing.jpg"), 0, decoded)));
    CHECK(decoder.Decode(L"", 0, decoded) == E_INVALIDARG);
}

TEST_CASE("independent COM-initialized threads can decode concurrently") {
    const std::wstring path = Fixture(L"orientation-6.jpg");
    std::vector<std::future<bool>> work;
    for (int i = 0; i < 4; ++i) {
        work.push_back(std::async(std::launch::async, [path] {
            ComApartment apartment;
            if (FAILED(apartment.result)) return false;
            DecodedImage decoded;
            if (FAILED(ImageDecoder{}.Decode(path, 16, decoded))) return false;
            WICRect rect{0, 0, 1, 1};
            Pixel pixel{};
            return decoded.width == 10 && decoded.height == 16 &&
                   SUCCEEDED(decoded.bitmap->CopyPixels(&rect, 4, 4, pixel.data()));
        }));
    }
    for (auto& result : work) CHECK(result.get());
}

namespace {

// Writes a width x height four-colour JPEG with the given EXIF orientation.
HRESULT WriteLargeJpeg(const std::wstring& path, UINT width, UINT height, USHORT orientation) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) return hr;
    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 3);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            BYTE* p = &pixels[(static_cast<size_t>(y) * width + x) * 3];  // BGR
            const bool right = x >= width / 2, bottom = y >= height / 2;
            const int rgb[4][3] = {{240, 20, 20}, {20, 220, 20}, {20, 20, 240}, {240, 220, 20}};
            const int* c = rgb[(bottom ? 2 : 0) + (right ? 1 : 0)];
            p[0] = static_cast<BYTE>(c[2]);
            p[1] = static_cast<BYTE>(c[1]);
            p[2] = static_cast<BYTE>(c[0]);
        }
    }
    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(stream.GetAddressOf());
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    ComPtr<IWICBitmapEncoder> encoder;
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.GetAddressOf());
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(frame.GetAddressOf(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr)) {
        ComPtr<IWICMetadataQueryWriter> writer;
        hr = frame->GetMetadataQueryWriter(writer.GetAddressOf());
        if (SUCCEEDED(hr)) {
            PROPVARIANT value{};
            value.vt = VT_UI2;
            value.uiVal = orientation;
            hr = writer->SetMetadataByName(L"/app1/ifd/{ushort=274}", &value);
        }
    }
    if (SUCCEEDED(hr)) {
        hr = frame->WritePixels(height, width * 3, static_cast<UINT>(pixels.size()), pixels.data());
    }
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    return hr;
}

}  // namespace

// Regression: IWICBitmapFlipRotator on an unbuffered decoder is quadratic, so a
// 12 MP portrait phone photo would take minutes. The bound is generous on purpose.
TEST_CASE("rotating a 12 MP JPEG stays fast") {
    ComApartment apartment;
    REQUIRE(SUCCEEDED(apartment.result));
    wchar_t temp[MAX_PATH]{};
    REQUIRE(GetTempPathW(MAX_PATH, temp) > 0);
    const std::wstring path = std::wstring(temp) + L"LiteView-large-" +
                              std::to_wstring(GetCurrentProcessId()) + L".jpg";
    const HRESULT written = WriteLargeJpeg(path, 4000, 3000, 6);
    if (FAILED(written)) {
        DeleteFileW(path.c_str());
        MESSAGE("JPEG encoder with EXIF writer unavailable; skipped");
        return;
    }
    ImageDecoder decoder;
    for (UINT maxSide : {0u, 1920u, 256u}) {
        CAPTURE(maxSide);
        DecodedImage decoded;
        const ULONGLONG start = GetTickCount64();
        REQUIRE(decoder.Decode(path, maxSide, decoded) == S_OK);
        const ULONGLONG ms = GetTickCount64() - start;
        MESSAGE("4000x3000 orientation 6, maxSide " << maxSide << ": " << ms << " ms");
        CHECK(decoded.originalWidth == 3000);
        CHECK(decoded.originalHeight == 4000);
        CHECK(decoded.height >= decoded.width);
        CHECK(ms < 3000);
    }
    DeleteFileW(path.c_str());
}
