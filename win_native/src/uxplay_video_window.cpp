// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbt.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <imm.h>

#include "uxplay_version.h"

module uxplay_video_window;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_media_types;
import uxplay_settings;
import uxplay_ui_text;
import uxplay_video_overlay;
import uxplay_remote_control;

namespace uxplay
{
    namespace
    {
        constexpr wchar_t const *window_class_name = video_window_class_name;
        constexpr UINT message_show = WM_APP + 1;
        constexpr UINT message_hide = WM_APP + 2;
        constexpr UINT message_title = WM_APP + 3;
        constexpr UINT message_toggle_fullscreen = WM_APP + 4;
        constexpr UINT message_quit = WM_APP + 5;
        constexpr UINT message_tray_update = WM_APP + 6;
        constexpr UINT message_tray_notify = WM_APP + 7;
        constexpr UINT message_tray_callback = WM_APP + 10;
        constexpr UINT message_overlay_update = WM_APP + 11;
        constexpr UINT tray_icon_id = 1;
        constexpr UINT_PTR cursor_timer_id = 1;
        // same idle time as the control bar (media-player behaviour)
        constexpr UINT cursor_hide_delay_ms = 3000;
        constexpr UINT message_keep_awake = WM_APP + 12;
        constexpr UINT message_detach = WM_APP + 13;
        constexpr UINT message_remote_control = WM_APP + 14;
        // answer to the second-launch message of an instance with our name
        constexpr LRESULT second_launch_answer = 1;

        // Default window size by orientation, as a fraction of the monitor
        // work area: a portrait phone uses most of the height, a landscape
        // one about half the width (a player window, not a maximized one).
        // A size chosen by hand is remembered per orientation.
        constexpr double portrait_height_fraction = 0.9;
        constexpr double landscape_width_fraction = 0.5;
        // limits for a remembered size, and for either edge of any placement
        constexpr double smallest_fraction = 0.15;
        constexpr double largest_fraction = 0.95;
        // a landscape window is not made narrower than this on a small or
        // scaled monitor, and no client edge shorter than this (96-DPI pixels)
        constexpr double landscape_minimum_width = 960.0;
        constexpr int minimum_client_edge = 160;

        // Crop frame drawn by hand: two rings of thin edge windows owned by
        // the video window, a light one inside a dark one, so that the frame
        // shows on any picture.
        constexpr wchar_t const *frame_light_class_name = L"uxplay_native_frame_light";
        constexpr wchar_t const *frame_dark_class_name = L"uxplay_native_frame_dark";
        constexpr int frame_light_thickness = 2; // 96-DPI pixels
        constexpr int frame_dark_thickness = 1;
        // a frame smaller than this (96-DPI pixels), or than this share of the
        // picture, is a click and not a selection
        constexpr int smallest_selection_edge = 24;
        constexpr double smallest_selection_fraction = 0.05;

        // per-user settings (uxplay_settings)
        constexpr std::wstring_view setting_landscape_size = L"window_landscape_permille";
        constexpr std::wstring_view setting_portrait_size = L"window_portrait_permille";
        constexpr std::wstring_view setting_center_x = L"window_center_x";
        constexpr std::wstring_view setting_center_y = L"window_center_y";
        constexpr std::wstring_view setting_always_on_top = L"always_on_top";

        enum menu_identifier : UINT
        {
            menu_fullscreen = 100,
            menu_always_on_top,
            menu_data_guard,
            menu_enable_adapter,
            menu_prefer_usb,
            menu_wifi_only,
            menu_mute,
            menu_mode_quality,
            menu_mode_balanced,
            menu_mode_performance,
            menu_crop_off,
            menu_crop_automatic,
            menu_crop_select,
            menu_remote_control,
            menu_autostart,
            menu_open_log,
            menu_check_for_updates,
            menu_about,
            menu_exit
        };

        RECT monitor_rect(HMONITOR monitor, bool work_area)
        {
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            GetMonitorInfoW(monitor, &info);
            return work_area ? info.rcWork : info.rcMonitor;
        }

        RECT monitor_rect(HWND window, bool work_area)
        {
            return monitor_rect(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), work_area);
        }

        using settings::store_number;
        using settings::stored_number;

        // Dark caption and border, like a media player (Windows 10 20H1+ /
        // Windows 11); older systems keep the default frame.
        void use_dark_frame(HWND window)
        {
            BOOL const dark = TRUE;
            constexpr DWORD immersive_dark_mode = 20;         // DWMWA_USE_IMMERSIVE_DARK_MODE
            constexpr DWORD immersive_dark_mode_pre_20h1 = 19; // same attribute before Windows 10 20H1
            if (FAILED(DwmSetWindowAttribute(window, immersive_dark_mode, &dark, sizeof(dark))))
            {
                DwmSetWindowAttribute(window, immersive_dark_mode_pre_20h1, &dark, sizeof(dark));
            }
            COLORREF const black = RGB(0, 0, 0);
            constexpr DWORD border_color = 34;  // DWMWA_BORDER_COLOR (Windows 11)
            constexpr DWORD caption_color = 35; // DWMWA_CAPTION_COLOR (Windows 11)
            DwmSetWindowAttribute(window, caption_color, &black, sizeof(black));
            DwmSetWindowAttribute(window, border_color, &black, sizeof(black));
        }

        template <std::size_t size>
        void copy_text(wchar_t (&destination)[size], std::wstring_view text)
        {
            std::size_t const count = std::min(text.size(), size - 1);
            std::copy_n(text.data(), count, destination);
            destination[count] = L'\0';
        }

        // Brings a window to the front even when another process owns the
        // foreground (mirroring starts from the phone, not from a click here).
        void bring_to_front(HWND window)
        {
            SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            HWND const foreground = GetForegroundWindow();
            DWORD const foreground_thread = foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
            DWORD const own_thread = GetCurrentThreadId();
            bool const attached = foreground_thread != 0 && foreground_thread != own_thread &&
                                  AttachThreadInput(foreground_thread, own_thread, TRUE) != 0;
            SetForegroundWindow(window);
            SetActiveWindow(window);
            if (attached)
            {
                AttachThreadInput(foreground_thread, own_thread, FALSE);
            }
        }
    }

    struct video_window::state
    {
        window_callbacks callbacks;
        std::wstring pending_title;
        std::wstring status_text = L"" UXPLAY_PRODUCT_NAME;
        std::wstring notify_title;
        std::wstring notify_text;
        std::optional<tray_command> notify_action;
        std::optional<tray_command> balloon_action; // UI thread only
        std::wstring usb_link_text{ui_string(ui_text::usb_checking)};
        std::mutex text_mutex;
        HWND window = nullptr;
        HDEVNOTIFY device_notification = nullptr;
        std::thread ui_thread;
        std::atomic<bool> is_visible{false};
        std::atomic<bool> is_fullscreen{false};
        std::atomic<bool> data_guard_offer{false};
        std::atomic<bool> enable_adapter_offer{false};
        std::atomic<bool> prefer_usb_link{true};
        std::atomic<bool> muted{false};
        std::atomic<bool> always_on_top{false};
        std::atomic<bool> autostart{false};
        std::atomic<bool> log_available{false};
        std::atomic<bool> stream_active{false};
        std::atomic<performance_mode> mode{performance_mode::balanced};
        std::atomic<crop_mode> crop{crop_mode::off};
        // remote control: chosen in the menu / input goes to the phone
        std::atomic<bool> control_wanted{false};
        std::atomic<bool> control_ready{false};
        std::wstring stream_transport; // guarded by text_mutex
        std::wstring stream_detail;    // guarded by text_mutex
        std::unique_ptr<video_overlay> overlay; // UI thread only
        POINT last_pointer{LONG_MIN, LONG_MIN};
        bool fullscreen_wanted = false; // "-fs", then whatever the user last chose
        bool shown_once = false;
        bool cursor_hidden = false;
        bool tray_wanted = false;
        bool tray_added = false;
        UINT taskbar_created_message = 0;
        UINT second_launch_message = 0;
        std::uintptr_t instance_tag = 0; // second_launch_tag of this receiver's name
        int video_width = 0;
        int video_height = 0;
        WINDOWPLACEMENT saved_placement{};
        LONG_PTR saved_style = 0;
        // UI thread only: window size and position memory
        std::optional<double> landscape_fraction; // size chosen by hand, of the work-area width
        std::optional<double> portrait_fraction;  // of the work-area height
        std::optional<POINT> remembered_center;   // from the previous run, used for the first show
        bool placement_pending = false;           // the aspect changed while fullscreen or maximized
        bool placing = false;                     // place_for_video is moving the window
        bool resized_by_user = false;             // a WM_SIZING drag is in progress
        // UI thread only: framing a crop by hand
        bool framing = false;      // armed: the next drag frames the part to show
        bool framing_drag = false; // the button is down
        POINT frame_anchor{};      // client coordinates
        POINT frame_corner{};
        std::array<HWND, 8> frame_edges{}; // light ring, dark ring; created on first use
        bool frame_failure_logged = false;
        bool detached = false; // UI thread only: the owner wants no callback any more
        // the right button just cancelled a frame: its release opens no menu
        bool right_click_consumed = false;
        // UI thread only: remote control
        bool pointer_pressed = false;           // the left button is a finger on the phone
        std::pair<double, double> pointer_position{}; // where it was last, as fractions of the picture
        std::vector<std::uint8_t> held_keys;    // HID usages down on the phone
        bool input_method_detached = false;     // the IME of this PC is kept out while keys go to the phone
        // the Esc press that is down now was the window's (it left full screen
        // or cancelled a frame): its auto-repeat and its release are not keys
        // for the phone
        bool escape_taken = false;

        [[nodiscard]] bool controlling() const;
        [[nodiscard]] std::optional<std::pair<double, double>> picture_fraction(POINT client, bool clamp) const;
        void apply_remote_control();
        void release_remote_input();
        [[nodiscard]] bool control_key(bool pressed, WPARAM key, LPARAM details);

        [[nodiscard]] RECT picture_rectangle() const;
        void begin_framing();
        void end_framing();
        void update_frame();
        void hide_frame();
        void finish_framing();
        void enter_fullscreen();
        void leave_fullscreen();
        void set_fullscreen(bool enable);
        void place_for_video(bool keep_center);
        void constrain_to_video_aspect(WPARAM edge, RECT &outer) const;
        void remember_placement(bool size_changed);
        void load_window_memory();
        void apply_always_on_top();
        void apply_keep_awake();
        void show_cursor_again();
        void add_tray_icon();
        void update_tray_icon();
        void remove_tray_icon();
        void show_tray_menu();
        void tray(tray_command command);
        void refresh_overlay();
        void on_overlay_command(overlay_command command);
        LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);

        static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    };

    LRESULT CALLBACK video_window::state::procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE)
        {
            auto const *creation = reinterpret_cast<CREATESTRUCTW const *>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        auto *owner = reinterpret_cast<state *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (owner == nullptr || owner->window == nullptr)
        {
            return DefWindowProcW(window, message, wparam, lparam);
        }
        return owner->handle(message, wparam, lparam);
    }

    void video_window::state::enter_fullscreen()
    {
        if (is_fullscreen.load())
        {
            return;
        }
        saved_placement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(window, &saved_placement);
        saved_style = GetWindowLongPtrW(window, GWL_STYLE);
        RECT const monitor = monitor_rect(window, false);
        SetWindowLongPtrW(window, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(window, HWND_TOP, monitor.left, monitor.top, monitor.right - monitor.left,
                     monitor.bottom - monitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        is_fullscreen.store(true);
        SetTimer(window, cursor_timer_id, cursor_hide_delay_ms, nullptr);
        refresh_overlay();
        if (overlay)
        {
            overlay->owner_changed();
        }
        if (!detached && callbacks.on_fullscreen_changed)
        {
            callbacks.on_fullscreen_changed(true);
        }
    }

    void video_window::state::leave_fullscreen()
    {
        if (!is_fullscreen.load())
        {
            return;
        }
        KillTimer(window, cursor_timer_id);
        show_cursor_again();
        SetWindowLongPtrW(window, GWL_STYLE, saved_style);
        SetWindowPlacement(window, &saved_placement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        is_fullscreen.store(false);
        apply_always_on_top();
        if (placement_pending && !IsZoomed(window))
        {
            // the phone rotated while fullscreen: the restored window still
            // has the old orientation's shape
            place_for_video(true);
        }
        refresh_overlay();
        if (overlay)
        {
            overlay->owner_changed();
        }
        if (!detached && callbacks.on_fullscreen_changed)
        {
            callbacks.on_fullscreen_changed(false);
        }
    }

    void video_window::state::set_fullscreen(bool enable)
    {
        end_framing(); // a frame being drawn belongs to the old window shape
        fullscreen_wanted = enable;
        if (enable)
        {
            enter_fullscreen();
        }
        else
        {
            leave_fullscreen();
        }
    }

    void video_window::state::show_cursor_again()
    {
        if (cursor_hidden)
        {
            cursor_hidden = false;
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        }
    }

    // The part of the client area the picture occupies: the presenter shows
    // it centred with its aspect ratio (bars only when the window has another
    // shape: fullscreen, maximized).
    RECT video_window::state::picture_rectangle() const
    {
        RECT client{};
        GetClientRect(window, &client);
        if (video_width <= 0 || video_height <= 0 || client.right <= 0 || client.bottom <= 0)
        {
            return client;
        }
        double const scale = std::min(static_cast<double>(client.right) / video_width,
                                      static_cast<double>(client.bottom) / video_height);
        auto const width = static_cast<LONG>(std::lround(video_width * scale));
        auto const height = static_cast<LONG>(std::lround(video_height * scale));
        RECT picture{};
        picture.left = (client.right - width) / 2;
        picture.top = (client.bottom - height) / 2;
        picture.right = picture.left + width;
        picture.bottom = picture.top + height;
        return picture;
    }

    // Input on the picture goes to the phone.
    bool video_window::state::controlling() const
    {
        return control_ready.load() && !framing && is_visible.load();
    }

    // A client position as fractions of the picture.  Outside the picture (the
    // bars of a window with another shape) it is nothing, or the nearest
    // position on it when `clamp` is set (a drag that left the picture).
    std::optional<std::pair<double, double>> video_window::state::picture_fraction(POINT client, bool clamp) const
    {
        RECT const picture = picture_rectangle();
        LONG const width = picture.right - picture.left;
        LONG const height = picture.bottom - picture.top;
        if (width <= 0 || height <= 0)
        {
            return std::nullopt;
        }
        if (!clamp && (client.x < picture.left || client.x >= picture.right || client.y < picture.top ||
                       client.y >= picture.bottom))
        {
            return std::nullopt;
        }
        // the middle of the pixel under the pointer
        return std::pair{std::clamp((client.x - picture.left + 0.5) / width, 0.0, 1.0),
                         std::clamp((client.y - picture.top + 0.5) / height, 0.0, 1.0)};
    }

    // Nothing stays pressed on the phone: the finger lifts, the keys are released.
    void video_window::state::release_remote_input()
    {
        if (std::exchange(pointer_pressed, false))
        {
            if (GetCapture() == window)
            {
                ReleaseCapture();
            }
            if (!detached && callbacks.on_pointer)
            {
                callbacks.on_pointer(pointer_action::release, pointer_position.first, pointer_position.second);
            }
        }
        if (!held_keys.empty())
        {
            held_keys.clear();
            if (!detached && callbacks.on_keys)
            {
                callbacks.on_keys(held_keys);
            }
        }
    }

    // Remote control became ready or ended (UI thread).
    void video_window::state::apply_remote_control()
    {
        bool const ready = control_ready.load();
        if (!ready)
        {
            release_remote_input();
        }
        else if (overlay)
        {
            overlay->hide(); // it would cover the bottom of the phone's screen
        }
        // Keys go to the phone as keys: the input method of this PC must not
        // compose text from them as well (the phone has its own).
        if (ready != input_method_detached)
        {
            ImmAssociateContextEx(window, nullptr, ready ? 0 : IACE_DEFAULT);
            input_method_detached = ready;
        }
    }

    // A key while the phone is controlled.  True: it went to the phone (or is
    // one a phone has no use for) and the window does nothing with it.
    bool video_window::state::control_key(bool pressed, WPARAM key, LPARAM details)
    {
        auto const scan_code = static_cast<unsigned>((details >> 16) & 0xFF);
        bool const extended = (details & (1 << 24)) != 0;
        std::optional<std::uint8_t> usage = keyboard_usage(scan_code, extended);
        // Ctrl acts as Command: Ctrl+C, Ctrl+V and the like do on the phone
        // what they do on this PC
        if (usage && *usage == 0xE0)
        {
            usage = std::uint8_t{0xE3};
        }
        else if (usage && *usage == 0xE4)
        {
            usage = std::uint8_t{0xE7};
        }
        auto const send_held = [this]
        {
            if (detached || !callbacks.on_keys)
            {
                return;
            }
            // Control + Space is the phone's "next input source": while Space
            // is down, Ctrl is sent as Control and not as Command
            if (std::ranges::contains(held_keys, std::uint8_t{0x2C}))
            {
                std::vector<std::uint8_t> sent = held_keys;
                std::ranges::replace(sent, std::uint8_t{0xE3}, std::uint8_t{0xE0});
                std::ranges::replace(sent, std::uint8_t{0xE7}, std::uint8_t{0xE4});
                callbacks.on_keys(sent);
            }
            else
            {
                callbacks.on_keys(held_keys);
            }
        };
        // A key that is down on the phone is released there, whatever the
        // window has become since it went down (full screen, say): no key
        // stays held on the phone because its release turned into a window key.
        if (!pressed && usage)
        {
            if (auto const held = std::ranges::find(held_keys, *usage); held != held_keys.end())
            {
                held_keys.erase(held);
                send_held();
                return true;
            }
        }
        bool const alt_down = (details & (1 << 29)) != 0;
        if (key == VK_F8 || key == VK_F11 || (key == VK_ESCAPE && is_fullscreen.load()) ||
            (alt_down && (key == VK_RETURN || key == VK_F4 || key == VK_SPACE)))
        {
            return false; // the window's own keys
        }
        // Page Up / Page Down and Alt + arrow flick the screen instead of
        // being sent as keys (few apps on a phone act on those keys)
        std::optional<swipe_action> flick;
        if (key == VK_NEXT || (alt_down && key == VK_DOWN))
        {
            flick = swipe_action::next_below;
        }
        else if (key == VK_PRIOR || (alt_down && key == VK_UP))
        {
            flick = swipe_action::previous_above;
        }
        else if (alt_down && key == VK_RIGHT)
        {
            flick = swipe_action::next_right;
        }
        else if (alt_down && key == VK_LEFT)
        {
            flick = swipe_action::previous_left;
        }
        if (flick)
        {
            bool const repeated = (details & (1 << 30)) != 0;
            if (pressed && !repeated && !detached && callbacks.on_swipe)
            {
                POINT pointer{};
                GetCursorPos(&pointer);
                ScreenToClient(window, &pointer);
                std::pair<double, double> const around =
                    picture_fraction(pointer, false).value_or(std::pair{0.5, 0.5});
                callbacks.on_swipe(*flick, around.first, around.second);
            }
            return true;
        }
        if (!usage || !pressed)
        {
            return true; // no key of a phone, or the release of one that never went there
        }
        if (std::ranges::contains(held_keys, *usage))
        {
            return true; // the keyboard's auto-repeat: the phone repeats by itself
        }
        held_keys.push_back(*usage);
        send_held();
        return true;
    }

    // Arms the hand-drawn crop: the next drag on the picture frames the part
    // to show.  The window title carries the hint while it is armed.
    void video_window::state::begin_framing()
    {
        if (framing || !is_visible.load() || video_width <= 0 || video_height <= 0 || IsIconic(window))
        {
            return;
        }
        release_remote_input(); // the mouse frames from here on
        framing = true;
        framing_drag = false;
        bring_to_front(window);
        apply_always_on_top();
        if (overlay)
        {
            overlay->hide(); // the control bar would take the clicks at the bottom
        }
        show_cursor_again();
        KillTimer(window, cursor_timer_id);
        SetWindowTextW(window, std::wstring{ui_string(ui_text::crop_select_hint)}.c_str());
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
    }

    void video_window::state::hide_frame()
    {
        for (HWND const edge : frame_edges)
        {
            if (edge != nullptr)
            {
                ShowWindow(edge, SW_HIDE);
            }
        }
    }

    // Leaves framing without a selection (also called when there is none).
    void video_window::state::end_framing()
    {
        if (!framing)
        {
            return;
        }
        framing = false;
        if (std::exchange(framing_drag, false) && GetCapture() == window)
        {
            ReleaseCapture();
        }
        hide_frame();
        std::wstring title;
        {
            std::scoped_lock lock{text_mutex};
            title = pending_title;
        }
        SetWindowTextW(window, title.c_str());
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }

    // Shows the frame between frame_anchor and frame_corner (client
    // coordinates); the edge windows are created the first time.
    void video_window::state::update_frame()
    {
        RECT selection{std::min(frame_anchor.x, frame_corner.x), std::min(frame_anchor.y, frame_corner.y),
                       std::max(frame_anchor.x, frame_corner.x), std::max(frame_anchor.y, frame_corner.y)};
        POINT origin{0, 0};
        ClientToScreen(window, &origin);
        OffsetRect(&selection, origin.x, origin.y);
        HINSTANCE const instance = GetModuleHandleW(nullptr);
        int const dpi = static_cast<int>(GetDpiForWindow(window));
        int const dark = std::max(1, MulDiv(frame_dark_thickness, dpi, 96));
        for (int ring = 0; ring < 2; ++ring)
        {
            // ring 0: light, just inside the selection; ring 1: dark, around it
            int const thickness = ring == 0 ? std::max(1, MulDiv(frame_light_thickness, dpi, 96)) : dark;
            RECT outer = selection;
            if (ring == 1)
            {
                InflateRect(&outer, dark, dark);
            }
            std::array<RECT, 4> const places{
                RECT{outer.left, outer.top, outer.right, outer.top + thickness},
                RECT{outer.left, outer.bottom - thickness, outer.right, outer.bottom},
                RECT{outer.left, outer.top, outer.left + thickness, outer.bottom},
                RECT{outer.right - thickness, outer.top, outer.right, outer.bottom}};
            for (std::size_t side = 0; side < places.size(); ++side)
            {
                HWND &edge = frame_edges[static_cast<std::size_t>(ring) * 4 + side];
                if (edge == nullptr)
                {
                    edge = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                           ring == 0 ? frame_light_class_name : frame_dark_class_name, L"", WS_POPUP,
                                           0, 0, 1, 1, window, nullptr, instance, nullptr);
                    if (edge == nullptr)
                    {
                        // the selection still works, only its outline is missing
                        if (!std::exchange(frame_failure_logged, true))
                        {
                            log::warning("crop frame: {}",
                                         make_win32_failure("CreateWindowExW", GetLastError()).describe());
                        }
                        continue;
                    }
                    SetLayeredWindowAttributes(edge, 0, 255, LWA_ALPHA);
                }
                RECT const &place = places[side];
                SetWindowPos(edge, HWND_TOP, place.left, place.top,
                             std::max(static_cast<int>(place.right - place.left), 1),
                             std::max(static_cast<int>(place.bottom - place.top), 1),
                             SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
        }
    }

    // The button was released: reports the framed part as fractions of the
    // picture shown.  A frame that is too small is a click: framing stays
    // armed for another try (Esc or the right button leaves it).
    void video_window::state::finish_framing()
    {
        framing_drag = false;
        if (GetCapture() == window)
        {
            ReleaseCapture();
        }
        hide_frame();
        RECT const picture = picture_rectangle();
        double const picture_width = picture.right - picture.left;
        double const picture_height = picture.bottom - picture.top;
        if (picture_width <= 0.0 || picture_height <= 0.0)
        {
            return;
        }
        LONG const left = std::clamp(std::min(frame_anchor.x, frame_corner.x), picture.left, picture.right);
        LONG const right = std::clamp(std::max(frame_anchor.x, frame_corner.x), picture.left, picture.right);
        LONG const top = std::clamp(std::min(frame_anchor.y, frame_corner.y), picture.top, picture.bottom);
        LONG const bottom = std::clamp(std::max(frame_anchor.y, frame_corner.y), picture.top, picture.bottom);
        int const smallest = MulDiv(smallest_selection_edge, static_cast<int>(GetDpiForWindow(window)), 96);
        if (right - left < smallest || bottom - top < smallest ||
            right - left < picture_width * smallest_selection_fraction ||
            bottom - top < picture_height * smallest_selection_fraction)
        {
            return;
        }
        fraction_region selection;
        selection.left = (left - picture.left) / picture_width;
        selection.right = (right - picture.left) / picture_width;
        selection.top = (top - picture.top) / picture_height;
        selection.bottom = (bottom - picture.top) / picture_height;
        end_framing();
        if (!detached && callbacks.on_crop_selected)
        {
            callbacks.on_crop_selected(selection);
        }
    }

    // Sizes and places the window for the current video.  keep_center: stay
    // where the window is (rotation, leaving fullscreen); otherwise use the
    // position remembered from the previous run, or the middle of the
    // primary monitor's work area.
    void video_window::state::place_for_video(bool keep_center)
    {
        if (video_width <= 0 || video_height <= 0)
        {
            return;
        }
        placement_pending = false;
        POINT center{};
        HMONITOR monitor = nullptr;
        if (keep_center)
        {
            RECT current{};
            GetWindowRect(window, &current);
            center = POINT{(current.left + current.right) / 2, (current.top + current.bottom) / 2};
            monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        }
        else if (remembered_center)
        {
            center = *remembered_center;
            // a monitor that is gone: the nearest remaining one
            monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
        }
        else
        {
            monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
            RECT const area = monitor_rect(monitor, true);
            center = POINT{(area.left + area.right) / 2, (area.top + area.bottom) / 2};
        }
        RECT const work = monitor_rect(monitor, true);
        double const work_width = work.right - work.left;
        double const work_height = work.bottom - work.top;
        if (work_width <= 0.0 || work_height <= 0.0)
        {
            return;
        }

        bool const landscape = video_width > video_height;
        auto const style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        placing = true;
        // the frame depends on the DPI of the monitor the window lands on:
        // when the move changes it, size once more with the new DPI
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            UINT const dpi = GetDpiForWindow(window);
            double scale = 0.0;
            if (landscape)
            {
                double const fraction = landscape_fraction.value_or(landscape_width_fraction);
                double target_width = work_width * fraction;
                if (!landscape_fraction)
                {
                    target_width = std::max(target_width, landscape_minimum_width * dpi / 96.0);
                }
                scale = target_width / video_width;
            }
            else
            {
                scale = work_height * portrait_fraction.value_or(portrait_height_fraction) / video_height;
            }
            scale = std::min({scale, work_width * largest_fraction / video_width,
                              work_height * largest_fraction / video_height});
            RECT client{0, 0, static_cast<LONG>(std::lround(video_width * scale)),
                        static_cast<LONG>(std::lround(video_height * scale))};
            AdjustWindowRectExForDpi(&client, style, FALSE, 0, dpi);
            int const outer_width = client.right - client.left;
            int const outer_height = client.bottom - client.top;
            int const left =
                std::clamp(static_cast<int>(center.x) - outer_width / 2, static_cast<int>(work.left),
                           std::max(static_cast<int>(work.left), static_cast<int>(work.right) - outer_width));
            int const top =
                std::clamp(static_cast<int>(center.y) - outer_height / 2, static_cast<int>(work.top),
                           std::max(static_cast<int>(work.top), static_cast<int>(work.bottom) - outer_height));
            SetWindowPos(window, nullptr, left, top, outer_width, outer_height, SWP_NOZORDER | SWP_NOACTIVATE);
            if (GetDpiForWindow(window) == dpi)
            {
                break;
            }
        }
        placing = false;
    }

    // WM_SIZING: the client area keeps the video's aspect ratio, so the
    // window hugs the picture instead of growing black bars.
    void video_window::state::constrain_to_video_aspect(WPARAM edge, RECT &outer) const
    {
        if (video_width <= 0 || video_height <= 0)
        {
            return;
        }
        UINT const dpi = GetDpiForWindow(window);
        RECT frame{};
        AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE, 0, dpi);
        int const frame_width = frame.right - frame.left;
        int const frame_height = frame.bottom - frame.top;
        double const aspect = static_cast<double>(video_width) / video_height;
        int const minimum_edge = MulDiv(minimum_client_edge, static_cast<int>(dpi), 96);
        int client_width = std::max(static_cast<int>(outer.right - outer.left) - frame_width, 1);
        int client_height = std::max(static_cast<int>(outer.bottom - outer.top) - frame_height, 1);

        bool const height_leads = edge == WMSZ_TOP || edge == WMSZ_BOTTOM;
        if (height_leads)
        {
            client_width = static_cast<int>(std::lround(client_height * aspect));
        }
        else
        {
            client_height = static_cast<int>(std::lround(client_width / aspect));
        }
        // the shorter edge decides the minimum size
        if (std::min(client_width, client_height) < minimum_edge)
        {
            if (aspect >= 1.0)
            {
                client_height = minimum_edge;
                client_width = static_cast<int>(std::lround(client_height * aspect));
            }
            else
            {
                client_width = minimum_edge;
                client_height = static_cast<int>(std::lround(client_width / aspect));
            }
        }
        int const outer_width = client_width + frame_width;
        int const outer_height = client_height + frame_height;
        // the edge or corner being dragged moves; the opposite one stays
        bool const left_moves = edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT;
        bool const top_moves = edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT;
        if (left_moves)
        {
            outer.left = outer.right - outer_width;
        }
        else
        {
            outer.right = outer.left + outer_width;
        }
        if (top_moves)
        {
            outer.top = outer.bottom - outer_height;
        }
        else
        {
            outer.bottom = outer.top + outer_height;
        }
    }

    // After the user moved or resized the window: the position, and a size
    // chosen by hand for this orientation, are remembered (also for the next
    // run).
    void video_window::state::remember_placement(bool size_changed)
    {
        if (is_fullscreen.load() || IsZoomed(window) || IsIconic(window) || video_width <= 0 || video_height <= 0)
        {
            return;
        }
        RECT outer{};
        RECT client{};
        if (!GetWindowRect(window, &outer) || !GetClientRect(window, &client))
        {
            return;
        }
        POINT const center{(outer.left + outer.right) / 2, (outer.top + outer.bottom) / 2};
        remembered_center = center;
        store_number(setting_center_x, std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(center.x)));
        store_number(setting_center_y, std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(center.y)));
        if (!size_changed)
        {
            return;
        }
        RECT const work = monitor_rect(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), true);
        double const work_width = work.right - work.left;
        double const work_height = work.bottom - work.top;
        if (work_width <= 0.0 || work_height <= 0.0)
        {
            return;
        }
        if (video_width > video_height)
        {
            landscape_fraction =
                std::clamp((client.right - client.left) / work_width, smallest_fraction, largest_fraction);
            store_number(setting_landscape_size,
                         static_cast<std::uint32_t>(std::lround(*landscape_fraction * 1000.0)));
        }
        else
        {
            portrait_fraction =
                std::clamp((client.bottom - client.top) / work_height, smallest_fraction, largest_fraction);
            store_number(setting_portrait_size,
                         static_cast<std::uint32_t>(std::lround(*portrait_fraction * 1000.0)));
        }
    }

    // Window memory of the previous run; values outside their valid range are
    // ignored (the defaults apply).
    void video_window::state::load_window_memory()
    {
        auto const fraction = [](std::wstring_view name) -> std::optional<double>
        {
            std::optional<std::uint32_t> const permille = stored_number(name);
            if (!permille || *permille < smallest_fraction * 1000.0 || *permille > largest_fraction * 1000.0)
            {
                return std::nullopt;
            }
            return *permille / 1000.0;
        };
        landscape_fraction = fraction(setting_landscape_size);
        portrait_fraction = fraction(setting_portrait_size);
        std::optional<std::uint32_t> const center_x = stored_number(setting_center_x);
        std::optional<std::uint32_t> const center_y = stored_number(setting_center_y);
        if (center_x && center_y)
        {
            POINT const center{std::bit_cast<std::int32_t>(*center_x), std::bit_cast<std::int32_t>(*center_y)};
            // only a point that is still on a monitor
            if (MonitorFromPoint(center, MONITOR_DEFAULTTONULL) != nullptr)
            {
                remembered_center = center;
            }
        }
        always_on_top.store(stored_number(setting_always_on_top).value_or(0) != 0);
    }

    void video_window::state::apply_always_on_top()
    {
        SetWindowPos(window, always_on_top.load() ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // The display and the PC stay awake only while the window shows a stream
    // that is still delivering pictures (not for a kept last frame).
    void video_window::state::apply_keep_awake()
    {
        bool const awake = is_visible.load() && stream_active.load();
        SetThreadExecutionState(awake ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS);
    }

    void video_window::state::add_tray_icon()
    {
        if (!tray_wanted)
        {
            return;
        }
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = tray_icon_id;
        data.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
        data.uCallbackMessage = message_tray_callback;
        data.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                                   LR_DEFAULTCOLOR));
        if (data.hIcon == nullptr)
        {
            data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        }
        {
            std::scoped_lock lock{text_mutex};
            copy_text(data.szTip, status_text);
        }
        tray_added = Shell_NotifyIconW(NIM_ADD, &data) != 0;
        if (tray_added)
        {
            data.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &data);
        }
    }

    void video_window::state::update_tray_icon()
    {
        if (!tray_added)
        {
            return;
        }
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = tray_icon_id;
        data.uFlags = NIF_TIP | NIF_SHOWTIP;
        {
            std::scoped_lock lock{text_mutex};
            copy_text(data.szTip, status_text);
        }
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    void video_window::state::remove_tray_icon()
    {
        if (!tray_added)
        {
            return;
        }
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = tray_icon_id;
        Shell_NotifyIconW(NIM_DELETE, &data);
        tray_added = false;
    }

    void video_window::state::show_tray_menu()
    {
        HMENU const menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }
        std::wstring status;
        std::wstring usb_line;
        {
            std::scoped_lock lock{text_mutex};
            status = status_text;
            usb_line = usb_link_text;
        }
        auto const item = [menu](UINT identifier, ui_text label, std::wstring_view key = {}, bool checked = false,
                                 bool enabled = true)
        {
            std::wstring text{ui_string(label)};
            if (!key.empty())
            {
                text += L'\t';
                text += key;
            }
            AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0U) | (enabled ? 0U : MF_GRAYED), identifier,
                        text.c_str());
        };
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, usb_line.c_str());
        if (enable_adapter_offer.load())
        {
            item(menu_enable_adapter, ui_text::enable_usb_adapter_menu);
        }
        if (data_guard_offer.load())
        {
            item(menu_data_guard, ui_text::protect_mobile_data_menu);
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        // switching here never touches the USB device: the cable keeps charging
        item(menu_prefer_usb, ui_text::prefer_usb_link);
        item(menu_wifi_only, ui_text::wifi_only);
        CheckMenuRadioItem(menu, menu_prefer_usb, menu_wifi_only,
                           prefer_usb_link.load() ? menu_prefer_usb : menu_wifi_only, MF_BYCOMMAND);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        item(menu_mode_quality, ui_text::mode_quality);
        item(menu_mode_balanced, ui_text::mode_balanced);
        item(menu_mode_performance, ui_text::mode_performance);
        performance_mode const current_mode = mode.load();
        CheckMenuRadioItem(menu, menu_mode_quality, menu_mode_performance,
                           current_mode == performance_mode::quality       ? menu_mode_quality
                           : current_mode == performance_mode::performance ? menu_mode_performance
                                                                           : menu_mode_balanced,
                           MF_BYCOMMAND);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        bool const shown = is_visible.load();
        item(menu_crop_off, ui_text::crop_off);
        item(menu_crop_automatic, ui_text::crop_automatic, L"C");
        item(menu_crop_select, ui_text::crop_select, L"X", false, shown);
        crop_mode const current_crop = crop.load();
        CheckMenuRadioItem(menu, menu_crop_off, menu_crop_select,
                           current_crop == crop_mode::automatic ? menu_crop_automatic
                           : current_crop == crop_mode::manual  ? menu_crop_select
                                                                : menu_crop_off,
                           MF_BYCOMMAND);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        item(menu_remote_control, ui_text::control_phone, L"F8", control_wanted.load());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        item(menu_fullscreen, is_fullscreen.load() ? ui_text::exit_full_screen : ui_text::full_screen, L"F11", false,
             shown);
        item(menu_always_on_top, ui_text::always_on_top, L"T", always_on_top.load());
        item(menu_mute, ui_text::mute, L"M", muted.load());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        item(menu_autostart, ui_text::start_with_windows, {}, autostart.load());
        item(menu_open_log, ui_text::open_log, {}, false, log_available.load());
        item(menu_check_for_updates, ui_text::check_for_updates);
        item(menu_about, ui_text::about);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        item(menu_exit, ui_text::exit);
        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(window); // required so the menu closes on outside clicks
        auto const chosen = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                                               cursor.x, cursor.y, window, nullptr));
        DestroyMenu(menu);
        PostMessageW(window, WM_NULL, 0, 0);
        switch (chosen)
        {
        case menu_fullscreen:
            tray(tray_command::toggle_fullscreen);
            break;
        case menu_always_on_top:
            tray(tray_command::toggle_always_on_top);
            break;
        case menu_data_guard:
            tray(tray_command::apply_data_guard);
            break;
        case menu_enable_adapter:
            tray(tray_command::enable_link_adapter);
            break;
        case menu_prefer_usb:
            tray(tray_command::prefer_usb_link);
            break;
        case menu_wifi_only:
            tray(tray_command::wifi_only);
            break;
        case menu_mute:
            tray(tray_command::toggle_mute);
            break;
        case menu_mode_quality:
            tray(tray_command::mode_quality);
            break;
        case menu_mode_balanced:
            tray(tray_command::mode_balanced);
            break;
        case menu_mode_performance:
            tray(tray_command::mode_performance);
            break;
        case menu_crop_off:
            tray(tray_command::crop_off);
            break;
        case menu_crop_automatic:
            tray(tray_command::crop_automatic);
            break;
        case menu_crop_select:
            tray(tray_command::crop_select);
            break;
        case menu_remote_control:
            tray(tray_command::toggle_remote_control);
            break;
        case menu_autostart:
            tray(tray_command::toggle_autostart);
            break;
        case menu_open_log:
            tray(tray_command::open_log);
            break;
        case menu_check_for_updates:
            tray(tray_command::check_for_updates);
            break;
        case menu_about:
            tray(tray_command::about);
            break;
        case menu_exit:
            tray(tray_command::exit);
            break;
        default:
            break;
        }
    }

    void video_window::state::tray(tray_command command)
    {
        if (command == tray_command::toggle_fullscreen)
        {
            if (is_visible.load())
            {
                if (IsIconic(window))
                {
                    ShowWindow(window, SW_RESTORE);
                }
                bring_to_front(window);
                apply_always_on_top();
                set_fullscreen(!is_fullscreen.load());
            }
            return;
        }
        if (command == tray_command::toggle_always_on_top)
        {
            end_framing(); // another choice was made
            bool const enabled = !always_on_top.load();
            always_on_top.store(enabled);
            store_number(setting_always_on_top, enabled ? 1U : 0U);
            apply_always_on_top();
            return;
        }
        if (command == tray_command::crop_select)
        {
            begin_framing();
            return;
        }
        end_framing(); // another choice was made
        if (!detached && callbacks.on_tray_command)
        {
            callbacks.on_tray_command(command);
        }
    }

    void video_window::state::refresh_overlay()
    {
        if (!overlay)
        {
            return;
        }
        overlay_model model;
        {
            std::scoped_lock lock{text_mutex};
            model.transport = stream_transport;
            model.detail = stream_detail;
        }
        model.prefer_usb_link = prefer_usb_link.load();
        model.fullscreen = is_fullscreen.load();
        model.muted = muted.load();
        model.offer_data_guard = data_guard_offer.load();
        model.offer_enable_adapter = enable_adapter_offer.load();
        overlay->set_model(std::move(model));
    }

    void video_window::state::on_overlay_command(overlay_command command)
    {
        switch (command)
        {
        case overlay_command::toggle_fullscreen:
            set_fullscreen(!is_fullscreen.load());
            break;
        case overlay_command::prefer_usb_link:
            tray(tray_command::prefer_usb_link);
            break;
        case overlay_command::wifi_only:
            tray(tray_command::wifi_only);
            break;
        case overlay_command::toggle_mute:
            tray(tray_command::toggle_mute);
            break;
        case overlay_command::apply_data_guard:
            // the click is user input to this process: it may take the
            // foreground, so the UAC prompt opens instead of flashing
            SetForegroundWindow(window);
            tray(tray_command::apply_data_guard);
            break;
        case overlay_command::enable_link_adapter:
            SetForegroundWindow(window);
            tray(tray_command::enable_link_adapter);
            break;
        case overlay_command::open_menu:
            show_tray_menu();
            break;
        }
    }

    LRESULT video_window::state::handle(UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (taskbar_created_message != 0 && message == taskbar_created_message)
        {
            // Explorer restarted: the notification area lost our icon
            tray_added = false;
            add_tray_icon();
            return 0;
        }
        if (second_launch_message != 0 && message == second_launch_message)
        {
            if (static_cast<std::uintptr_t>(wparam) != instance_tag)
            {
                return 0; // addressed to a receiver with another name
            }
            if (is_visible.load())
            {
                if (IsIconic(window))
                {
                    ShowWindow(window, SW_RESTORE);
                }
                bring_to_front(window);
                apply_always_on_top();
            }
            if (!detached && callbacks.on_second_launch)
            {
                callbacks.on_second_launch();
            }
            return second_launch_answer;
        }
        switch (message)
        {
        case message_show:
        {
            bool const aspect_changed =
                static_cast<int>(wparam) != video_width || static_cast<int>(lparam) != video_height;
            video_width = static_cast<int>(wparam);
            video_height = static_cast<int>(lparam);
            if (aspect_changed)
            {
                end_framing(); // the picture being framed is gone
            }
            if (!is_visible.load())
            {
                if (IsZoomed(window) || IsIconic(window))
                {
                    // hidden while maximized or minimized: SW_SHOWNORMAL below
                    // restores the old rectangle (the previous stream's shape),
                    // and WM_SIZE(SIZE_RESTORED) then shapes it for this one
                    placement_pending = true;
                }
                else if (!is_fullscreen.load())
                {
                    place_for_video(shown_once);
                }
                ShowWindow(window, SW_SHOWNORMAL);
                bring_to_front(window);
                apply_always_on_top();
                is_visible.store(true);
                // keep the monitor awake while mirrored video is on screen
                stream_active.store(true);
                apply_keep_awake();
                if (fullscreen_wanted)
                {
                    enter_fullscreen();
                }
                shown_once = true;
                // like a player: show the controls briefly when playback starts
                refresh_overlay();
                if (overlay)
                {
                    overlay->owner_changed();
                    overlay->activity();
                }
            }
            else if (aspect_changed)
            {
                if (is_fullscreen.load() || IsZoomed(window) || IsIconic(window))
                {
                    // re-shape when the window is back to its normal state
                    placement_pending = true;
                }
                else
                {
                    place_for_video(true);
                }
            }
            return 0;
        }
        case message_hide:
            end_framing();
            release_remote_input();
            if (overlay)
            {
                overlay->hide();
            }
            if (is_visible.load())
            {
                // fullscreen_wanted is kept: the next session opens the same way
                leave_fullscreen();
                ShowWindow(window, SW_HIDE);
                is_visible.store(false);
                apply_keep_awake();
            }
            return 0;
        case message_keep_awake:
            apply_keep_awake();
            return 0;
        case message_detach:
            // The function objects stay alive: one of them may be running
            // further up this very stack (a callback inside a modal loop, such
            // as a message box, still receives sent messages).
            end_framing();
            release_remote_input();
            detached = true;
            return 0;
        case message_remote_control:
            apply_remote_control();
            return 0;
        case WM_KILLFOCUS:
            // another window gets the keys: none stays down on the phone
            release_remote_input();
            escape_taken = false; // its release goes elsewhere
            break;
        case message_title:
        {
            std::wstring title;
            {
                std::scoped_lock lock{text_mutex};
                title = pending_title;
            }
            if (!framing) // the title shows the framing hint; end_framing sets this one
            {
                SetWindowTextW(window, title.c_str());
            }
            return 0;
        }
        case message_toggle_fullscreen:
            set_fullscreen(!is_fullscreen.load());
            return 0;
        case message_tray_update:
            update_tray_icon();
            return 0;
        case message_overlay_update:
            refresh_overlay();
            return 0;
        case message_tray_notify:
            if (tray_added)
            {
                NOTIFYICONDATAW data{};
                data.cbSize = sizeof(data);
                data.hWnd = window;
                data.uID = tray_icon_id;
                data.uFlags = NIF_INFO;
                data.dwInfoFlags = NIIF_INFO;
                {
                    std::scoped_lock lock{text_mutex};
                    copy_text(data.szInfoTitle, notify_title);
                    copy_text(data.szInfo, notify_text);
                    balloon_action = notify_action;
                }
                Shell_NotifyIconW(NIM_MODIFY, &data);
            }
            return 0;
        case message_tray_callback:
            switch (LOWORD(lparam))
            {
            case WM_CONTEXTMENU:
            case WM_RBUTTONUP:
                show_tray_menu();
                return 0;
            case NIN_SELECT:
            case NIN_KEYSELECT:
            case WM_LBUTTONDBLCLK:
                if (is_visible.load())
                {
                    if (IsIconic(window))
                    {
                        ShowWindow(window, SW_RESTORE);
                    }
                    bring_to_front(window);
                    apply_always_on_top();
                }
                else
                {
                    // nothing to show while idle: the menu tells what is going on
                    show_tray_menu();
                }
                return 0;
            case NIN_BALLOONUSERCLICK:
                // the click gives this process the foreground, so a UAC
                // prompt started now opens directly instead of flashing
                if (std::optional<tray_command> const action = std::exchange(balloon_action, std::nullopt); action)
                {
                    SetForegroundWindow(window);
                    tray(*action);
                }
                return 0;
            case NIN_BALLOONTIMEOUT:
            case NIN_BALLOONHIDE:
                balloon_action.reset();
                return 0;
            default:
                return 0;
            }
        case message_quit:
            KillTimer(window, cursor_timer_id);
            overlay.reset(); // owned popup: destroy before the owner
            remove_tray_icon();
            if (device_notification != nullptr)
            {
                UnregisterDeviceNotification(device_notification);
                device_notification = nullptr;
            }
            SetThreadExecutionState(ES_CONTINUOUS);
            DestroyWindow(window);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wparam != FALSE && !detached && callbacks.on_session_ending)
            {
                callbacks.on_session_ending();
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_CLOSE:
            // the window is reused for the next session: hide, never destroy
            if (!detached && callbacks.on_close_requested)
            {
                callbacks.on_close_requested();
            }
            return 0;
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED && !detached && callbacks.on_client_resized)
            {
                callbacks.on_client_resized(LOWORD(lparam), HIWORD(lparam));
            }
            if (overlay)
            {
                overlay->owner_changed();
            }
            if (wparam == SIZE_RESTORED && placement_pending && !placing && !is_fullscreen.load())
            {
                // the phone rotated while the window was maximized or minimized
                place_for_video(true);
            }
            return 0;
        case WM_SIZING:
            if (!is_fullscreen.load())
            {
                constrain_to_video_aspect(wparam, *reinterpret_cast<RECT *>(lparam));
                resized_by_user = true;
                return TRUE;
            }
            break;
        case WM_EXITSIZEMOVE:
            remember_placement(std::exchange(resized_by_user, false));
            break;
        case WM_MOVE:
            if (overlay)
            {
                overlay->owner_changed();
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            // A double-click switches full screen on and off, also while the
            // phone is controlled: the window's own command comes before the
            // phone's input.  (The first click of the pair went to the phone
            // as a tap when it happened; this second one does not, and
            // neither does its release.)  With Alt held the pair belongs to
            // the phone: a double tap.
            if (!framing && !(controlling() && GetKeyState(VK_MENU) < 0))
            {
                log::debug("window: double-click, full screen {}", is_fullscreen.load() ? "off" : "on");
                set_fullscreen(!is_fullscreen.load());
                return 0;
            }
            // while framing, and for Alt + double-click on the controlled
            // phone, the second press is a press like any other
            [[fallthrough]];
        case WM_LBUTTONDOWN:
            if (framing)
            {
                RECT const picture = picture_rectangle();
                frame_anchor = POINT{std::clamp(static_cast<LONG>(GET_X_LPARAM(lparam)), picture.left, picture.right),
                                     std::clamp(static_cast<LONG>(GET_Y_LPARAM(lparam)), picture.top, picture.bottom)};
                frame_corner = frame_anchor;
                framing_drag = true;
                SetCapture(window);
                return 0;
            }
            if (controlling())
            {
                // a finger goes down where the pointer is (nothing on the bars)
                if (auto const position = picture_fraction(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, false);
                    position)
                {
                    pointer_pressed = true;
                    pointer_position = *position;
                    SetCapture(window);
                    if (!detached && callbacks.on_pointer)
                    {
                        callbacks.on_pointer(pointer_action::press, position->first, position->second);
                    }
                }
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (framing && framing_drag)
            {
                finish_framing();
                return 0;
            }
            if (pointer_pressed)
            {
                if (auto const position = picture_fraction(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, true);
                    position)
                {
                    pointer_position = *position;
                }
                release_remote_input();
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (controlling())
            {
                // (the position of this message is in screen coordinates)
                POINT pointer{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(window, &pointer);
                if (auto const position = picture_fraction(pointer, false);
                    position && !pointer_pressed && !detached && callbacks.on_wheel)
                {
                    double const notches = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
                    // a tilting wheel, or Shift with the wheel, scrolls sideways
                    bool const sideways =
                        message == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) != 0;
                    callbacks.on_wheel(position->first, position->second,
                                       sideways ? (message == WM_MOUSEHWHEEL ? notches : -notches) : 0.0,
                                       sideways ? 0.0 : notches);
                }
                return 0;
            }
            break;
        case WM_MBUTTONDOWN:
            if (controlling())
            {
                if (!detached && callbacks.on_home_button)
                {
                    callbacks.on_home_button();
                }
                return 0;
            }
            break;
        case WM_RBUTTONDOWN:
            if (framing)
            {
                end_framing();
                right_click_consumed = true;
                return 0;
            }
            break;
        case WM_RBUTTONUP:
            if (std::exchange(right_click_consumed, false))
            {
                return 0; // no WM_CONTEXTMENU for the click that cancelled a frame
            }
            break;
        case WM_CONTEXTMENU:
            // right click anywhere on the picture (or the Menu key): the same
            // menu as the notification-area icon and the control bar's "more"
            if (!framing)
            {
                show_cursor_again();
                show_tray_menu();
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (framing_drag)
            {
                // another window took the mouse: this drag is over, framing stays armed
                framing_drag = false;
                hide_frame();
            }
            if (pointer_pressed)
            {
                release_remote_input(); // the finger lifts where it was
            }
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wparam == VK_ESCAPE && std::exchange(escape_taken, false))
            {
                return 0; // the release of a press the window has used
            }
            if (controlling() && control_key(false, wparam, lparam))
            {
                return 0;
            }
            break;
        case WM_CHAR:
        case WM_SYSDEADCHAR:
        case WM_DEADCHAR:
            if (controlling())
            {
                return 0; // the text of keys that went to the phone
            }
            break;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE && escape_taken)
            {
                return 0; // the auto-repeat of a press the window has used
            }
            if (framing)
            {
                // only Esc while a frame is being drawn
                if (wparam == VK_ESCAPE)
                {
                    end_framing();
                    escape_taken = true;
                }
                return 0;
            }
            // Esc leaves full screen before anything else looks at the key:
            // while the phone is controlled it is the window's key too, and
            // the phone gets neither this press, nor its auto-repeat, nor its
            // release (escape_taken).
            if (wparam == VK_ESCAPE && is_fullscreen.load())
            {
                log::debug("window: Esc, full screen off");
                set_fullscreen(false);
                escape_taken = true;
                return 0;
            }
            if (wparam == VK_F8)
            {
                if ((lparam & (1 << 30)) == 0) // not the auto-repeat of a held key
                {
                    tray(tray_command::toggle_remote_control);
                }
                return 0;
            }
            if (controlling() && control_key(true, wparam, lparam))
            {
                return 0;
            }
            if (wparam == 'C')
            {
                tray(crop.load() == crop_mode::automatic ? tray_command::crop_off : tray_command::crop_automatic);
                return 0;
            }
            if (wparam == 'X')
            {
                tray(tray_command::crop_select);
                return 0;
            }
            if (wparam == 'F' || wparam == VK_F11)
            {
                set_fullscreen(!is_fullscreen.load());
                return 0;
            }
            if (wparam == 'T')
            {
                tray(tray_command::toggle_always_on_top);
                return 0;
            }
            if (wparam == 'M')
            {
                tray(tray_command::toggle_mute);
                return 0;
            }
            if (wparam == VK_ESCAPE)
            {
                set_fullscreen(false);
                return 0;
            }
            break;
        case WM_SYSKEYDOWN:
            if (wparam == VK_RETURN && (lparam & (1 << 29)) != 0)
            {
                set_fullscreen(!is_fullscreen.load());
                return 0;
            }
            if (controlling() && control_key(true, wparam, lparam))
            {
                return 0; // Alt and Alt+key go to the phone (no window menu)
            }
            break;
        case WM_SYSCHAR:
            if (wparam == VK_RETURN || (controlling() && wparam != VK_SPACE))
            {
                return 0; // swallow the beep of Alt+Enter and of Alt+key sent to the phone
            }
            break;
        case WM_MOUSEMOVE:
        {
            if (framing)
            {
                if (framing_drag)
                {
                    RECT const picture = picture_rectangle();
                    frame_corner =
                        POINT{std::clamp(static_cast<LONG>(GET_X_LPARAM(lparam)), picture.left, picture.right),
                              std::clamp(static_cast<LONG>(GET_Y_LPARAM(lparam)), picture.top, picture.bottom)};
                    update_frame();
                }
                return 0;
            }
            if (pointer_pressed)
            {
                // the finger follows the pointer (and stays on the screen's
                // edge when the pointer leaves the picture)
                if (auto const position = picture_fraction(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, true);
                    position && *position != pointer_position)
                {
                    pointer_position = *position;
                    if (!detached && callbacks.on_pointer)
                    {
                        callbacks.on_pointer(pointer_action::move, position->first, position->second);
                    }
                }
            }
            // Windows also sends WM_MOUSEMOVE when the window moves under a
            // still pointer or the cursor changes: react to real movement only
            POINT pointer{};
            GetCursorPos(&pointer);
            if (pointer.x == last_pointer.x && pointer.y == last_pointer.y)
            {
                break;
            }
            last_pointer = pointer;
            show_cursor_again();
            if (overlay && !controlling())
            {
                overlay->activity();
            }
            if (is_fullscreen.load())
            {
                SetTimer(window, cursor_timer_id, cursor_hide_delay_ms, nullptr);
            }
            break;
        }
        case WM_TIMER:
            if (wparam == cursor_timer_id)
            {
                KillTimer(window, cursor_timer_id);
                if (is_fullscreen.load() && !framing)
                {
                    cursor_hidden = true;
                    SetCursor(nullptr);
                }
                return 0;
            }
            break;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT)
            {
                SetCursor(framing         ? LoadCursorW(nullptr, IDC_CROSS)
                          : cursor_hidden ? nullptr
                                          : LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_GETMINMAXINFO:
        {
            auto *limits = reinterpret_cast<MINMAXINFO *>(lparam);
            int const minimum_edge = MulDiv(minimum_client_edge, static_cast<int>(GetDpiForWindow(window)), 96);
            limits->ptMinTrackSize.x = minimum_edge;
            limits->ptMinTrackSize.y = minimum_edge;
            return 0;
        }
        case WM_DPICHANGED:
        {
            if (placing)
            {
                // place_for_video sizes for the new monitor itself
                return 0;
            }
            auto const *suggested = reinterpret_cast<RECT const *>(lparam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DEVICECHANGE:
            if ((wparam == DBT_DEVICEARRIVAL || wparam == DBT_DEVICEREMOVECOMPLETE) && lparam != 0 &&
                reinterpret_cast<DEV_BROADCAST_HDR const *>(lparam)->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE &&
                !detached && callbacks.on_usb_device_change)
            {
                callbacks.on_usb_device_change();
            }
            return TRUE;
        case WM_DISPLAYCHANGE:
            // a monitor was plugged in/out or changed resolution/refresh:
            // let the engine re-detect the display and re-pace presentation
            if (is_fullscreen.load())
            {
                // keep covering the monitor at its new size
                RECT const monitor = monitor_rect(window, false);
                SetWindowPos(window, nullptr, monitor.left, monitor.top, monitor.right - monitor.left,
                             monitor.bottom - monitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (!detached && callbacks.on_display_changed)
            {
                callbacks.on_display_changed();
            }
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    std::uintptr_t second_launch_tag(std::wstring_view receiver_name)
    {
        // FNV-1a over the UTF-16 code units (32 bits: fits a WPARAM everywhere)
        std::uint32_t hash = 2166136261U;
        for (wchar_t const unit : receiver_name)
        {
            hash = (hash ^ static_cast<std::uint32_t>(unit)) * 16777619U;
        }
        return hash;
    }

    video_window::video_window(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    video_window::~video_window()
    {
        if (impl->window != nullptr)
        {
            PostMessageW(impl->window, message_quit, 0, 0);
        }
        if (impl->ui_thread.joinable())
        {
            impl->ui_thread.join();
        }
    }

    outcome<std::unique_ptr<video_window>> video_window::create(std::wstring title, window_callbacks callbacks,
                                                                bool start_fullscreen, bool tray_icon)
    {
        auto created = std::make_unique<state>();
        created->callbacks = std::move(callbacks);
        created->fullscreen_wanted = start_fullscreen;
        created->tray_wanted = tray_icon;
        created->status_text = title;
        created->pending_title = title;
        created->instance_tag = second_launch_tag(title);
        state *const shared = created.get();

        std::promise<status> ready;
        std::future<status> ready_result = ready.get_future();
        created->ui_thread = std::thread{[shared, title = std::move(title), &ready]
        {
            HINSTANCE const instance = GetModuleHandleW(nullptr);
            WNDCLASSEXW window_class{};
            window_class.cbSize = sizeof(window_class);
            window_class.style = CS_DBLCLKS;
            window_class.lpfnWndProc = state::procedure;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
            window_class.lpszClassName = window_class_name;
            RegisterClassExW(&window_class);
            // edges of the hand-drawn crop frame: plain filled popups
            WNDCLASSEXW edge_class{};
            edge_class.cbSize = sizeof(edge_class);
            edge_class.lpfnWndProc = DefWindowProcW;
            edge_class.hInstance = instance;
            edge_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
            edge_class.lpszClassName = frame_light_class_name;
            RegisterClassExW(&edge_class);
            edge_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            edge_class.lpszClassName = frame_dark_class_name;
            RegisterClassExW(&edge_class);

            HWND const window = CreateWindowExW(0, window_class_name, title.c_str(), WS_OVERLAPPEDWINDOW,
                                                CW_USEDEFAULT, CW_USEDEFAULT, 960, 540, nullptr, nullptr, instance,
                                                shared);
            if (window == nullptr)
            {
                ready.set_value(std::unexpected(make_win32_failure("CreateWindowExW", GetLastError())));
                return;
            }
            shared->window = window;
            use_dark_frame(window);
            shared->load_window_memory();
            shared->taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
            shared->second_launch_message = RegisterWindowMessageW(second_launch_message_name);
            if (shared->second_launch_message != 0)
            {
                // a second start is not elevated when this instance is: without
                // this, Windows would not deliver its message
                ChangeWindowMessageFilterEx(window, shared->second_launch_message, MSGFLT_ALLOW, nullptr);
            }
            shared->add_tray_icon();
            if (auto bar = video_overlay::create(window, [shared](overlay_command command)
                {
                    shared->on_overlay_command(command);
                });
                bar)
            {
                shared->overlay = std::move(*bar);
            }
            else
            {
                // the window, keyboard shortcuts and tray menu still work
                log::warning("control bar unavailable: {}", bar.error().describe());
            }

            DEV_BROADCAST_DEVICEINTERFACE_W filter{};
            filter.dbcc_size = sizeof(filter);
            filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
            shared->device_notification = RegisterDeviceNotificationW(
                window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
            ready.set_value(status{});

            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            shared->window = nullptr;
        }};

        status const started = ready_result.get();
        if (!started)
        {
            created->ui_thread.join();
            return std::unexpected(started.error());
        }
        return std::unique_ptr<video_window>{new video_window{std::move(created)}};
    }

    void *video_window::native_handle() const
    {
        return impl->window;
    }

    void video_window::detach_callbacks()
    {
        if (impl->window != nullptr)
        {
            // sent, not posted: it returns when the UI thread has done it,
            // i.e. after any callback that was running
            SendMessageW(impl->window, message_detach, 0, 0);
        }
    }

    void video_window::show_for_video(int video_width, int video_height)
    {
        PostMessageW(impl->window, message_show, static_cast<WPARAM>(video_width), static_cast<LPARAM>(video_height));
    }

    void video_window::hide()
    {
        PostMessageW(impl->window, message_hide, 0, 0);
    }

    void video_window::set_title(std::wstring title)
    {
        {
            std::scoped_lock lock{impl->text_mutex};
            impl->pending_title = std::move(title);
        }
        PostMessageW(impl->window, message_title, 0, 0);
    }

    void video_window::toggle_fullscreen()
    {
        PostMessageW(impl->window, message_toggle_fullscreen, 0, 0);
    }

    void video_window::set_status(std::wstring status)
    {
        {
            std::scoped_lock lock{impl->text_mutex};
            impl->status_text = std::move(status);
        }
        PostMessageW(impl->window, message_tray_update, 0, 0);
    }

    void video_window::set_usb_link(std::wstring text, bool offer_data_guard, bool offer_enable_adapter)
    {
        {
            std::scoped_lock lock{impl->text_mutex};
            impl->usb_link_text = std::move(text);
        }
        impl->data_guard_offer.store(offer_data_guard);
        impl->enable_adapter_offer.store(offer_enable_adapter);
        PostMessageW(impl->window, message_overlay_update, 0, 0);
    }

    void video_window::set_link_preference(bool prefer_usb_link)
    {
        impl->prefer_usb_link.store(prefer_usb_link);
        PostMessageW(impl->window, message_overlay_update, 0, 0);
    }

    void video_window::set_stream_info(std::wstring transport, std::wstring detail)
    {
        {
            std::scoped_lock lock{impl->text_mutex};
            impl->stream_transport = std::move(transport);
            impl->stream_detail = std::move(detail);
        }
        PostMessageW(impl->window, message_overlay_update, 0, 0);
    }

    void video_window::set_muted(bool muted)
    {
        impl->muted.store(muted);
        PostMessageW(impl->window, message_overlay_update, 0, 0);
    }

    void video_window::set_performance_mode(performance_mode mode)
    {
        impl->mode.store(mode);
    }

    void video_window::set_crop_mode(crop_mode mode)
    {
        impl->crop.store(mode);
    }

    void video_window::set_remote_control(bool wanted, bool ready)
    {
        impl->control_wanted.store(wanted);
        if (impl->control_ready.exchange(ready) != ready)
        {
            PostMessageW(impl->window, message_remote_control, 0, 0);
        }
    }

    void video_window::set_autostart(bool enabled)
    {
        impl->autostart.store(enabled);
    }

    void video_window::set_log_available(bool available)
    {
        impl->log_available.store(available);
    }

    void video_window::set_stream_active(bool active)
    {
        if (impl->stream_active.exchange(active) != active)
        {
            PostMessageW(impl->window, message_keep_awake, 0, 0);
        }
    }

    void video_window::notify(std::wstring title, std::wstring text, std::optional<tray_command> on_click)
    {
        {
            std::scoped_lock lock{impl->text_mutex};
            impl->notify_title = std::move(title);
            impl->notify_text = std::move(text);
            impl->notify_action = on_click;
        }
        PostMessageW(impl->window, message_tray_notify, 0, 0);
    }

    bool video_window::visible() const
    {
        return impl->is_visible.load();
    }

    bool video_window::fullscreen() const
    {
        return impl->is_fullscreen.load();
    }
}
