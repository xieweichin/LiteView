// Light / dark appearance that follows the Windows "app mode" setting, plus
// helpers that give standard controls the Windows 10/11 look.
#pragma once

#include <windows.h>

namespace theme {

struct Palette {
    bool dark = false;
    bool highContrast = false;
    COLORREF window;      // chrome: command bar, status bar, splitters
    COLORREF surface;     // panes: folder tree, file list, preview
    COLORREF text;
    COLORREF subtleText;  // disabled glyphs, secondary text
    COLORREF border;      // hairlines between areas
    COLORREF hover;       // command bar button under the mouse
    COLORREF pressed;     // pressed or checked button
    COLORREF field;       // address bar and edit boxes
    COLORREF accent;      // focus underline
    COLORREF viewer;      // viewer background (full screen stays black)
    COLORREF error;       // red text (batch rename conflicts)
};

// Call once before creating windows: reads the system setting and lets
// menus, scroll bars and tooltips follow it.
void InitProcess();

// Re-reads the system setting. Returns true when the palette changed.
bool Refresh();

// True for WM_SETTINGCHANGE("ImmersiveColorSet"), WM_SYSCOLORCHANGE and
// WM_THEMECHANGED: the caller should Refresh() and re-apply.
bool IsThemeChangeMessage(UINT msg, LPARAM lParam);

const Palette& Current();
HBRUSH WindowBrush();
HBRUSH SurfaceBrush();
HBRUSH FieldBrush();

// Dark title bar and Windows 11 rounded corners / caption colour.
void ApplyFrame(HWND topLevel);
void ApplyTree(HWND tree);
void ApplyListView(HWND list);
void ApplyTooltip(HWND tooltip);
// Frame plus every child control of a dialog.
void ApplyDialog(HWND dialog);

// Answers WM_CTLCOLOR* with the palette. Returns false for other messages.
bool HandleCtlColor(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result);

// "Segoe Fluent Icons" on Windows 11, "Segoe MDL2 Assets" on Windows 10.
const wchar_t* IconFontFace();

}  // namespace theme
