#include "doctest.h"

#include "core/FormatSupport.h"

#include <objbase.h>

#include <cwchar>
#include <string>
#include <vector>

TEST_CASE("codec packs map known extensions case-insensitively") {
    CodecPack pack = CodecPack::Count;
    REQUIRE(CodecPackForExtension(L".HEIC", pack));
    CHECK(pack == CodecPack::Heif);
    REQUIRE(CodecPackForExtension(L".webp", pack));
    CHECK(pack == CodecPack::WebP);
    REQUIRE(CodecPackForExtension(L".avif", pack));
    CHECK(pack == CodecPack::Avif);
    REQUIRE(CodecPackForExtension(L".Cr3", pack));
    CHECK(pack == CodecPack::Raw);
    REQUIRE(CodecPackForExtension(L".jxl", pack));
    CHECK(pack == CodecPack::JpegXl);
    CHECK_FALSE(CodecPackForExtension(L".jpg", pack));
    CHECK_FALSE(CodecPackForExtension(L".txt", pack));
    CHECK_FALSE(CodecPackForExtension(L"", pack));
    CHECK_FALSE(CodecPackForExtension(nullptr, pack));
}

TEST_CASE("missing packs are those with no decoder for any of their extensions") {
    const std::vector<std::wstring> extensions{L".jpg", L".png", L".WEBP", L".nef"};
    const std::uint32_t missing = MissingCodecPacks(extensions);
    CHECK((missing & CodecPackBit(CodecPack::Heif)) != 0);
    CHECK((missing & CodecPackBit(CodecPack::WebP)) == 0);
    CHECK((missing & CodecPackBit(CodecPack::Avif)) != 0);
    CHECK((missing & CodecPackBit(CodecPack::Raw)) == 0);
    CHECK((missing & CodecPackBit(CodecPack::JpegXl)) != 0);
    CHECK(FirstCodecPack(missing) == CodecPack::Heif);
    CHECK(FirstCodecPack(CodecPackBit(CodecPack::Raw)) == CodecPack::Raw);
    CHECK(FirstCodecPack(0) == CodecPack::Count);
}

TEST_CASE("every codec pack has a name and a Microsoft Store link") {
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(CodecPack::Count); ++i) {
        const auto pack = static_cast<CodecPack>(i);
        CHECK(std::wcslen(CodecPackName(pack)) > 0);
        CHECK(std::wcsncmp(CodecPackStoreUri(pack), L"ms-windows-store://", 19) == 0);
    }
    CHECK(std::wcslen(CodecPackName(CodecPack::Count)) == 0);
}

TEST_CASE("installed WIC decoders include the built-in formats") {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<std::wstring> extensions;
    REQUIRE(DecoderExtensions(extensions) == S_OK);
    CHECK(HasExtension(extensions, L".JPG"));
    CHECK(HasExtension(extensions, L".png"));
    CHECK_FALSE(HasExtension(extensions, L".txt"));
    CHECK_FALSE(HasExtension(extensions, L""));
    if (SUCCEEDED(init)) CoUninitialize();
}

TEST_CASE("Office documents are recognised by extension") {
    CHECK(IsDocumentExtension(L".docx"));
    CHECK(IsDocumentExtension(L".XLSX"));
    CHECK(IsDocumentExtension(L".pptx"));
    CHECK(IsDocumentExtension(L".odt"));
    CHECK(IsDocumentExtension(L".rtf"));
    CHECK_FALSE(IsDocumentExtension(L".pdf"));  // drawn by Windows.Data.Pdf instead
    CHECK_FALSE(IsDocumentExtension(L".txt"));
    CHECK_FALSE(IsDocumentExtension(L""));
    CHECK_FALSE(IsDocumentExtension(nullptr));
}

TEST_CASE("video extensions") {
    CHECK(IsVideoExtension(L".mp4"));
    CHECK(IsVideoExtension(L".MOV"));
    CHECK(IsVideoExtension(L".mkv"));
    CHECK(IsVideoExtension(L".m2ts"));
    CHECK_FALSE(IsVideoExtension(L".ts"));  // TypeScript
    CHECK_FALSE(IsVideoExtension(L".jpg"));
    CHECK_FALSE(IsVideoExtension(L""));
    CHECK_FALSE(IsVideoExtension(nullptr));
}

TEST_CASE("media times") {
    constexpr long long kSecond = 10000000;
    CHECK(FormatMediaTime(0) == L"0:00");
    CHECK(FormatMediaTime(-5) == L"0:00");
    CHECK(FormatMediaTime(65 * kSecond + kSecond / 2) == L"1:05");
    CHECK(FormatMediaTime((3600 + 2 * 60 + 3) * kSecond) == L"1:02:03");
}
