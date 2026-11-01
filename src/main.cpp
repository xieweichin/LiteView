#include <windows.h>
#include <commctrl.h>
#include <objbase.h>

#include <string>

#include "res/resource.h"
#include "ui/MainWindow.h"

namespace {

std::wstring LoadStr(HINSTANCE instance, UINT id) {
    const wchar_t* text = nullptr;
    const int len = LoadStringW(instance, id, reinterpret_cast<LPWSTR>(&text), 0);
    return (len > 0 && text) ? std::wstring(text, static_cast<size_t>(len)) : std::wstring();
}

// Shows "LiteView 無法啟動（錯誤碼 0x...）" with the system's description.
void ReportStartupError(HINSTANCE instance, HRESULT hr) {
    wchar_t code[16] = {};
    wsprintfW(code, L"0x%08X", static_cast<unsigned>(hr));
    std::wstring text = LoadStr(instance, IDS_ERR_STARTUP);
    const size_t pos = text.find(L"%s");
    if (pos != std::wstring::npos) text.replace(pos, 2, code);

    wchar_t* sys = nullptr;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&sys), 0,
                       nullptr) &&
        sys) {
        text += L"\n\n";
        text += sys;
    }
    LocalFree(sys);
    MessageBoxW(nullptr, text.c_str(), LoadStr(instance, IDS_APP_TITLE).c_str(),
                MB_OK | MB_ICONERROR);
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        ReportStartupError(instance, hr);
        return 1;
    }

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES |
                                                 ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    int exitCode = 0;
    {
        MainWindow window;
        hr = window.Create(instance, showCommand);
        if (FAILED(hr)) {
            ReportStartupError(instance, hr);
            CoUninitialize();
            return 1;
        }

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (window.TranslateAccelerator(&msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exitCode = static_cast<int>(msg.wParam);
    }

    CoUninitialize();
    return exitCode;
}
