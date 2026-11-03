// SPDX-License-Identifier: MIT
// Player-style control bar over the video window.
//
// A layered, non-activating popup owned by the video window and drawn with
// Direct2D/DirectWrite.  It never touches the video swap chain: while it is
// hidden the window keeps its lowest-latency presentation path (independent
// flip in fullscreen); while it is visible DWM composes it over the video.
// Like a media player it fades in when the pointer moves over the video and
// fades out after a few idle seconds (it stays while the pointer is on it).
//
// UI-thread only: create it on the video window's thread and call every
// member from there.  Commands are delivered on that thread.
export module uxplay_video_overlay;

import std;
import uxplay_status;

export namespace uxplay
{
    enum class overlay_command
    {
        toggle_fullscreen,
        prefer_usb_link,
        wifi_only,
        toggle_mute,
        apply_data_guard,
        enable_link_adapter,
        open_menu
    };

    // what the bar shows
    struct overlay_model
    {
        std::wstring transport; // "USB" / "Wi-Fi"; empty while waiting
        std::wstring detail;    // e.g. "664 × 1440"
        bool prefer_usb_link = true;
        bool fullscreen = false;
        bool muted = false;
        bool offer_data_guard = false;
        bool offer_enable_adapter = false;

        bool operator==(overlay_model const &other) const = default;
    };

    class video_overlay
    {
    public:
        // owner_window: HWND of the video window
        static outcome<std::unique_ptr<video_overlay>> create(void *owner_window,
                                                              std::function<void(overlay_command command)> on_command);

        ~video_overlay();
        video_overlay(video_overlay const &) = delete;
        video_overlay &operator=(video_overlay const &) = delete;

        void set_model(overlay_model model);

        // the pointer moved over the video: show the bar, restart the idle timer
        void activity();

        // the owner moved, resized, changed DPI, was shown or hidden: follow it
        void owner_changed();

        // hide at once (owner hidden or minimized)
        void hide();

    private:
        struct state;

        explicit video_overlay(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
