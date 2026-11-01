#include "doctest.h"

#include "core/ImageDecoder.h"
#include "core/PdfRenderer.h"

#include <array>
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
    std::wstring executable(path.data(), length);
    const auto slash = executable.find_last_of(L"\\/");
    REQUIRE(slash != std::wstring::npos);
    return executable.substr(0, slash) + L"\\fixtures\\" + name;
}

std::array<BYTE, 4> PixelAt(IWICBitmapSource* bitmap, UINT x, UINT y) {
    WICRect rect{static_cast<INT>(x), static_cast<INT>(y), 1, 1};
    std::array<BYTE, 4> pixel{};
    CHECK(bitmap->CopyPixels(&rect, 4, 4, pixel.data()) == S_OK);
    return pixel;
}

// Windows.Data.Pdf may be missing on stripped-down Windows editions.
bool PdfUnavailable(HRESULT hr) {
    return hr == REGDB_E_CLASSNOTREG || hr == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) || hr == E_NOTIMPL;
}

}  // namespace

TEST_CASE("PDF page paths round-trip and reject plain paths") {
    CHECK(PdfPagePath(L"C:\\a.pdf", 0) == L"C:\\a.pdf");
    CHECK(PdfPagePath(L"C:\\a.pdf", 3) == L"C:\\a.pdf|3");
    std::wstring file;
    UINT page = 99;
    CHECK(SplitPdfPagePath(L"C:\\a.pdf|3", file, page));
    CHECK(file == L"C:\\a.pdf");
    CHECK(page == 3);
    CHECK_FALSE(SplitPdfPagePath(L"C:\\a.pdf", file, page));
    CHECK(file == L"C:\\a.pdf");
    CHECK(page == 0);
    CHECK_FALSE(SplitPdfPagePath(L"C:\\a.pdf|x", file, page));
    CHECK(IsPdfPath(L"C:\\Docs\\Report.PDF"));
    CHECK(IsPdfPath(L"C:\\Docs\\Report.pdf|2"));
    CHECK_FALSE(IsPdfPath(L"C:\\Docs\\photo.jpg"));
}

TEST_CASE("Windows renders PDF pages through the image decoder") {
    ComApartment com;
    REQUIRE(SUCCEEDED(com.result));
    const std::wstring path = Fixture(L"two-pages.pdf");

    DecodedImage first;
    const HRESULT hr = ImageDecoder().Decode(path, 200, first);
    if (PdfUnavailable(hr)) {
        MESSAGE("Windows.Data.Pdf is not available here; skipping render checks");
        return;
    }
    if (FAILED(hr)) MESSAGE("PDF render failed at step " << LastPdfRenderStep() << ", hr " << std::hex << hr);
    REQUIRE(hr == S_OK);
    CHECK(first.pageCount == 2);
    CHECK(first.width == 200);   // 200 x 100 pt page, long side fitted to 200
    CHECK(first.height == 100);
    CHECK(first.originalWidth > first.width);  // full size is about 192 dpi
    CHECK(static_cast<int>(first.originalHeight * 2) - static_cast<int>(first.originalWidth) <= 1);
    CHECK(static_cast<int>(first.originalWidth) - static_cast<int>(first.originalHeight * 2) <= 1);
    const auto centre = PixelAt(first.bitmap.Get(), 100, 50);   // inside the red box
    CHECK(centre[2] > 200);
    CHECK(centre[1] < 60);
    const auto corner = PixelAt(first.bitmap.Get(), 5, 5);      // white page
    CHECK(corner[0] > 200);
    CHECK(corner[1] > 200);

    DecodedImage second;
    REQUIRE(ImageDecoder().Decode(PdfPagePath(path, 1), 200, second) == S_OK);
    CHECK(second.width == 100);  // the second page is portrait
    CHECK(second.height == 200);

    DecodedImage missing;
    CHECK(FAILED(ImageDecoder().Decode(Fixture(L"no-such-file.pdf"), 200, missing)));
}
