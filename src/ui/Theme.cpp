#include "ui/Theme.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

namespace theme {
namespace {

// Undocumented uxtheme exports (by ordinal) that Explorer, Notepad and the
// File dialogs use for dark menus and scroll bars. All are optional: when
// missing (old builds, Wine) only the custom-drawn parts follow the theme.
enum class AppMode { Default, AllowDark, ForceDark, ForceLight };
using SetPreferredAppModeFn = AppMode(WINAPI*)(AppMode);         // ordinal 135, 1903+
using AllowDarkModeForAppFn = BOOL(WINAPI*)(BOOL);               // ordinal 135, 1809
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);      // ordinal 133
using FlushMenuThemesFn = void(WINAPI*)();                       // ordinal 136
using RefreshImmersiveColorPolicyStateFn = void(WINAPI*)();      // ordinal 104

// DWM attributes newer than some SDKs.
constexpr DWORD kDwmUseDarkModeOld = 19;  // builds 17763 – 18985
constexpr DWORD kDwmUseDarkMode = 20;
constexpr DWORD kDwmCornerPreference = 33;
constexpr DWORD kDwmCaptionColor = 35;
constexpr DWORD kDwmCornerRound = 2;
constexpr DWORD kDwmColorDefault = 0xFFFFFFFF;

constexpr UINT_PTR kListSubclassId = 0x4C56;

DWORD g_build = 0;
FARPROC g_appMode = nullptr;
AllowDarkModeForWindowFn g_allowForWindow = nullptr;
FlushMenuThemesFn g_flushMenus = nullptr;
RefreshImmersiveColorPolicyStateFn g_refreshPolicy = nullptr;

Palette g_palette{};
HBRUSH g_windowBrush = nullptr;
HBRUSH g_surfaceBrush = nullptr;
HBRUSH g_fieldBrush = nullptr;

DWORD WindowsBuild() {
    using Fn = void(WINAPI*)(DWORD*, DWORD*, DWORD*);
    DWORD major = 0, minor = 0, build = 0;
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetNtVersionNumbers")))) {
            fn(&major, &minor, &build);
        }
    }
    return major >= 10 ? (build & 0x0FFFFFFF) : 0;
}

bool ReadDword(const wchar_t* key, const wchar_t* name, DWORD& value) {
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
}

bool HighContrastOn() {
    HIGHCONTRASTW hc = {sizeof(hc)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

bool AppsUseDark() {
    DWORD light = 1;
    ReadDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", light);
    return light == 0;
}

COLORREF Mix(COLORREF a, COLORREF b, int percentB) {
    auto ch = [&](int shift) {
        const int x = (a >> shift) & 0xFF, y = (b >> shift) & 0xFF;
        return static_cast<COLORREF>((x * (100 - percentB) + y * percentB) / 100) << shift;
    };
    return ch(0) | ch(8) | ch(16);
}

COLORREF AccentColor(bool dark) {
    DWORD abgr = 0;
    COLORREF accent = RGB(0, 95, 184);
    if (ReadDword(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", abgr)) accent = abgr & 0x00FFFFFF;
    // Windows uses a lighter shade of the accent on dark backgrounds.
    return dark ? Mix(accent, RGB(255, 255, 255), 45) : accent;
}

Palette MakePalette() {
    Palette p{};
    if (HighContrastOn()) {
        p.highContrast = true;
        p.window = GetSysColor(COLOR_BTNFACE);
        p.surface = GetSysColor(COLOR_WINDOW);
        p.text = GetSysColor(COLOR_WINDOWTEXT);
        p.subtleText = GetSysColor(COLOR_GRAYTEXT);
        p.border = GetSysColor(COLOR_WINDOWFRAME);
        p.hover = GetSysColor(COLOR_3DLIGHT);
        p.pressed = GetSysColor(COLOR_BTNSHADOW);
        p.field = GetSysColor(COLOR_WINDOW);
        p.accent = GetSysColor(COLOR_HIGHLIGHT);
        p.viewer = GetSysColor(COLOR_WINDOW);
        p.error = GetSysColor(COLOR_WINDOWTEXT);
        return p;
    }
    if (AppsUseDark() && g_build >= 17763) {
        p.dark = true;
        p.window = RGB(32, 32, 32);
        p.surface = RGB(25, 25, 25);
        p.text = RGB(242, 242, 242);
        p.subtleText = RGB(130, 130, 130);
        p.border = RGB(52, 52, 52);
        p.hover = RGB(50, 50, 50);
        p.pressed = RGB(62, 62, 62);
        p.field = RGB(44, 44, 44);
        p.accent = AccentColor(true);
        p.viewer = RGB(28, 28, 28);
        p.error = RGB(255, 153, 164);
    } else {
        p.window = RGB(243, 243, 243);
        p.surface = RGB(255, 255, 255);
        p.text = RGB(26, 26, 26);
        p.subtleText = RGB(140, 140, 140);
        p.border = RGB(224, 224, 224);
        p.hover = RGB(230, 230, 230);
        p.pressed = RGB(218, 218, 218);
        p.field = RGB(255, 255, 255);
        p.accent = AccentColor(false);
        p.viewer = RGB(243, 243, 243);
        p.error = RGB(196, 43, 28);
    }
    return p;
}

void ReplaceBrush(HBRUSH& brush, COLORREF color) {
    if (brush) DeleteObject(brush);
    brush = CreateSolidBrush(color);
}

void ApplyAppMode() {
    if (!g_appMode) return;
    if (g_build >= 18362) {
        reinterpret_cast<SetPreferredAppModeFn>(reinterpret_cast<void*>(g_appMode))(
            g_palette.dark ? AppMode::ForceDark : AppMode::ForceLight);
    } else {
        reinterpret_cast<AllowDarkModeForAppFn>(reinterpret_cast<void*>(g_appMode))(g_palette.dark);
    }
    if (g_refreshPolicy) g_refreshPolicy();
    if (g_flushMenus) g_flushMenus();
}

void AllowDark(HWND hwnd) {
    if (g_allowForWindow) g_allowForWindow(hwnd, g_palette.dark);
}

// Header text is not recoloured by the dark ItemsView theme on every build.
LRESULT CALLBACK ListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NOTIFY) {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->code == NM_CUSTOMDRAW && header->hwndFrom == ListView_GetHeader(hwnd) && g_palette.dark) {
            auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
            if (draw->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(draw->hdc, g_palette.text);
                return CDRF_DODEFAULT;
            }
        }
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, &ListSubclassProc, kListSubclassId);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

BOOL CALLBACK ApplyChild(HWND child, LPARAM) {
    wchar_t cls[32] = {};
    GetClassNameW(child, cls, ARRAYSIZE(cls));
    const bool dark = g_palette.dark;
    if (lstrcmpiW(cls, WC_BUTTONW) == 0) {
        const LONG_PTR type = GetWindowLongPtrW(child, GWL_STYLE) & BS_TYPEMASK;
        const bool box = type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_RADIOBUTTON ||
                         type == BS_AUTORADIOBUTTON || type == BS_3STATE || type == BS_AUTO3STATE;
        AllowDark(child);
        if (box && dark) {
            // Themed check boxes ignore WM_CTLCOLORSTATIC text colour.
            SetWindowTheme(child, L"", L"");
        } else {
            SetWindowTheme(child, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        }
    } else if (lstrcmpiW(cls, WC_EDITW) == 0) {
        AllowDark(child);
        SetWindowTheme(child, dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
    } else if (lstrcmpiW(cls, WC_LISTVIEWW) == 0) {
        ApplyListView(child);
    } else if (lstrcmpiW(cls, WC_TREEVIEWW) == 0) {
        ApplyTree(child);
    } else if (lstrcmpiW(cls, L"msctls_progress32") == 0) {
        AllowDark(child);
        SetWindowTheme(child, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    }
    SendMessageW(child, WM_THEMECHANGED, 0, 0);
    return TRUE;
}

int CALLBACK FontExists(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
    *reinterpret_cast<bool*>(found) = true;
    return 0;
}

bool HasFont(const wchar_t* face) {
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, &FontExists, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

}  // namespace

void InitProcess() {
    g_build = WindowsBuild();
    if (g_build >= 17763) {
        if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            g_appMode = GetProcAddress(ux, MAKEINTRESOURCEA(135));
            g_allowForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(
                reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(133))));
            g_flushMenus = reinterpret_cast<FlushMenuThemesFn>(
                reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136))));
            g_refreshPolicy = reinterpret_cast<RefreshImmersiveColorPolicyStateFn>(
                reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(104))));
            // uxtheme stays loaded for the process lifetime.
        }
    }
    g_palette = Palette{};
    g_palette.window = 1;  // force Refresh() to see a change
    Refresh();
}

bool Refresh() {
    const Palette next = MakePalette();
    const Palette& cur = g_palette;
    const bool changed = next.dark != cur.dark || next.highContrast != cur.highContrast ||
                         next.window != cur.window || next.surface != cur.surface || next.text != cur.text ||
                         next.accent != cur.accent || next.field != cur.field;
    if (!changed) return false;
    g_palette = next;
    ReplaceBrush(g_windowBrush, g_palette.window);
    ReplaceBrush(g_surfaceBrush, g_palette.surface);
    ReplaceBrush(g_fieldBrush, g_palette.field);
    ApplyAppMode();
    return true;
}

bool IsThemeChangeMessage(UINT msg, LPARAM lParam) {
    if (msg == WM_SYSCOLORCHANGE || msg == WM_THEMECHANGED) return true;
    if (msg == WM_SETTINGCHANGE && lParam) {
        return CompareStringOrdinal(reinterpret_cast<const wchar_t*>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) ==
               CSTR_EQUAL;
    }
    return false;
}

const Palette& Current() { return g_palette; }
HBRUSH WindowBrush() { return g_windowBrush; }
HBRUSH SurfaceBrush() { return g_surfaceBrush; }
HBRUSH FieldBrush() { return g_fieldBrush; }

void ApplyFrame(HWND hwnd) {
    AllowDark(hwnd);
    if (g_build >= 17763) {
        const BOOL dark = g_palette.dark;
        DwmSetWindowAttribute(hwnd, g_build >= 18985 ? kDwmUseDarkMode : kDwmUseDarkModeOld, &dark, sizeof(dark));
    }
    if (g_build >= 22000) {
        // Windows 11: rounded corners and a caption that blends into the
        // command bar below it.
        const DWORD corner = kDwmCornerRound;
        DwmSetWindowAttribute(hwnd, kDwmCornerPreference, &corner, sizeof(corner));
        const DWORD caption = g_palette.highContrast ? kDwmColorDefault : g_palette.window;
        DwmSetWindowAttribute(hwnd, kDwmCaptionColor, &caption, sizeof(caption));
    }
}

void ApplyTree(HWND tree) {
    AllowDark(tree);
    SetWindowTheme(tree, g_palette.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    TreeView_SetBkColor(tree, g_palette.surface);
    TreeView_SetTextColor(tree, g_palette.text);
    if (HWND tip = TreeView_GetToolTips(tree)) ApplyTooltip(tip);
}

void ApplyListView(HWND list) {
    AllowDark(list);
    SetWindowTheme(list, g_palette.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    if (HWND header = ListView_GetHeader(list)) {
        AllowDark(header);
        SetWindowTheme(header, g_palette.dark ? L"DarkMode_ItemsView" : L"ItemsView", nullptr);
    }
    if (HWND tip = ListView_GetToolTips(list)) ApplyTooltip(tip);
    ListView_SetBkColor(list, g_palette.surface);
    ListView_SetTextBkColor(list, g_palette.surface);
    ListView_SetTextColor(list, g_palette.text);
    SetWindowSubclass(list, &ListSubclassProc, kListSubclassId, 0);
}

void ApplyTooltip(HWND tooltip) {
    AllowDark(tooltip);
    SetWindowTheme(tooltip, g_palette.dark ? L"DarkMode_Explorer" : nullptr, nullptr);
}

void ApplyDialog(HWND dialog) {
    ApplyFrame(dialog);
    EnumChildWindows(dialog, &ApplyChild, 0);
    InvalidateRect(dialog, nullptr, TRUE);
}

bool HandleCtlColor(UINT msg, WPARAM wParam, LPARAM, LRESULT& result) {
    HDC dc = reinterpret_cast<HDC>(wParam);
    switch (msg) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor(dc, g_palette.text);
        SetBkColor(dc, g_palette.window);
        result = reinterpret_cast<LRESULT>(g_windowBrush);
        return true;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(dc, g_palette.text);
        SetBkColor(dc, g_palette.field);
        result = reinterpret_cast<LRESULT>(g_fieldBrush);
        return true;
    }
    return false;
}

const wchar_t* IconFontFace() {
    static const wchar_t* face = HasFont(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    return face;
}

}  // namespace theme
