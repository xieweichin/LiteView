#pragma once

#include <windows.h>

#include <string>
#include <vector>

// A flat Windows 11 style command bar: icon buttons drawn with the system
// icon font, optional text labels and drop-down buttons. Clicking an item
// sends WM_COMMAND(item.command) to the parent; drop-down items send it on
// mouse down so the parent can open a menu under ItemScreenRect().
// Tooltips use the string resource whose ID equals the command ID.
class CommandBar {
public:
    enum class Kind { Button, DropDown, Separator, Spacer };
    struct Item {
        Kind kind = Kind::Button;
        UINT command = 0;
        wchar_t glyph = 0;  // Segoe Fluent Icons / MDL2 code point
        UINT label = 0;     // string ID shown next to the glyph, 0 for icon only
    };

    CommandBar() = default;
    ~CommandBar();
    CommandBar(const CommandBar&) = delete;
    CommandBar& operator=(const CommandBar&) = delete;

    HRESULT Create(HWND parent, HINSTANCE instance);
    HWND Hwnd() const { return hwnd_; }

    void SetItems(std::vector<Item> items);
    // `textFont` is owned by the caller and must outlive the bar.
    void SetDpi(UINT dpi, HFONT textFont);
    void SetChecked(UINT command, bool checked);
    void OnThemeChanged();

    int Height() const;         // preferred height at the current DPI
    int ContentWidth() const;   // width of all items without the spacer
    bool ItemScreenRect(UINT command, RECT* rect) const;

private:
    struct Slot {
        Item item;
        std::wstring text;
        RECT rect{};
        bool checked = false;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void Layout();
    void Paint(HDC dc, const RECT& client);
    int HitTest(POINT pt) const;
    int ItemWidth(HDC dc, const Slot& slot) const;
    void SetHot(int index);
    void UpdateTools();
    void Click(int index);

    HWND hwnd_ = nullptr;
    HWND tooltip_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HFONT textFont_ = nullptr;   // not owned
    HFONT iconFont_ = nullptr;   // owned
    HFONT chevronFont_ = nullptr;  // owned, smaller glyphs
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    std::vector<Slot> slots_;
    int hot_ = -1;
    int pressed_ = -1;
    bool tracking_ = false;
};
