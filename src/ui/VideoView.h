#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <functional>
#include <string>

struct IMFPMediaPlayer;
struct MFP_EVENT_HEADER;

// Viewer-mode page for videos, played by Media Foundation (MFPlay) with the
// codecs Windows has. A thin bar at the bottom shows play/pause, progress and
// time. Space plays or pauses, Left/Right seek 5 seconds, a click on the
// picture plays or pauses, a click on the bar seeks.
class VideoView {
public:
    enum class State { Empty, Loading, Playing, Paused, Ended, NoCodec, Failed };

    struct Callbacks {
        std::function<void()> changed;         // state or position changed
        std::function<void(int step)> navigate;  // mouse wheel
        std::function<void()> exit;            // double-click
    };

    VideoView();
    ~VideoView();
    VideoView(const VideoView&) = delete;
    VideoView& operator=(const VideoView&) = delete;

    HRESULT Create(HWND parent, HINSTANCE instance, Callbacks callbacks);
    HWND Hwnd() const { return hwnd_; }
    bool Owns(HWND hwnd) const { return hwnd && (hwnd == hwnd_ || hwnd == surface_); }

    void Show(const std::wstring& path);
    void Clear();
    void SetFullscreen(bool fullscreen);
    void OnThemeChanged();

    void TogglePlay();
    void Seek(int seconds);

    State GetState() const { return state_; }
    long long Position() const;  // 100-ns units
    long long Duration() const { return duration_; }

private:
    class Callback;
    friend class Callback;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SurfaceProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void OnPlayerEvent(MFP_EVENT_HEADER* header);
    void SetState(State state);
    void SeekTo(long long position);
    void Layout();
    void PaintBar(HDC dc);
    int BarHeight() const;
    RECT ButtonRect() const;
    RECT TrackRect() const;

    HWND hwnd_ = nullptr;
    HWND surface_ = nullptr;  // MFPlay draws the video here
    Callbacks callbacks_;
    Microsoft::WRL::ComPtr<IMFPMediaPlayer> player_;
    Microsoft::WRL::ComPtr<Callback> callback_;
    std::wstring path_;
    State state_ = State::Empty;
    long long duration_ = 0;
    bool hasVideo_ = false;
    bool fullscreen_ = false;
    int wheelRemainder_ = 0;
};
