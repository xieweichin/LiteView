#include "ui/BatchRenameDialog.h"

#include <commctrl.h>
#include <windowsx.h>

#include <optional>
#include <unordered_map>

#include "core/BatchRename.h"
#include "res/resource.h"

namespace {

constexpr UINT_PTR kPreviewTimer = 1;
constexpr UINT kPreviewDelayMs = 150;

// Remembered for the next time the dialog opens in this run.
std::wstring g_lastPattern = L"*_###";
int g_lastStart = 1;
bool g_lastLowercase = false;

std::wstring LoadStr(HINSTANCE instance, UINT id) {
    const wchar_t* text = nullptr;
    const int len = LoadStringW(instance, id, reinterpret_cast<LPWSTR>(&text), 0);
    return (len > 0 && text) ? std::wstring(text, static_cast<size_t>(len)) : std::wstring();
}

std::wstring FileName(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring WindowText(HWND hwnd) {
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(hwnd)) + 1, L'\0');
    text.resize(static_cast<std::size_t>(GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()))));
    return text;
}

struct DialogState {
    HINSTANCE instance = nullptr;
    const std::vector<std::wstring>* paths = nullptr;
    std::wstring undoFile;
    std::vector<RenamePlanItem> plan;
    std::optional<std::unordered_map<std::wstring, SYSTEMTIME>> dates;  // read on first use of {date:}
    std::vector<std::wstring> issueText;           // per RenameIssue
    std::wstring cell;                             // LVN_GETDISPINFO text buffer
    HRESULT result = S_FALSE;
    bool running = false;
};

void UpdatePreview(HWND dialog, DialogState& state) {
    RenameOptions options;
    options.pattern = WindowText(GetDlgItem(dialog, IDC_BR_PATTERN));
    BOOL ok = FALSE;
    options.startNumber = static_cast<int>(GetDlgItemInt(dialog, IDC_BR_START, &ok, FALSE));
    if (!ok) options.startNumber = 1;
    options.lowercaseExtension = IsDlgButtonChecked(dialog, IDC_BR_LOWER_EXT) == BST_CHECKED;

    DateFunction dateOf;
    if (options.pattern.find(L"{date:") != std::wstring::npos) {
        if (!state.dates) {
            HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            std::unordered_map<std::wstring, SYSTEMTIME> dates;
            for (const auto& path : *state.paths) dates.emplace(path, BatchRename::DateTakenOrModified(path));
            state.dates = std::move(dates);
            SetCursor(old);
        }
        const std::unordered_map<std::wstring, SYSTEMTIME>* dates = &*state.dates;
        dateOf = [dates](const std::wstring& path) {
            const auto it = dates->find(path);
            return it != dates->end() ? it->second : SYSTEMTIME{};
        };
    }
    state.plan = BatchRename::Preview(*state.paths, options, dateOf);

    std::size_t conflicts = 0;
    for (const auto& item : state.plan) {
        if (item.issue != RenameIssue::None) ++conflicts;
    }
    HWND list = GetDlgItem(dialog, IDC_BR_LIST);
    ListView_SetItemCountEx(list, static_cast<int>(state.plan.size()), LVSICF_NOSCROLL);
    InvalidateRect(list, nullptr, FALSE);

    wchar_t status[256] = {};
    if (conflicts) {
        swprintf(status, 256, LoadStr(state.instance, IDS_BR_CONFLICTS).c_str(), static_cast<unsigned>(conflicts));
    } else {
        swprintf(status, 256, LoadStr(state.instance, IDS_BR_READY).c_str(), static_cast<unsigned>(state.plan.size()));
    }
    SetDlgItemTextW(dialog, IDC_BR_STATUS, status);
    EnableWindow(GetDlgItem(dialog, IDOK), conflicts == 0 && !state.plan.empty());
}

void InitDialog(HWND dialog, DialogState& state) {
    HWND list = GetDlgItem(dialog, IDC_BR_LIST);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    RECT rc{};
    GetClientRect(list, &rc);
    const int width = rc.right - GetSystemMetrics(SM_CXVSCROLL);
    const UINT titles[] = {IDS_BR_COL_FROM, IDS_BR_COL_TO, IDS_BR_COL_ISSUE};
    const int widths[] = {width * 2 / 5, width * 2 / 5, width - 4 * (width / 5)};
    for (int i = 0; i < 3; ++i) {
        std::wstring title = LoadStr(state.instance, titles[i]);
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = title.data();
        column.cx = widths[i];
        ListView_InsertColumn(list, i, &column);
    }
    state.issueText = {L"",
                       LoadStr(state.instance, IDS_BR_ISSUE_DUPLICATE),
                       LoadStr(state.instance, IDS_BR_ISSUE_INVALID),
                       LoadStr(state.instance, IDS_BR_ISSUE_TOO_LONG),
                       LoadStr(state.instance, IDS_BR_ISSUE_EXISTS)};

    SendDlgItemMessageW(dialog, IDC_BR_PATTERN, EM_LIMITTEXT, 200, 0);
    SendDlgItemMessageW(dialog, IDC_BR_START, EM_LIMITTEXT, 7, 0);
    SetDlgItemTextW(dialog, IDC_BR_PATTERN, g_lastPattern.c_str());
    SetDlgItemInt(dialog, IDC_BR_START, static_cast<UINT>(g_lastStart), FALSE);
    CheckDlgButton(dialog, IDC_BR_LOWER_EXT, g_lastLowercase ? BST_CHECKED : BST_UNCHECKED);
    ShowWindow(GetDlgItem(dialog, IDC_BR_PROGRESS), SW_HIDE);
    UpdatePreview(dialog, state);
    SendDlgItemMessageW(dialog, IDC_BR_PATTERN, EM_SETSEL, 0, -1);
}

void Execute(HWND dialog, DialogState& state) {
    KillTimer(dialog, kPreviewTimer);
    UpdatePreview(dialog, state);  // the files may have changed since the last preview
    if (BatchRename::HasConflicts(state.plan)) return;

    g_lastPattern = WindowText(GetDlgItem(dialog, IDC_BR_PATTERN));
    BOOL ok = FALSE;
    const UINT start = GetDlgItemInt(dialog, IDC_BR_START, &ok, FALSE);
    g_lastStart = ok ? static_cast<int>(start) : 1;
    g_lastLowercase = IsDlgButtonChecked(dialog, IDC_BR_LOWER_EXT) == BST_CHECKED;

    HWND progress = GetDlgItem(dialog, IDC_BR_PROGRESS);
    ShowWindow(progress, SW_SHOW);
    SendMessageW(progress, PBM_SETRANGE32, 0, 1000);
    state.running = true;
    for (int id : {IDOK, IDCANCEL, IDC_BR_PATTERN, IDC_BR_START, IDC_BR_LOWER_EXT}) EnableWindow(GetDlgItem(dialog, id), FALSE);
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const auto onProgress = [progress](std::size_t done, std::size_t total) {
        SendMessageW(progress, PBM_SETPOS, total ? static_cast<WPARAM>(done * 1000 / total) : 0, 0);
        UpdateWindow(progress);
    };
    state.result = BatchRename::Execute(state.plan, state.undoFile, onProgress);
    SetCursor(old);
    state.running = false;
    EndDialog(dialog, IDOK);
}

void OnGetDispInfo(DialogState& state, NMLVDISPINFOW* info) {
    if (!(info->item.mask & LVIF_TEXT)) return;
    const auto index = static_cast<std::size_t>(info->item.iItem);
    if (index >= state.plan.size()) return;
    const RenamePlanItem& item = state.plan[index];
    switch (info->item.iSubItem) {
    case 0: state.cell = FileName(item.from); break;
    case 1: state.cell = FileName(item.to); break;
    default: state.cell = state.issueText[static_cast<std::size_t>(item.issue)]; break;
    }
    info->item.pszText = state.cell.data();
}

LRESULT OnCustomDraw(DialogState& state, NMLVCUSTOMDRAW* draw) {
    switch (draw->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        const auto index = static_cast<std::size_t>(draw->nmcd.dwItemSpec);
        if (index < state.plan.size() && state.plan[index].issue != RenameIssue::None) {
            draw->clrText = RGB(200, 0, 0);
        } else if (index < state.plan.size() && !state.plan[index].Changes()) {
            draw->clrText = GetSysColor(COLOR_GRAYTEXT);
        }
        return CDRF_DODEFAULT;
    }
    }
    return CDRF_DODEFAULT;
}

INT_PTR CALLBACK DialogProc(HWND dialog, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        state = reinterpret_cast<DialogState*>(lParam);
        InitDialog(dialog, *state);
        SetFocus(GetDlgItem(dialog, IDC_BR_PATTERN));
        return FALSE;
    case WM_COMMAND:
        if (!state || state->running) return TRUE;
        switch (LOWORD(wParam)) {
        case IDC_BR_PATTERN:
        case IDC_BR_START:
            if (HIWORD(wParam) == EN_CHANGE) SetTimer(dialog, kPreviewTimer, kPreviewDelayMs, nullptr);
            return TRUE;
        case IDC_BR_LOWER_EXT:
            if (HIWORD(wParam) == BN_CLICKED) UpdatePreview(dialog, *state);
            return TRUE;
        case IDOK:
            Execute(dialog, *state);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (wParam == kPreviewTimer && state) {
            KillTimer(dialog, kPreviewTimer);
            UpdatePreview(dialog, *state);
            return TRUE;
        }
        break;
    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (!state || header->idFrom != IDC_BR_LIST) break;
        if (header->code == LVN_GETDISPINFOW) {
            OnGetDispInfo(*state, reinterpret_cast<NMLVDISPINFOW*>(lParam));
            return TRUE;
        }
        if (header->code == NM_CUSTOMDRAW) {
            SetWindowLongPtrW(dialog, DWLP_MSGRESULT, OnCustomDraw(*state, reinterpret_cast<NMLVCUSTOMDRAW*>(lParam)));
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

}  // namespace

HRESULT ShowBatchRenameDialog(HWND owner, HINSTANCE instance, const std::vector<std::wstring>& paths,
                              const std::wstring& undoFile) {
    if (paths.empty()) return S_FALSE;
    DialogState state;
    state.instance = instance;
    state.paths = &paths;
    state.undoFile = undoFile;
    const INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_BATCH_RENAME), owner, &DialogProc,
                                           reinterpret_cast<LPARAM>(&state));
    if (result == -1) return HRESULT_FROM_WIN32(GetLastError());
    return result == IDOK ? state.result : S_FALSE;
}
