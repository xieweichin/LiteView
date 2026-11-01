#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Ctrl+F2. `paths` in list order. Returns S_OK when files were renamed,
// S_FALSE when the user cancelled, or the failure of the rename (after
// which every file has its old name again).
HRESULT ShowBatchRenameDialog(HWND owner, HINSTANCE instance, const std::vector<std::wstring>& paths,
                              const std::wstring& undoFile);
