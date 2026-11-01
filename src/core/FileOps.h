#pragma once

#include <windows.h>

#include <string>
#include <vector>

// File management helpers. Copy, move and recycle go through
// IFileOperation so they get the system progress, conflict and undo
// behaviour; they must run on an STA thread (the UI thread).
namespace FileOps {

// True if `name` can be a file name: not empty, no \ / : * ? " < > | or
// control characters, no trailing dot or space, not a reserved device name.
bool IsValidFileName(const std::wstring& name) noexcept;

// "新增資料夾", then "新增資料夾 (2)", ... — the first name not present in `folder`.
std::wstring UniqueChildName(const std::wstring& folder, const std::wstring& base);

// Renames within the same folder. Fails if the target exists, except for a
// change of letter case only.
HRESULT Rename(const std::wstring& from, const std::wstring& to) noexcept;

HRESULT CopyOrMove(HWND owner, const std::vector<std::wstring>& paths, const std::wstring& destination,
                   bool move) noexcept;

// Moves to the Recycle Bin. Returns S_FALSE if the user cancelled.
HRESULT Recycle(HWND owner, const std::vector<std::wstring>& paths) noexcept;

}  // namespace FileOps
