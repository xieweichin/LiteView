#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <thread>

// Top-level frame: menu, toolbar, status bar, and (from T4 on) the browser
// and viewer panes. Owns the per-mode accelerator tables.
class MainWindow {
public:
    enum class Mode { Browser, Viewer };

    MainWindow() = default;
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // Registers the class and creates the window with its toolbar and status bar.
    HRESULT Create(HINSTANCE instance, int showCommand);
    HWND Hwnd() const { return hwnd_; }

    // Call from the message loop before TranslateMessage/DispatchMessage.
    bool TranslateAccelerator(MSG* msg);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    bool OnCreate();
    void OnSize();
    void OnCommand(UINT id);
    LRESULT OnNotify(NMHDR* header);
    void OnDpiChanged(UINT dpi, const RECT* suggested);
    HRESULT StartSettingsWorker() noexcept;
    void OnSettingsReady(WPARAM token);
    void OnClose();
    void OnSettingsSaved(WPARAM token);

    struct SettingsState;
    static void SettingsWorker(const std::shared_ptr<SettingsState>& state) noexcept;

    void CreateToolbar();
    void CreateStatusBar();
    void UpdateStatusParts();
    std::wstring LoadStr(UINT id) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND toolbar_ = nullptr;
    HWND status_ = nullptr;
    HACCEL accelBrowser_ = nullptr;
    HACCEL accelViewer_ = nullptr;
    Mode mode_ = Mode::Browser;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    bool placementTouched_ = false;
    bool closing_ = false;
    std::shared_ptr<SettingsState> settingsState_;
    std::thread settingsThread_;
};
