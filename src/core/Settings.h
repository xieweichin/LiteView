#pragma once

#include <windows.h>

#include <mutex>
#include <string>

// INI settings are stored next to the executable in portable mode, otherwise
// under the current user's roaming application-data directory.
class Settings {
public:
    Settings() = default;
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    static HRESULT ForCurrentProcess(Settings& out) noexcept;
    static HRESULT FromDirectories(const std::wstring& executableDirectory,
                                   const std::wstring& roamingDirectory,
                                   Settings& out) noexcept;

    const std::wstring& FilePath() const noexcept { return path_; }
    bool IsPortable() const noexcept { return portable_; }

    HRESULT ReadString(const std::wstring& section, const std::wstring& key,
                       const std::wstring& fallback, std::wstring& value) const noexcept;
    HRESULT WriteString(const std::wstring& section, const std::wstring& key,
                        const std::wstring& value) noexcept;
    HRESULT ReadInt(const std::wstring& section, const std::wstring& key,
                    int fallback, int& value) const noexcept;
    HRESULT WriteInt(const std::wstring& section, const std::wstring& key, int value) noexcept;
    HRESULT ReadBool(const std::wstring& section, const std::wstring& key,
                     bool fallback, bool& value) const noexcept;
    HRESULT WriteBool(const std::wstring& section, const std::wstring& key, bool value) noexcept;

private:
    HRESULT EnsureUnicodeFile() const;
    mutable std::mutex mutex_;
    std::wstring path_;
    std::wstring directory_;
    bool portable_ = false;
};
