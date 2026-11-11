// SPDX-License-Identifier: MIT
// Video window with its own UI thread.
//
// Fullscreen works like a web video player: double-click, F, F11 or
// Alt+Enter toggle at any time, Esc leaves fullscreen.  Fullscreen is a
// borderless window covering the monitor (no display mode change), which
// DXGI flip-model swap chains promote to independent flip: the lowest
// latency path, identical to exclusive fullscreen but instant to toggle.
// A player-style control bar (uxplay_video_overlay) appears when the
// pointer moves over the video and hides after a few idle seconds, in
// windowed and fullscreen mode alike.  A right click anywhere on the picture
// opens the menu of the notification-area icon.
//
// Size: a portrait picture uses most of the monitor height, a landscape one
// about half its width; the window keeps the picture's aspect ratio while it
// is resized, and the size chosen by hand (per orientation), the position and
// "always on top" are remembered between runs (uxplay_settings).  The
// "picture" is the part that is shown: with cropping the window hugs the
// cropped content.
//
// Framing a crop by hand (tray_command::crop_select, key X): the pointer
// becomes a cross, a drag draws a frame (thin always-on-top edge windows, so
// the video swap chain is not touched) and releasing the button reports the
// framed part; Esc or the right button cancels.
//
// All public members may be called from any thread; they post messages to
// the UI thread.  Callbacks are invoked on the UI thread and must not block.
export module uxplay_video_window;

import std;
import uxplay_status;
import uxplay_media_types;

export namespace uxplay
{
    enum class tray_command
    {
        toggle_fullscreen,
        toggle_always_on_top, // handled by the window itself
        apply_data_guard,
        enable_link_adapter, // re-enable a disabled "Apple Mobile Device Ethernet"
        prefer_usb_link,     // AirPlay over the USB cable when it is up (default)
        wifi_only,           // never use the USB link for AirPlay (cable only charges)
        toggle_mute,         // UxPlay's own output (independent of the iPhone volume)
        mode_quality,
        mode_balanced,
        mode_performance,
        crop_off,       // show the whole picture
        crop_automatic, // cut black bars off automatically
        crop_select,    // frame the part to show with the mouse; handled by the window itself
        toggle_autostart, // start with Windows
        open_log,
        about,
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
        // the program was started again while this instance runs
        std::function<void()> on_second_launch;
        // Windows is shutting down or the user signs out: the handler may
        // block briefly (bounded) to say goodbye on the network
        std::function<void()> on_session_ending;
        // the user framed the part of the picture to show, as fractions of
        // the picture as it is shown now (tray_command::crop_select)
        std::function<void(fraction_region selection)> on_crop_selected;
    };

    // name of the window message a second start posts to the running instance
    inline constexpr wchar_t second_launch_message_name[] = L"uxplay_native_second_launch";
    inline constexpr wchar_t video_window_class_name[] = L"uxplay_native_video_window";

    // Identifies the instance a second start addresses (wParam of the message):
    // instances with different receiver names ("-n") ignore each other.
    [[nodiscard]] std::uintptr_t second_launch_tag(std::wstring_view receiver_name);

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

        // Switches every callback off, on the UI thread: when this returns
        // none will be called again, and none is running except one that sits
        // in a modal loop of its own (a message box): such a callback must use
        // nothing of its owner after that loop.  For the owner, before it
        // destroys what the callbacks use.  Not from a callback.
        void detach_callbacks();

        // Shows the window sized for a video of the given dimensions (or
        // adapts the aspect ratio when already visible and not fullscreen).
        void show_for_video(int video_width, int video_height);
        void hide();
        void set_title(std::wstring title);
        void toggle_fullscreen();

        // tray tooltip / menu status line, balloon tips
        void set_status(std::wstring status);
        // menu line describing the iPhone USB link; offer_data_guard adds the
        // "keep PC internet off the iPhone's cellular data" item,
        // offer_enable_adapter the "re-enable the Apple USB network adapter" item
        void set_usb_link(std::wstring text, bool offer_data_guard, bool offer_enable_adapter);
        // which link AirPlay uses: USB first (true) or Wi-Fi only (false)
        void set_link_preference(bool prefer_usb_link);
        // control bar: link of the shown stream ("USB"/"Wi-Fi", empty when
        // idle) and its size; local mute state
        void set_stream_info(std::wstring transport, std::wstring detail);
        void set_muted(bool muted);
        // menu state: the active mode, "start with Windows", whether a log
        // file exists to open
        void set_performance_mode(performance_mode mode);
        void set_crop_mode(crop_mode mode);
        void set_autostart(bool enabled);
        void set_log_available(bool available);
        // false while no new picture arrives: a kept last frame does not keep
        // the display awake
        void set_stream_active(bool active);
        // on_click: command run when the user clicks the balloon
        void notify(std::wstring title, std::wstring text, std::optional<tray_command> on_click = std::nullopt);

        [[nodiscard]] bool visible() const;
        [[nodiscard]] bool fullscreen() const;

    private:
        struct state;

        explicit video_window(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
