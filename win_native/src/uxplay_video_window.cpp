module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbt.h>
#include <shellapi.h>
#include <windowsx.h>

module uxplay_video_window;

import std;
import uxplay_status;
import uxplay_log;

namespace uxplay
{
    namespace
    {
        constexpr wchar_t window_class_name[] = L"uxplay_native_video_window";
        constexpr UINT message_show = WM_APP + 1;
        constexpr UINT message_hide = WM_APP + 2;
        constexpr UINT message_title = WM_APP + 3;
        constexpr UINT message_toggle_fullscreen = WM_APP + 4;
        constexpr UINT message_quit = WM_APP + 5;
        constexpr UINT message_tray_update = WM_APP + 6;
        constexpr UINT message_tray_notify = WM_APP + 7;
        constexpr UINT message_tray_callback = WM_APP + 10;
        constexpr UINT tray_icon_id = 1;
        constexpr UINT_PTR cursor_timer_id = 1;
        constexpr UINT cursor_hide_delay_ms = 2500;
        // Initial window: largest size of the video aspect within this
        // fraction of the monitor work area.
        constexpr double work_area_fraction = 0.9;

        enum menu_identifier : UINT
        {
            menu_fullscreen = 100,
            menu_usb,
            menu_open_log,
            menu_exit
        };

        RECT monitor_rect(HWND window, bool work_area)
        {
            HMONITOR const monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            GetMonitorInfoW(monitor, &info);
            return work_area ? info.rcWork : info.rcMonitor;
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
        std::wstring status_text = L"UxPlay";
        std::wstring notify_title;
        std::wstring notify_text;
        std::mutex text_mutex;
        HWND window = nullptr;
        HDEVNOTIFY device_notification = nullptr;
        std::thread ui_thread;
        std::atomic<bool> is_visible{false};
        std::atomic<bool> is_fullscreen{false};
        std::atomic<bool> usb_enabled{true};
        bool fullscreen_on_first_show = false;
        bool shown_once = false;
        bool cursor_hidden = false;
        bool tray_wanted = false;
        bool tray_added = false;
        UINT taskbar_created_message = 0;
        int video_width = 0;
        int video_height = 0;
        WINDOWPLACEMENT saved_placement{};
        LONG_PTR saved_style = 0;

        void enter_fullscreen();
        void leave_fullscreen();
        void set_fullscreen(bool enable);
        void place_for_video(bool keep_center);
        void show_cursor_again();
        void add_tray_icon();
        void update_tray_icon();
        void remove_tray_icon();
        void show_tray_menu();
        void tray(tray_command command);
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
        if (callbacks.on_fullscreen_changed)
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
        if (callbacks.on_fullscreen_changed)
        {
            callbacks.on_fullscreen_changed(false);
        }
    }

    void video_window::state::set_fullscreen(bool enable)
    {
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

    void video_window::state::place_for_video(bool keep_center)
    {
        if (video_width <= 0 || video_height <= 0)
        {
            return;
        }
        RECT const work = monitor_rect(window, true);
        double const max_width = (work.right - work.left) * work_area_fraction;
        double const max_height = (work.bottom - work.top) * work_area_fraction;
        double const scale = std::min(max_width / video_width, max_height / video_height);
        RECT client{0, 0, static_cast<LONG>(std::lround(video_width * scale)),
                    static_cast<LONG>(std::lround(video_height * scale))};
        auto const style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        UINT const dpi = GetDpiForWindow(window);
        AdjustWindowRectExForDpi(&client, style, FALSE, 0, dpi);
        int const outer_width = client.right - client.left;
        int const outer_height = client.bottom - client.top;

        int center_x = (work.left + work.right) / 2;
        int center_y = (work.top + work.bottom) / 2;
        if (keep_center)
        {
            RECT current{};
            GetWindowRect(window, &current);
            center_x = (current.left + current.right) / 2;
            center_y = (current.top + current.bottom) / 2;
        }
        int const left = std::clamp(center_x - outer_width / 2, static_cast<int>(work.left),
                                    std::max(static_cast<int>(work.left), static_cast<int>(work.right) - outer_width));
        int const top = std::clamp(center_y - outer_height / 2, static_cast<int>(work.top),
                                   std::max(static_cast<int>(work.top), static_cast<int>(work.bottom) - outer_height));
        SetWindowPos(window, nullptr, left, top, outer_width, outer_height, SWP_NOZORDER | SWP_NOACTIVATE);
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
        {
            std::scoped_lock lock{text_mutex};
            status = status_text;
        }
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (is_visible.load() ? 0U : MF_GRAYED), menu_fullscreen,
                    L"全螢幕切換\tF11 / 雙擊畫面");
        AppendMenuW(menu, MF_STRING | (usb_enabled.load() ? MF_CHECKED : 0U), menu_usb,
                    L"USB 投影（插上 iPhone 自動開始）");
        AppendMenuW(menu, MF_STRING, menu_open_log, L"開啟記錄檔");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, menu_exit, L"結束 UxPlay");
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
        case menu_usb:
            tray(tray_command::toggle_usb);
            break;
        case menu_open_log:
            tray(tray_command::open_log);
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
                bring_to_front(window);
                set_fullscreen(!is_fullscreen.load());
            }
            return;
        }
        if (callbacks.on_tray_command)
        {
            callbacks.on_tray_command(command);
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
        switch (message)
        {
        case message_show:
        {
            bool const aspect_changed =
                static_cast<int>(wparam) != video_width || static_cast<int>(lparam) != video_height;
            video_width = static_cast<int>(wparam);
            video_height = static_cast<int>(lparam);
            if (!is_visible.load())
            {
                if (!is_fullscreen.load() && !IsZoomed(window))
                {
                    place_for_video(shown_once);
                }
                ShowWindow(window, SW_SHOWNORMAL);
                bring_to_front(window);
                is_visible.store(true);
                // keep the monitor awake while mirrored video is on screen
                SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
                if (!shown_once && fullscreen_on_first_show)
                {
                    enter_fullscreen();
                }
                shown_once = true;
            }
            else if (aspect_changed && !is_fullscreen.load() && !IsZoomed(window))
            {
                place_for_video(true);
            }
            return 0;
        }
        case message_hide:
            if (is_visible.load())
            {
                leave_fullscreen();
                ShowWindow(window, SW_HIDE);
                is_visible.store(false);
                SetThreadExecutionState(ES_CONTINUOUS);
            }
            return 0;
        case message_title:
        {
            std::wstring title;
            {
                std::scoped_lock lock{text_mutex};
                title = pending_title;
            }
            SetWindowTextW(window, title.c_str());
            return 0;
        }
        case message_toggle_fullscreen:
            set_fullscreen(!is_fullscreen.load());
            return 0;
        case message_tray_update:
            update_tray_icon();
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
                    bring_to_front(window);
                }
                return 0;
            default:
                return 0;
            }
        case message_quit:
            KillTimer(window, cursor_timer_id);
            remove_tray_icon();
            if (device_notification != nullptr)
            {
                UnregisterDeviceNotification(device_notification);
                device_notification = nullptr;
            }
            SetThreadExecutionState(ES_CONTINUOUS);
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_CLOSE:
            // the window is reused for the next session: hide, never destroy
            if (callbacks.on_close_requested)
            {
                callbacks.on_close_requested();
            }
            return 0;
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED && callbacks.on_client_resized)
            {
                callbacks.on_client_resized(LOWORD(lparam), HIWORD(lparam));
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
            set_fullscreen(!is_fullscreen.load());
            return 0;
        case WM_KEYDOWN:
            if (wparam == 'F' || wparam == VK_F11)
            {
                set_fullscreen(!is_fullscreen.load());
                return 0;
            }
            if (wparam == VK_ESCAPE)
            {
                leave_fullscreen();
                return 0;
            }
            break;
        case WM_SYSKEYDOWN:
            if (wparam == VK_RETURN && (lparam & (1 << 29)) != 0)
            {
                set_fullscreen(!is_fullscreen.load());
                return 0;
            }
            break;
        case WM_SYSCHAR:
            if (wparam == VK_RETURN)
            {
                return 0; // swallow the Alt+Enter beep
            }
            break;
        case WM_MOUSEMOVE:
            show_cursor_again();
            if (is_fullscreen.load())
            {
                SetTimer(window, cursor_timer_id, cursor_hide_delay_ms, nullptr);
            }
            break;
        case WM_TIMER:
            if (wparam == cursor_timer_id)
            {
                KillTimer(window, cursor_timer_id);
                if (is_fullscreen.load())
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
                SetCursor(cursor_hidden ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_GETMINMAXINFO:
        {
            auto *limits = reinterpret_cast<MINMAXINFO *>(lparam);
            limits->ptMinTrackSize.x = 160;
            limits->ptMinTrackSize.y = 160;
            return 0;
        }
        case WM_DPICHANGED:
        {
            auto const *suggested = reinterpret_cast<RECT const *>(lparam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DEVICECHANGE:
            if ((wparam == DBT_DEVICEARRIVAL || wparam == DBT_DEVICEREMOVECOMPLETE) && lparam != 0 &&
                reinterpret_cast<DEV_BROADCAST_HDR const *>(lparam)->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE &&
                callbacks.on_usb_device_change)
            {
                callbacks.on_usb_device_change();
            }
            return TRUE;
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
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
        created->fullscreen_on_first_show = start_fullscreen;
        created->tray_wanted = tray_icon;
        created->status_text = title;
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

            HWND const window = CreateWindowExW(0, window_class_name, title.c_str(), WS_OVERLAPPEDWINDOW,
                                                CW_USEDEFAULT, CW_USEDEFAULT, 960, 540, nullptr, nullptr, instance,
                                                shared);
            if (window == nullptr)
            {
                ready.set_value(std::unexpected(make_win32_failure("CreateWindowExW", GetLastError())));
                return;
            }
            shared->window = window;
            shared->taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
            shared->add_tray_icon();

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

    void video_window::set_usb_enabled(bool enabled)
    {
        impl->usb_enabled.store(enabled);
    }

    void video_window::notify(std::wstring title, std::wstring text)
    {
        {
            std::scoped_lock lock{impl->text_mutex};
            impl->notify_title = std::move(title);
            impl->notify_text = std::move(text);
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
