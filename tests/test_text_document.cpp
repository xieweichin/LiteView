#include "doctest.h"

#include "core/TextDocument.h"

#include <string>

namespace {

TextDocument Decode(const std::string& bytes, bool cut = false) {
    TextDocument doc;
    REQUIRE(DecodeText(bytes.data(), bytes.size(), cut, doc) == S_OK);
    return doc;
}

std::wstring TempFile(const std::string& bytes) {
    wchar_t dir[MAX_PATH]{}, name[MAX_PATH]{};
    REQUIRE(GetTempPathW(MAX_PATH, dir) > 0);
    REQUIRE(GetTempFileNameW(dir, L"ltx", 0, name) != 0);
    HANDLE file = CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    return name;
}

}  // namespace

TEST_CASE("text extensions") {
    CHECK(IsTextExtension(L".txt"));
    CHECK(IsTextExtension(L".MD"));
    CHECK(IsTextExtension(L".cpp"));
    CHECK(IsTextExtension(L".json"));
    CHECK_FALSE(IsTextExtension(L".jpg"));
    CHECK_FALSE(IsTextExtension(L".exe"));
    CHECK_FALSE(IsTextExtension(L""));
    CHECK_FALSE(IsTextExtension(nullptr));
}

TEST_CASE("UTF-8 with and without BOM, line ends become CRLF") {
    const TextDocument plain = Decode("a\nb\r\nc\rd");
    CHECK(plain.encoding == TextEncoding::Utf8);
    CHECK(plain.text == L"a\r\nb\r\nc\r\nd");
    CHECK_FALSE(plain.truncated);

    const TextDocument bom = Decode("\xEF\xBB\xBF\xE4\xB8\xAD\xE6\x96\x87");  // 中文
    CHECK(bom.encoding == TextEncoding::Utf8Bom);
    CHECK(bom.text == L"\x4E2D\x6587");
}

TEST_CASE("UTF-16 with a BOM and by NUL pattern") {
    const TextDocument le = Decode(std::string("\xFF\xFE" "A\0B\0", 6));
    CHECK(le.encoding == TextEncoding::Utf16Le);
    CHECK(le.text == L"AB");
    const TextDocument be = Decode(std::string("\xFE\xFF\0A\0B", 6));
    CHECK(be.encoding == TextEncoding::Utf16Be);
    CHECK(be.text == L"AB");
    const TextDocument guessed = Decode(std::string("H\0e\0l\0l\0o\0", 10));
    CHECK(guessed.encoding == TextEncoding::Utf16Le);
    CHECK(guessed.text == L"Hello");
}

TEST_CASE("invalid UTF-8 falls back to the ANSI code page") {
    const TextDocument doc = Decode("caf\xE9 ok");
    CHECK(doc.encoding == TextEncoding::Ansi);
    CHECK(doc.text.size() >= 7);
    CHECK(doc.text.substr(0, 3) == L"caf");
}

TEST_CASE("a cut in the middle of a UTF-8 character drops the partial character") {
    // "中" is E4 B8 AD; keep only its first two bytes after "x".
    const TextDocument doc = Decode("x\xE4\xB8", true);
    CHECK(doc.encoding == TextEncoding::Utf8);
    CHECK(doc.text == L"x");
    CHECK(doc.truncated);
}

TEST_CASE("NUL characters are made visible") {
    const TextDocument doc = Decode(std::string("a\0b", 3));
    CHECK(doc.text.size() == 3);
    CHECK(doc.text[1] == L'\x2400');
}

TEST_CASE("loading a file honours the byte limit") {
    const std::wstring path = TempFile("line1\nline2\nline3\n");
    TextDocument whole;
    REQUIRE(LoadTextFile(path, 1024, whole) == S_OK);
    CHECK(whole.text == L"line1\r\nline2\r\nline3\r\n");
    CHECK(whole.fileSize == 18);
    CHECK_FALSE(whole.truncated);

    TextDocument head;
    REQUIRE(LoadTextFile(path, 5, head) == S_OK);
    CHECK(head.text == L"line1");
    CHECK(head.truncated);
    DeleteFileW(path.c_str());

    TextDocument missing;
    CHECK(FAILED(LoadTextFile(path, 1024, missing)));
}
