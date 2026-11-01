#pragma once

#include <windows.h>

// Bottom-left preview area. T4 provides an empty framed pane; T5 draws the
// selected picture's thumbnail into it.
class PreviewPane {
public:
    PreviewPane() = default;
    PreviewPane(const PreviewPane&) = delete;
    PreviewPane& operator=(const PreviewPane&) = delete;

    HRESULT Create(HWND parent, HINSTANCE instance);
    HWND Hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void OnPaint();

    HWND hwnd_ = nullptr;
};
