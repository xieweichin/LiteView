#include "ui/PropertiesDialog.h"

#include <commctrl.h>
#include <shlwapi.h>

#include <cmath>
#include <cwchar>
#include <utility>
#include <vector>

#include "core/Metadata.h"
#include "res/resource.h"
#include "ui/Theme.h"

namespace {

std::wstring LoadStr(HINSTANCE instance, UINT id) {
    const wchar_t* text = nullptr;
    const int len = LoadStringW(instance, id, reinterpret_cast<LPWSTR>(&text), 0);
    return (len > 0 && text) ? std::wstring(text, static_cast<size_t>(len)) : std::wstring();
}

std::wstring FormatNumber(double value, int decimals) {
    wchar_t text[32] = {};
    swprintf(text, 32, L"%.*f", decimals, value);
    std::wstring result = text;
    if (result.find(L'.') != std::wstring::npos) {
        while (!result.empty() && result.back() == L'0') result.pop_back();
        if (!result.empty() && result.back() == L'.') result.pop_back();
    }
    return result;
}

std::wstring FormatTime(const FILETIME& utc) {
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return {};
    wchar_t date[64] = {}, time[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &st, nullptr, date, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, nullptr, time, 64);
    return std::wstring(date) + L" " + time;
}

// EXIF dates look like "2024:05:17 08:30:00".
std::wstring FormatExifDate(const std::wstring& exif) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (swscanf(exif.c_str(), L"%d:%d:%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3) return exif;
    SYSTEMTIME st{static_cast<WORD>(y), static_cast<WORD>(mo), 0, static_cast<WORD>(d),
                  static_cast<WORD>(h), static_cast<WORD>(mi), static_cast<WORD>(s), 0};
    wchar_t date[64] = {}, time[64] = {};
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &st, nullptr, date, 64, nullptr)) return exif;
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, nullptr, time, 64);
    return std::wstring(date) + L" " + time;
}

struct DialogData {
    HINSTANCE instance;
    std::wstring path;
    std::vector<std::pair<std::wstring, std::wstring>> rows;
};

void Collect(DialogData& data) {
    HINSTANCE inst = data.instance;
    const std::wstring& path = data.path;
    const auto slash = path.find_last_of(L'\\');
    data.rows.emplace_back(LoadStr(inst, IDS_PROP_NAME), slash == std::wstring::npos ? path : path.substr(slash + 1));
    data.rows.emplace_back(LoadStr(inst, IDS_PROP_FOLDER), slash == std::wstring::npos ? L"" : path.substr(0, slash));

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        const ULONGLONG size = (static_cast<ULONGLONG>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
        wchar_t text[64] = {};
        StrFormatByteSizeW(static_cast<LONGLONG>(size), text, 64);
        data.rows.emplace_back(LoadStr(inst, IDS_PROP_SIZE), std::wstring(text) + L" (" + std::to_wstring(size) + L" bytes)");
        data.rows.emplace_back(LoadStr(inst, IDS_PROP_MODIFIED), FormatTime(attributes.ftLastWriteTime));
    }

    PhotoMetadata meta;
    if (SUCCEEDED(Metadata().Read(path, meta))) {
        // Show the upright size, as the viewer does.
        const bool sideways = meta.orientation && *meta.orientation >= 5 && *meta.orientation <= 8;
        const UINT w = sideways ? meta.height : meta.width, h = sideways ? meta.width : meta.height;
        data.rows.emplace_back(LoadStr(inst, IDS_PROP_DIMENSIONS), std::to_wstring(w) + L" × " + std::to_wstring(h));
        if (!meta.dateTaken.empty()) data.rows.emplace_back(LoadStr(inst, IDS_PROP_TAKEN), FormatExifDate(meta.dateTaken));
        std::wstring camera = meta.cameraMake;
        if (!meta.cameraModel.empty()) {
            // Many models already start with the make ("Canon Canon EOS R6").
            if (!camera.empty() && meta.cameraModel.rfind(camera, 0) != 0) camera += L" ";
            else camera.clear();
            camera += meta.cameraModel;
        }
        if (!camera.empty()) data.rows.emplace_back(LoadStr(inst, IDS_PROP_CAMERA), camera);
        if (meta.exposureSeconds && *meta.exposureSeconds > 0) {
            const double t = *meta.exposureSeconds;
            const std::wstring text = t < 1.0 ? L"1/" + FormatNumber(std::round(1.0 / t), 0) + L" s"
                                              : FormatNumber(t, 1) + L" s";
            data.rows.emplace_back(LoadStr(inst, IDS_PROP_EXPOSURE), text);
        }
        if (meta.fNumber) data.rows.emplace_back(LoadStr(inst, IDS_PROP_APERTURE), L"f/" + FormatNumber(*meta.fNumber, 1));
        if (meta.iso) data.rows.emplace_back(LoadStr(inst, IDS_PROP_ISO), std::to_wstring(*meta.iso));
        if (meta.focalLengthMm) {
            data.rows.emplace_back(LoadStr(inst, IDS_PROP_FOCAL), FormatNumber(*meta.focalLengthMm, 1) + L" mm");
        }
    }
}

INT_PTR CALLBACK DialogProc(HWND dialog, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        auto* data = reinterpret_cast<DialogData*>(lParam);
        const auto slash = data->path.find_last_of(L'\\');
        std::wstring title = LoadStr(data->instance, IDS_PROP_TITLE);
        const auto at = title.find(L"%s");
        if (at != std::wstring::npos) title.replace(at, 2, slash == std::wstring::npos ? data->path : data->path.substr(slash + 1));
        SetWindowTextW(dialog, title.c_str());

        HWND list = GetDlgItem(dialog, IDC_PROPERTIES_LIST);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER);
        RECT rc{};
        GetClientRect(list, &rc);
        const int first = rc.right * 3 / 10;
        std::wstring field = LoadStr(data->instance, IDS_PROP_FIELD), value = LoadStr(data->instance, IDS_PROP_VALUE);
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = first;
        col.pszText = field.data();
        ListView_InsertColumn(list, 0, &col);
        col.cx = rc.right - first - GetSystemMetrics(SM_CXVSCROLL);
        col.pszText = value.data();
        ListView_InsertColumn(list, 1, &col);
        int row = 0;
        for (auto& entry : data->rows) {
            LVITEMW item = {};
            item.mask = LVIF_TEXT;
            item.iItem = row;
            item.pszText = entry.first.data();
            ListView_InsertItem(list, &item);
            ListView_SetItemText(list, row, 1, entry.second.data());
            ++row;
        }
        theme::ApplyDialog(dialog);
        return TRUE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT: {
        LRESULT brush = 0;
        if (theme::HandleCtlColor(msg, wParam, lParam, brush)) return static_cast<INT_PTR>(brush);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, LOWORD(wParam));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

}  // namespace

void ShowPropertiesDialog(HWND owner, HINSTANCE instance, const std::wstring& path) {
    DialogData data{instance, path, {}};
    Collect(data);
    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_PROPERTIES), owner, &DialogProc, reinterpret_cast<LPARAM>(&data));
}
