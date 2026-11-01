#include "doctest.h"

#include "core/FileOps.h"

#include <string>

namespace {

std::wstring TempDir() {
    wchar_t base[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, base);
    std::wstring dir = std::wstring(base) + L"LiteViewOps-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(GetTickCount64());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void Touch(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    CloseHandle(file);
}

}  // namespace

TEST_CASE("file name validation") {
    CHECK(FileOps::IsValidFileName(L"旅遊_001.jpg"));
    CHECK(FileOps::IsValidFileName(L"a"));
    CHECK(FileOps::IsValidFileName(L"console.jpg"));
    CHECK_FALSE(FileOps::IsValidFileName(L""));
    CHECK_FALSE(FileOps::IsValidFileName(L"a/b.jpg"));
    CHECK_FALSE(FileOps::IsValidFileName(L"a:b.jpg"));
    CHECK_FALSE(FileOps::IsValidFileName(L"what?.jpg"));
    CHECK_FALSE(FileOps::IsValidFileName(L"name."));
    CHECK_FALSE(FileOps::IsValidFileName(L"name "));
    CHECK_FALSE(FileOps::IsValidFileName(L"CON"));
    CHECK_FALSE(FileOps::IsValidFileName(L"com1.jpg"));
    CHECK_FALSE(FileOps::IsValidFileName(L"tab\there"));
    CHECK_FALSE(FileOps::IsValidFileName(std::wstring(256, L'a')));
}

TEST_CASE("unique child names and rename") {
    const std::wstring dir = TempDir();
    CHECK(FileOps::UniqueChildName(dir, L"新增資料夾") == L"新增資料夾");
    CreateDirectoryW((dir + L"\\新增資料夾").c_str(), nullptr);
    CHECK(FileOps::UniqueChildName(dir, L"新增資料夾") == L"新增資料夾 (2)");
    CreateDirectoryW((dir + L"\\新增資料夾 (2)").c_str(), nullptr);
    CHECK(FileOps::UniqueChildName(dir, L"新增資料夾") == L"新增資料夾 (3)");

    Touch(dir + L"\\a.jpg");
    Touch(dir + L"\\b.jpg");
    CHECK(FileOps::Rename(dir + L"\\a.jpg", dir + L"\\b.jpg") == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    CHECK(FileOps::Rename(dir + L"\\a.jpg", dir + L"\\c.jpg") == S_OK);
    CHECK(GetFileAttributesW((dir + L"\\c.jpg").c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(FileOps::Rename(dir + L"\\c.jpg", dir + L"\\C.jpg") == S_OK);  // case only

    DeleteFileW((dir + L"\\b.jpg").c_str());
    DeleteFileW((dir + L"\\C.jpg").c_str());
    RemoveDirectoryW((dir + L"\\新增資料夾").c_str());
    RemoveDirectoryW((dir + L"\\新增資料夾 (2)").c_str());
    RemoveDirectoryW(dir.c_str());
}
