#include "doctest.h"

#include "core/Settings.h"

#include <atomic>
#include <string>
#include <vector>

namespace {

std::atomic<unsigned> fixtureSequence{0};

struct SettingsFixture {
    std::wstring root;
    std::wstring executable;
    std::wstring roaming;

    SettingsFixture() {
        wchar_t temp[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        REQUIRE(length > 0);
        REQUIRE(length < MAX_PATH);
        root = std::wstring(temp) + L"LiteView-測試-" + std::to_wstring(GetCurrentProcessId()) +
               L"-" + std::to_wstring(fixtureSequence.fetch_add(1));
        REQUIRE(CreateDirectoryW(root.c_str(), nullptr));
        executable = root + L"\\exe";
        roaming = root + L"\\roaming";
        REQUIRE(CreateDirectoryW(executable.c_str(), nullptr));
        REQUIRE(CreateDirectoryW(roaming.c_str(), nullptr));
    }

    ~SettingsFixture() {
        DeleteFileW((executable + L"\\portable.ini").c_str());
        DeleteFileW((roaming + L"\\LiteView\\settings.ini").c_str());
        RemoveDirectoryW((roaming + L"\\LiteView").c_str());
        RemoveDirectoryW(executable.c_str());
        RemoveDirectoryW(roaming.c_str());
        RemoveDirectoryW(root.c_str());
    }

    void WriteFile(const std::wstring& path, const std::vector<unsigned char>& bytes) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(file != INVALID_HANDLE_VALUE);
        DWORD written = 0;
        const BOOL success = bytes.empty() || ::WriteFile(file, bytes.data(),
                                                            static_cast<DWORD>(bytes.size()), &written, nullptr);
        CloseHandle(file);
        REQUIRE(success);
        REQUIRE(written == bytes.size());
    }

    bool HasUtf16Bom(const std::wstring& path) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        unsigned char bom[2]{};
        DWORD read = 0;
        const BOOL ok = ReadFile(file, bom, sizeof(bom), &read, nullptr);
        CloseHandle(file);
        return ok && read == sizeof(bom) && bom[0] == 0xFF && bom[1] == 0xFE;
    }

    bool HasUtf8Bom(const std::wstring& path) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        unsigned char bom[3]{};
        DWORD read = 0;
        const BOOL ok = ReadFile(file, bom, sizeof(bom), &read, nullptr);
        CloseHandle(file);
        return ok && read == sizeof(bom) && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF;
    }
};

}  // namespace

TEST_CASE("roaming settings preserve Unicode, negative integers, and booleans") {
    SettingsFixture fixture;
    Settings settings;
    REQUIRE(Settings::FromDirectories(fixture.executable, fixture.roaming, settings) == S_OK);
    CHECK_FALSE(settings.IsPortable());
    CHECK(settings.FilePath() == fixture.roaming + L"\\LiteView\\settings.ini");

    std::wstring text;
    CHECK(settings.ReadString(L"一般", L"路徑", L"預設", text) == S_OK);
    CHECK(text == L"預設");
    CHECK(settings.WriteString(L"一般", L"路徑", L"D:\\照片\\測試") == S_OK);
    CHECK(settings.WriteInt(L"Window", L"Left", -1920) == S_OK);
    CHECK(settings.WriteBool(L"Window", L"Maximized", true) == S_OK);
    CHECK(fixture.HasUtf16Bom(settings.FilePath()));

    int left = 0;
    bool maximized = false;
    CHECK(settings.ReadString(L"一般", L"路徑", L"", text) == S_OK);
    CHECK(text == L"D:\\照片\\測試");
    CHECK(settings.ReadInt(L"Window", L"Left", 0, left) == S_OK);
    CHECK(left == -1920);
    CHECK(settings.ReadBool(L"Window", L"Maximized", false, maximized) == S_OK);
    CHECK(maximized);
}

TEST_CASE("empty portable.ini selects portable mode and becomes UTF-16") {
    SettingsFixture fixture;
    fixture.WriteFile(fixture.executable + L"\\portable.ini", {});
    Settings settings;
    REQUIRE(Settings::FromDirectories(fixture.executable, fixture.roaming, settings) == S_OK);
    CHECK(settings.IsPortable());
    CHECK(settings.FilePath() == fixture.executable + L"\\portable.ini");
    REQUIRE(settings.WriteString(L"一般", L"名稱", L"八德") == S_OK);
    CHECK(fixture.HasUtf16Bom(settings.FilePath()));
    std::wstring name;
    CHECK(settings.ReadString(L"一般", L"名稱", L"", name) == S_OK);
    CHECK(name == L"八德");
    CHECK(GetFileAttributesW((fixture.roaming + L"\\LiteView").c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("existing ANSI portable settings are preserved on write") {
    SettingsFixture fixture;
    const std::wstring path = fixture.executable + L"\\portable.ini";
    const std::string content = "[General]\r\nOld=abc\r\n";
    fixture.WriteFile(path, std::vector<unsigned char>(content.begin(), content.end()));
    Settings settings;
    REQUIRE(Settings::FromDirectories(fixture.executable, fixture.roaming, settings) == S_OK);
    REQUIRE(settings.WriteString(L"General", L"New", L"繁體中文") == S_OK);
    CHECK(fixture.HasUtf16Bom(path));
    std::wstring old;
    std::wstring added;
    CHECK(settings.ReadString(L"General", L"Old", L"", old) == S_OK);
    CHECK(settings.ReadString(L"General", L"New", L"", added) == S_OK);
    CHECK(old == L"abc");
    CHECK(added == L"繁體中文");
}

TEST_CASE("existing UTF-8 portable settings are converted without losing text") {
    SettingsFixture fixture;
    const std::wstring path = fixture.executable + L"\\portable.ini";
    const std::string content = u8"[General]\r\nOld=臺灣\r\n";
    std::vector<unsigned char> bytes{0xEF, 0xBB, 0xBF};
    bytes.insert(bytes.end(), content.begin(), content.end());
    fixture.WriteFile(path, bytes);
    Settings settings;
    REQUIRE(Settings::FromDirectories(fixture.executable, fixture.roaming, settings) == S_OK);
    std::wstring beforeWrite;
    CHECK(settings.ReadString(L"General", L"Old", L"", beforeWrite) == S_OK);
    CHECK(beforeWrite == L"臺灣");
    CHECK(fixture.HasUtf8Bom(path));
    REQUIRE(settings.WriteString(L"General", L"New", L"八德") == S_OK);
    CHECK(fixture.HasUtf16Bom(path));
    std::wstring old;
    std::wstring added;
    CHECK(settings.ReadString(L"General", L"Old", L"", old) == S_OK);
    CHECK(settings.ReadString(L"General", L"New", L"", added) == S_OK);
    CHECK(old == L"臺灣");
    CHECK(added == L"八德");
}

TEST_CASE("UTF-16 big-endian settings are readable and safely converted") {
    SettingsFixture fixture;
    const std::wstring path = fixture.executable + L"\\portable.ini";
    const std::wstring content = L"[General]\r\nOld=臺灣\r\n";
    std::vector<unsigned char> bytes{0xFE, 0xFF};
    for (wchar_t character : content) {
        bytes.push_back(static_cast<unsigned char>((character >> 8) & 0xFF));
        bytes.push_back(static_cast<unsigned char>(character & 0xFF));
    }
    fixture.WriteFile(path, bytes);
    Settings settings;
    REQUIRE(Settings::FromDirectories(fixture.executable, fixture.roaming, settings) == S_OK);
    std::wstring value;
    CHECK(settings.ReadString(L"General", L"Old", L"", value) == S_OK);
    CHECK(value == L"臺灣");
    REQUIRE(settings.WriteInt(L"General", L"Count", 2) == S_OK);
    CHECK(fixture.HasUtf16Bom(path));
    CHECK(settings.ReadString(L"General", L"Old", L"", value) == S_OK);
    CHECK(value == L"臺灣");
}

TEST_CASE("malformed settings use fallbacks and report an error") {
    SettingsFixture fixture;
    Settings settings;
    REQUIRE(Settings::FromDirectories(fixture.executable, fixture.roaming, settings) == S_OK);
    REQUIRE(settings.WriteString(L"Window", L"Left", L"not-a-number") == S_OK);
    REQUIRE(settings.WriteString(L"Window", L"Maximized", L"perhaps") == S_OK);
    int left = 0;
    bool maximized = false;
    CHECK(FAILED(settings.ReadInt(L"Window", L"Left", -50, left)));
    CHECK(left == -50);
    CHECK(FAILED(settings.ReadBool(L"Window", L"Maximized", true, maximized)));
    CHECK(maximized);
    CHECK(settings.WriteString(L"Window", L"Bad=key", L"value") == E_INVALIDARG);
    CHECK(settings.WriteString(L"Window", L"Key", L"line1\nline2") == E_INVALIDARG);
}

TEST_CASE("invalid UTF-8 settings are left unchanged on write failure") {
    SettingsFixture fixture;
    const std::wstring path = fixture.executable + L"\\portable.ini";
    const std::vector<unsigned char> invalid{0xEF, 0xBB, 0xBF, 0xFF};
    fixture.WriteFile(path, invalid);
    Settings settings;
    REQUIRE(Settings::FromDirectories(fixture.executable, fixture.roaming, settings) == S_OK);
    CHECK(FAILED(settings.WriteString(L"General", L"New", L"值")));
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    unsigned char bytes[4]{};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, bytes, sizeof(bytes), &read, nullptr);
    CloseHandle(file);
    CHECK(ok);
    CHECK(read == invalid.size());
    CHECK(std::vector<unsigned char>(bytes, bytes + read) == invalid);
}
