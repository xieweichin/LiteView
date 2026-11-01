#include "doctest.h"

#include "core/BatchRename.h"

#include <string>
#include <vector>

namespace {

std::wstring TempDir() {
    wchar_t base[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, base);
    static int counter = 0;
    std::wstring dir = std::wstring(base) + L"LiteViewBatch-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++counter);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void WriteText(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

std::string ReadText(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return "<missing>";
    char buffer[256] = {};
    DWORD read = 0;
    ReadFile(file, buffer, sizeof(buffer), &read, nullptr);
    CloseHandle(file);
    return std::string(buffer, read);
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name != L"." && name != L"..") DeleteFileW((dir + L"\\" + name).c_str());
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

SYSTEMTIME Date(WORD y, WORD m, WORD d) {
    SYSTEMTIME t{};
    t.wYear = y;
    t.wMonth = m;
    t.wDay = d;
    return t;
}

}  // namespace

TEST_CASE("batch rename pattern expansion") {
    const SYSTEMTIME date = Date(2024, 5, 7);
    CHECK(BatchRename::ExpandName(L"旅遊_###", 7, L"IMG_1", nullptr) == L"旅遊_007");
    CHECK(BatchRename::ExpandName(L"#", 12, L"x", nullptr) == L"12");
    CHECK(BatchRename::ExpandName(L"*_##", 3, L"IMG_1", nullptr) == L"IMG_1_03");
    CHECK(BatchRename::ExpandName(L"{date:yyyyMMdd}_##", 1, L"x", &date) == L"20240507_01");
    CHECK(BatchRename::ExpandName(L"{date:yy-MM-dd}", 1, L"x", &date) == L"24-05-07");
    CHECK(BatchRename::ExpandName(L"a{date:", 1, L"x", &date) == L"a{date:");
    CHECK(BatchRename::ExpandName(L"plain", 1, L"x", nullptr) == L"plain");
}

TEST_CASE("batch rename preview keeps extensions and numbers in list order") {
    const std::wstring dir = TempDir();
    WriteText(dir + L"\\b.JPG", "b");
    WriteText(dir + L"\\a.png", "a");
    RenameOptions options;
    options.pattern = L"旅遊_###";
    options.startNumber = 9;
    options.lowercaseExtension = true;
    const auto plan = BatchRename::Preview({dir + L"\\b.JPG", dir + L"\\a.png"}, options, nullptr);
    REQUIRE(plan.size() == 2);
    CHECK(plan[0].to == dir + L"\\旅遊_009.jpg");
    CHECK(plan[1].to == dir + L"\\旅遊_010.png");
    CHECK_FALSE(BatchRename::HasConflicts(plan));

    options.pattern = L"{date:yyyyMMdd}";
    options.lowercaseExtension = false;
    const auto dated = BatchRename::Preview({dir + L"\\b.JPG"}, options,
                                            [](const std::wstring&) { return Date(2023, 12, 31); });
    CHECK(dated[0].to == dir + L"\\20231231.JPG");
    RemoveTree(dir);
}

TEST_CASE("batch rename preview flags conflicts") {
    const std::wstring dir = TempDir();
    WriteText(dir + L"\\a.jpg", "a");
    WriteText(dir + L"\\b.jpg", "b");
    WriteText(dir + L"\\taken.jpg", "t");
    RenameOptions options;

    options.pattern = L"same";  // duplicates
    auto plan = BatchRename::Preview({dir + L"\\a.jpg", dir + L"\\b.jpg"}, options, nullptr);
    CHECK(plan[0].issue == RenameIssue::Duplicate);
    CHECK(plan[1].issue == RenameIssue::Duplicate);
    CHECK(BatchRename::Execute(plan, L"") == E_INVALIDARG);

    options.pattern = L"bad?#";  // illegal character
    plan = BatchRename::Preview({dir + L"\\a.jpg"}, options, nullptr);
    CHECK(plan[0].issue == RenameIssue::InvalidName);
    options.pattern = L"";
    plan = BatchRename::Preview({dir + L"\\a.jpg"}, options, nullptr);
    CHECK(plan[0].issue == RenameIssue::InvalidName);

    options.pattern = std::wstring(240, L'x') + L"#";  // too long for MAX_PATH
    plan = BatchRename::Preview({dir + L"\\a.jpg"}, options, nullptr);
    CHECK(plan[0].issue != RenameIssue::None);

    options.pattern = L"taken";  // an unrelated file already has the name
    plan = BatchRename::Preview({dir + L"\\a.jpg"}, options, nullptr);
    CHECK(plan[0].issue == RenameIssue::Exists);
    RemoveTree(dir);
}

TEST_CASE("batch rename swaps names and undoes") {
    const std::wstring dir = TempDir();
    WriteText(dir + L"\\A.jpg", "first");
    WriteText(dir + L"\\B.jpg", "second");
    const std::wstring undo = dir + L"\\rename-undo.txt";

    std::vector<RenamePlanItem> plan(2);
    plan[0].from = dir + L"\\A.jpg";
    plan[0].to = dir + L"\\B.jpg";
    plan[1].from = dir + L"\\B.jpg";
    plan[1].to = dir + L"\\A.jpg";
    CHECK(BatchRename::Execute(plan, undo) == S_OK);
    CHECK(ReadText(dir + L"\\A.jpg") == "second");
    CHECK(ReadText(dir + L"\\B.jpg") == "first");
    CHECK(BatchRename::CanUndo(undo));

    CHECK(BatchRename::UndoLast(undo) == S_OK);
    CHECK(ReadText(dir + L"\\A.jpg") == "first");
    CHECK(ReadText(dir + L"\\B.jpg") == "second");
    CHECK_FALSE(BatchRename::CanUndo(undo));
    RemoveTree(dir);
}

TEST_CASE("batch rename rolls back after a failure part way") {
    const std::wstring dir = TempDir();
    std::vector<std::wstring> paths;
    for (int i = 0; i < 6; ++i) {
        paths.push_back(dir + L"\\f" + std::to_wstring(i) + L".jpg");
        WriteText(paths.back(), std::to_string(i));
    }
    RenameOptions options;
    options.pattern = L"new_#";
    const auto plan = BatchRename::Preview(paths, options, nullptr);
    REQUIRE_FALSE(BatchRename::HasConflicts(plan));

    for (int failAt : {2, 9}) {  // in phase one, and in phase two
        int calls = 0;
        const auto move = [&](const std::wstring& from, const std::wstring& to) {
            if (calls++ == failAt) {
                SetLastError(ERROR_ACCESS_DENIED);
                return false;
            }
            return MoveFileExW(from.c_str(), to.c_str(), 0) != FALSE;
        };
        CAPTURE(failAt);
        CHECK(BatchRename::Execute(plan, dir + L"\\undo.txt", nullptr, move) == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
        for (int i = 0; i < 6; ++i) CHECK(ReadText(paths[i]) == std::to_string(i));
        CHECK_FALSE(Exists(dir + L"\\new_1.jpg"));
        CHECK_FALSE(BatchRename::CanUndo(dir + L"\\undo.txt"));
    }
    RemoveTree(dir);
}

TEST_CASE("batch rename handles a thousand files quickly") {
    const std::wstring dir = TempDir();
    std::vector<std::wstring> paths;
    for (int i = 0; i < 1000; ++i) {
        paths.push_back(dir + L"\\IMG_" + std::to_wstring(i) + L".jpg");
        WriteText(paths.back(), "x");
    }
    RenameOptions options;
    options.pattern = L"旅遊_####";
    const ULONGLONG start = GetTickCount64();
    const auto plan = BatchRename::Preview(paths, options, nullptr);
    REQUIRE_FALSE(BatchRename::HasConflicts(plan));
    std::size_t lastDone = 0;
    CHECK(BatchRename::Execute(plan, dir + L"\\undo.txt", [&](std::size_t done, std::size_t) { lastDone = done; }) == S_OK);
    const ULONGLONG elapsed = GetTickCount64() - start;
    CHECK(elapsed <= 3000);
    CHECK(lastDone == 2000);
    CHECK(Exists(dir + L"\\旅遊_1000.jpg"));
    CHECK(BatchRename::UndoLast(dir + L"\\undo.txt") == S_OK);
    CHECK(Exists(dir + L"\\IMG_999.jpg"));
    RemoveTree(dir);
}
