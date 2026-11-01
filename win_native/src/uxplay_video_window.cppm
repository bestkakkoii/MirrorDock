// Video window with its own UI thread.
//
// Fullscreen works like a web video player: double-click, F, F11 or
// Alt+Enter toggle at any time, Esc leaves fullscreen.  Fullscreen is a
// borderless window covering the monitor (no display mode change), which
// DXGI flip-model swap chains promote to independent flip: the lowest
// latency path, identical to exclusive fullscreen but instant to toggle.
//
// All public members may be called from any thread; they post messages to
// the UI thread.  Callbacks are invoked on the UI thread and must not block.
export module uxplay_video_window;

import std;
import uxplay_status;

export namespace uxplay
{
    enum class tray_command
    {
        toggle_fullscreen,
        toggle_usb,
        open_log,
        exit
    };

    struct window_callbacks
    {
        std::function<void(int client_width, int client_height)> on_client_resized;
        std::function<void()> on_close_requested;
        std::function<void()> on_usb_device_change;
        std::function<void(bool fullscreen)> on_fullscreen_changed;
        std::function<void(tray_command command)> on_tray_command;
        // a monitor was attached/detached or its mode changed (WM_DISPLAYCHANGE)
        std::function<void()> on_display_changed;
    };

    class video_window
    {
    public:
        // tray_icon: notification-area icon with status and a context menu
        static outcome<std::unique_ptr<video_window>> create(std::wstring title, window_callbacks callbacks,
                                                             bool start_fullscreen, bool tray_icon);

        ~video_window();
        video_window(video_window const &) = delete;
        video_window &operator=(video_window const &) = delete;

        // HWND of the window (valid for the lifetime of this object).
        [[nodiscard]] void *native_handle() const;

        // Shows the window sized for a video of the given dimensions (or
        // adapts the aspect ratio when already visible and not fullscreen).
        void show_for_video(int video_width, int video_height);
        void hide();
        void set_title(std::wstring title);
        void toggle_fullscreen();

        // tray tooltip / menu status line, USB menu check mark, balloon tips
        void set_status(std::wstring status);
        void set_usb_enabled(bool enabled);
        void notify(std::wstring title, std::wstring text);

        [[nodiscard]] bool visible() const;
        [[nodiscard]] bool fullscreen() const;

    private:
        struct state;

        explicit video_window(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
