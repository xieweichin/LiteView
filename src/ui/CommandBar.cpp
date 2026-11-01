#include "ui/CommandBar.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>

#include "ui/Theme.h"

namespace {

constexpr wchar_t kClassName[] = L"LiteViewCommandBar";
constexpr wchar_t kChevronDown = 0xE70D;

// Sizes in 96-DPI pixels.
constexpr int kBarHeight = 44;
constexpr int kButtonHeight = 32;
constexpr int kIconButtonWidth = 38;
constexpr int kPad = 10;
constexpr int kGlyph = 16;
constexpr int kGlyphGap = 8;
constexpr int kChevronGap = 6;
constexpr int kChevron = 10;
constexpr int kSeparatorWidth = 13;
constexpr int kSeparatorHeight = 20;
constexpr int kMargin = 6;
constexpr int kRadius = 8;  // RoundRect takes the ellipse size, i.e. 2 × 4 px

std::wstring LoadStr(HINSTANCE instance, UINT id) {
    const wchar_t* text = nullptr;
    const int len = id ? LoadStringW(instance, id, reinterpret_cast<LPWSTR>(&text), 0) : 0;
    return (len > 0 && text) ? std::wstring(text, static_cast<size_t>(len)) : std::wstring();
}

HFONT CreateIconFont(int pixels) {
    return CreateFontW(-pixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, theme::IconFontFace());
}

}  // namespace

CommandBar::~CommandBar() {
    if (iconFont_) DeleteObject(iconFont_);
    if (chevronFont_) DeleteObject(chevronFont_);
}

HRESULT CommandBar::Create(HWND parent, HINSTANCE instance) {
    instance_ = instance;
    WNDCLASSEXW wc = {sizeof(wc)};
    if (!GetClassInfoExW(instance, kClassName, &wc)) {
        wc = WNDCLASSEXW{sizeof(wc)};
        wc.lpfnWndProc = &CommandBar::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return HRESULT_FROM_WIN32(GetLastError());
    }
    hwnd_ = CreateWindowExW(0, kClassName, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
                            nullptr, instance, this);
    if (!hwnd_) return HRESULT_FROM_WIN32(GetLastError());
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, instance,
                               nullptr);
    if (tooltip_) theme::ApplyTooltip(tooltip_);
    SetDpi(GetDpiForWindow(hwnd_), nullptr);
    return S_OK;
}

LRESULT CALLBACK CommandBar::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CommandBar* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<CommandBar*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<CommandBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (msg == WM_NCDESTROY && self) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        self->tooltip_ = nullptr;  // destroyed with its owner
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return self ? self->HandleMessage(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CommandBar::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            HDC memory = CreateCompatibleDC(dc);
            HBITMAP canvas = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(memory, canvas);
            Paint(memory, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, memory, 0, 0, SRCCOPY);
            SelectObject(memory, old);
            DeleteObject(canvas);
            DeleteDC(memory);
        }
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_SIZE:
        Layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd_, 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        SetHot(HitTest(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}));
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        SetHot(-1);
        return 0;
    case WM_LBUTTONDOWN: {
        const int index = HitTest(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        if (index < 0) return 0;
        pressed_ = index;
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (slots_[static_cast<std::size_t>(index)].item.kind == Kind::DropDown) {
            // The parent opens its menu synchronously; keep the button
            // looking pressed until the menu closes.
            const UINT command = slots_[static_cast<std::size_t>(index)].item.command;
            SendMessageW(GetParent(hwnd_), WM_COMMAND, MAKEWPARAM(command, 0), reinterpret_cast<LPARAM>(hwnd_));
            pressed_ = -1;
            SetHot(-1);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            SetCapture(hwnd_);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (pressed_ < 0) return 0;
        const int index = HitTest(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        const int pressed = pressed_;
        pressed_ = -1;
        if (GetCapture() == hwnd_) ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (index == pressed) Click(index);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (pressed_ >= 0) {
            pressed_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->code == TTN_GETDISPINFOW) {
            auto* info = reinterpret_cast<NMTTDISPINFOW*>(lParam);
            info->hinst = instance_;
            info->lpszText = MAKEINTRESOURCEW(header->idFrom);  // tooltip string ID == command ID
            return 0;
        }
        break;
    }
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void CommandBar::SetItems(std::vector<Item> items) {
    if (tooltip_) {
        for (const Slot& slot : slots_) {
            TTTOOLINFOW tool = {sizeof(tool)};
            tool.hwnd = hwnd_;
            tool.uId = slot.item.command;
            SendMessageW(tooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
    }
    slots_.clear();
    for (Item& item : items) {
        Slot slot;
        slot.item = item;
        slot.text = LoadStr(instance_, item.label);
        slots_.push_back(std::move(slot));
    }
    hot_ = -1;
    pressed_ = -1;
    if (tooltip_) {
        for (const Slot& slot : slots_) {
            if (slot.item.kind == Kind::Separator || slot.item.kind == Kind::Spacer) continue;
            TTTOOLINFOW tool = {sizeof(tool)};
            tool.uFlags = TTF_SUBCLASS;
            tool.hwnd = hwnd_;
            tool.uId = slot.item.command;
            tool.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
    }
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void CommandBar::SetDpi(UINT dpi, HFONT textFont) {
    dpi_ = dpi;
    textFont_ = textFont;
    if (iconFont_) DeleteObject(iconFont_);
    if (chevronFont_) DeleteObject(chevronFont_);
    iconFont_ = CreateIconFont(MulDiv(kGlyph, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI));
    chevronFont_ = CreateIconFont(MulDiv(kChevron, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI));
    Layout();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void CommandBar::SetChecked(UINT command, bool checked) {
    for (Slot& slot : slots_) {
        if (slot.item.command == command && slot.checked != checked) {
            slot.checked = checked;
            InvalidateRect(hwnd_, &slot.rect, FALSE);
        }
    }
}

void CommandBar::OnThemeChanged() {
    if (tooltip_) theme::ApplyTooltip(tooltip_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int CommandBar::Height() const { return MulDiv(kBarHeight, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); }

int CommandBar::ItemWidth(HDC dc, const Slot& slot) const {
    auto scale = [this](int v) { return MulDiv(v, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); };
    switch (slot.item.kind) {
    case Kind::Separator: return scale(kSeparatorWidth);
    case Kind::Spacer: return 0;
    default: break;
    }
    if (slot.text.empty() && slot.item.kind == Kind::Button) return scale(kIconButtonWidth);
    int width = scale(kPad) * 2;
    if (slot.item.glyph) width += scale(kGlyph);
    if (!slot.text.empty()) {
        HGDIOBJ old = SelectObject(dc, textFont_ ? static_cast<HGDIOBJ>(textFont_) : GetStockObject(DEFAULT_GUI_FONT));
        SIZE size{};
        GetTextExtentPoint32W(dc, slot.text.c_str(), static_cast<int>(slot.text.size()), &size);
        SelectObject(dc, old);
        if (slot.item.glyph) width += scale(kGlyphGap);
        width += size.cx;
    }
    if (slot.item.kind == Kind::DropDown) width += scale(kChevronGap) + scale(kChevron);
    return width;
}

int CommandBar::ContentWidth() const {
    if (!hwnd_) return 0;
    HDC dc = GetDC(hwnd_);
    int total = MulDiv(kMargin, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI) * 2;
    for (const Slot& slot : slots_) total += ItemWidth(dc, slot);
    ReleaseDC(hwnd_, dc);
    return total;
}

void CommandBar::Layout() {
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    HDC dc = GetDC(hwnd_);
    const int margin = MulDiv(kMargin, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
    const int buttonHeight = MulDiv(kButtonHeight, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
    std::vector<int> widths;
    int total = margin * 2;
    for (const Slot& slot : slots_) {
        widths.push_back(ItemWidth(dc, slot));
        total += widths.back();
    }
    ReleaseDC(hwnd_, dc);
    const int spare = std::max(0, static_cast<int>(client.right) - total);
    const int top = (client.bottom - buttonHeight) / 2;
    int x = margin;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const int w = slots_[i].item.kind == Kind::Spacer ? spare : widths[i];
        slots_[i].rect = RECT{x, top, x + w, top + buttonHeight};
        x += w;
        if (tooltip_ && slots_[i].item.kind != Kind::Separator && slots_[i].item.kind != Kind::Spacer) {
            TTTOOLINFOW tool = {sizeof(tool)};
            tool.hwnd = hwnd_;
            tool.uId = slots_[i].item.command;
            tool.rect = slots_[i].rect;
            SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
        }
    }
}

int CommandBar::HitTest(POINT pt) const {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const Kind kind = slots_[i].item.kind;
        if (kind == Kind::Separator || kind == Kind::Spacer) continue;
        if (PtInRect(&slots_[i].rect, pt)) return static_cast<int>(i);
    }
    return -1;
}

void CommandBar::SetHot(int index) {
    if (index == hot_) return;
    hot_ = index;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void CommandBar::Click(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= slots_.size()) return;
    // Posted, because the command may replace this bar's items.
    PostMessageW(GetParent(hwnd_), WM_COMMAND, MAKEWPARAM(slots_[static_cast<std::size_t>(index)].item.command, 0),
                 reinterpret_cast<LPARAM>(hwnd_));
}

bool CommandBar::ItemScreenRect(UINT command, RECT* rect) const {
    for (const Slot& slot : slots_) {
        if (slot.item.command == command && slot.item.kind != Kind::Separator && slot.item.kind != Kind::Spacer) {
            *rect = slot.rect;
            MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(rect), 2);
            return true;
        }
    }
    return false;
}

void CommandBar::Paint(HDC dc, const RECT& client) {
    const theme::Palette& p = theme::Current();
    FillRect(dc, &client, theme::WindowBrush());
    SetBkMode(dc, TRANSPARENT);
    auto scale = [this](int v) { return MulDiv(v, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); };
    const bool enabled = IsWindowEnabled(hwnd_) != FALSE;

    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const Slot& slot = slots_[i];
        RECT r = slot.rect;
        if (slot.item.kind == Kind::Spacer) continue;
        if (slot.item.kind == Kind::Separator) {
            const int cx = (r.left + r.right) / 2;
            const int h = scale(kSeparatorHeight);
            const int cy = (client.top + client.bottom - h) / 2;
            RECT line{cx, cy, cx + std::max(1, scale(1)), cy + h};
            HBRUSH brush = CreateSolidBrush(p.border);
            FillRect(dc, &line, brush);
            DeleteObject(brush);
            continue;
        }
        const bool isPressed = static_cast<int>(i) == pressed_;
        const bool isHot = static_cast<int>(i) == hot_;
        if (isPressed || isHot || slot.checked) {
            const COLORREF fill = (isPressed || slot.checked) ? p.pressed : p.hover;
            HBRUSH brush = CreateSolidBrush(fill);
            HGDIOBJ oldBrush = SelectObject(dc, brush);
            HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, scale(kRadius), scale(kRadius));
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(brush);
        }
        SetTextColor(dc, enabled ? p.text : p.subtleText);

        int x = r.left;
        const bool iconOnly = slot.text.empty() && slot.item.kind == Kind::Button;
        if (iconOnly) {
            HGDIOBJ old = SelectObject(dc, iconFont_);
            wchar_t glyph[2] = {slot.item.glyph, 0};
            DrawTextW(dc, glyph, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, old);
            continue;
        }
        x += scale(kPad);
        if (slot.item.glyph) {
            RECT g{x, r.top, x + scale(kGlyph), r.bottom};
            HGDIOBJ old = SelectObject(dc, iconFont_);
            wchar_t glyph[2] = {slot.item.glyph, 0};
            DrawTextW(dc, glyph, 1, &g, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, old);
            x += scale(kGlyph);
            if (!slot.text.empty()) x += scale(kGlyphGap);
        }
        if (!slot.text.empty()) {
            const int textRight = slot.item.kind == Kind::DropDown
                                      ? r.right - scale(kPad) - scale(kChevron) - scale(kChevronGap)
                                      : r.right - scale(kPad);
            RECT t{x, r.top, textRight, r.bottom};
            HGDIOBJ old = SelectObject(dc, textFont_ ? static_cast<HGDIOBJ>(textFont_) : GetStockObject(DEFAULT_GUI_FONT));
            DrawTextW(dc, slot.text.c_str(), static_cast<int>(slot.text.size()), &t,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, old);
        }
        if (slot.item.kind == Kind::DropDown) {
            RECT c{r.right - scale(kPad) - scale(kChevron), r.top, r.right - scale(kPad), r.bottom};
            HGDIOBJ old = SelectObject(dc, chevronFont_);
            wchar_t glyph[2] = {kChevronDown, 0};
            DrawTextW(dc, glyph, 1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, old);
        }
    }
}
