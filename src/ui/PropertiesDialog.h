#pragma once

#include <windows.h>

#include <string>

// Alt+Enter: file facts plus the basic EXIF fields from core/Metadata.
void ShowPropertiesDialog(HWND owner, HINSTANCE instance, const std::wstring& path);
