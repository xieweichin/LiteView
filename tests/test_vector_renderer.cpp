#include "doctest.h"

#include "core/ImageDecoder.h"
#include "core/VectorRenderer.h"

#include <array>
#include <ios>
#include <string>

namespace {

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

std::wstring TempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH]{};
    REQUIRE(GetTempPathW(MAX_PATH, dir) > 0);
    return std::wstring(dir) + L"liteview-" + std::to_wstring(GetCurrentProcessId()) + L"-" + name;
}

void WriteFile(const std::wstring& path, const std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
}

std::array<BYTE, 4> PixelAt(IWICBitmapSource* bitmap, UINT x, UINT y) {
    WICRect rect{static_cast<INT>(x), static_cast<INT>(y), 1, 1};
    std::array<BYTE, 4> pixel{};
    CHECK(bitmap->CopyPixels(&rect, 4, 4, pixel.data()) == S_OK);
    return pixel;
}

}  // namespace

TEST_CASE("vector extensions") {
    CHECK(IsVectorPath(L"C:\\a\\logo.SVG"));
    CHECK(IsVectorPath(L"C:\\a\\chart.emf"));
    CHECK(IsVectorPath(L"C:\\a\\clip.wmf"));
    CHECK_FALSE(IsVectorPath(L"C:\\a\\photo.png"));
}

TEST_CASE("SVG is drawn by Direct2D at the requested size") {
    ComApartment com;
    const std::wstring path = TempPath(L"box.svg");
    WriteFile(path,
              "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"50\" viewBox=\"0 0 100 50\">"
              "<rect x=\"25\" y=\"12.5\" width=\"50\" height=\"25\" fill=\"#ff0000\"/></svg>");
    DecodedImage image;
    const HRESULT hr = ImageDecoder().Decode(path, 200, image);
    DeleteFileW(path.c_str());
    if (hr == E_NOTIMPL || hr == E_NOINTERFACE) {
        MESSAGE("Direct2D SVG support is not available in this build or Windows; skipping");
        return;
    }
    if (FAILED(hr)) MESSAGE("SVG render failed, hr " << std::hex << hr);
    REQUIRE(hr == S_OK);
    CHECK(image.width == 200);
    CHECK(image.height == 100);
    CHECK(image.originalWidth == 1024);  // own size 100 x 50 -> at least 1024 px
    CHECK(image.originalHeight == 512);
    const auto centre = PixelAt(image.bitmap.Get(), 100, 50);
    CHECK(centre[2] > 200);  // red, opaque
    CHECK(centre[3] > 200);
    const auto corner = PixelAt(image.bitmap.Get(), 3, 3);
    CHECK(corner[3] < 20);   // transparent outside the box
}

TEST_CASE("EMF is drawn by GDI+") {
    ComApartment com;
    const std::wstring path = TempPath(L"box.emf");
    // 40 x 20 mm picture with a red box in the middle.
    HDC reference = GetDC(nullptr);
    RECT frame{0, 0, 4000, 2000};  // 0.01 mm units
    HDC dc = CreateEnhMetaFileW(reference, path.c_str(), &frame, nullptr);
    ReleaseDC(nullptr, reference);
    REQUIRE(dc != nullptr);
    const int mmX = GetDeviceCaps(dc, HORZRES) * 100 / GetDeviceCaps(dc, HORZSIZE);  // pixels per 1 mm * 100
    const int mmY = GetDeviceCaps(dc, VERTRES) * 100 / GetDeviceCaps(dc, VERTSIZE);
    HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
    RECT box{10 * mmX / 100, 5 * mmY / 100, 30 * mmX / 100, 15 * mmY / 100};
    FillRect(dc, &box, red);
    DeleteObject(red);
    HENHMETAFILE metafile = CloseEnhMetaFile(dc);
    REQUIRE(metafile != nullptr);
    DeleteEnhMetaFile(metafile);

    DecodedImage image;
    const HRESULT hr = ImageDecoder().Decode(path, 400, image);
    DeleteFileW(path.c_str());
    if (FAILED(hr)) MESSAGE("EMF render failed, hr " << std::hex << hr);
    REQUIRE(hr == S_OK);
    CHECK(image.width == 400);
    CHECK(image.height >= 190);
    CHECK(image.height <= 210);
    const auto centre = PixelAt(image.bitmap.Get(), image.width / 2, image.height / 2);
    CHECK(centre[2] > 200);
    CHECK(centre[1] < 60);
}
