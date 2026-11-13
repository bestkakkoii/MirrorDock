// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <objbase.h>
#include <shellapi.h>
#include <wrl/client.h>

#include "uxplay_version.h"

module uxplay_about_window;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_ui_text;
import uxplay_update_check;
import uxplay_system_report;
import uxplay_build_info;

namespace uxplay
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr wchar_t window_class_name[] = L"uxplay_about_window";
        constexpr UINT message_model = WM_APP + 1;    // what is shown changed
        constexpr UINT message_activate = WM_APP + 2; // wParam: page, lParam: 1 = stay on top
        constexpr UINT message_close = WM_APP + 3;
        constexpr UINT_PTR copied_timer_id = 1;
        constexpr UINT copied_feedback_ms = 1600;
        // on the return to the window the PC is measured again when the last look is this old
        constexpr ULONGLONG return_measure_ms = 3000;

        // layout in device-independent pixels (scaled by the monitor's DPI)
        constexpr float default_width = 760.0F;
        constexpr float default_height = 820.0F;
        constexpr float minimum_width = 560.0F;
        constexpr float minimum_height = 420.0F;
        constexpr float side_margin = 24.0F;
        constexpr float widest_column = 860.0F;
        constexpr float card_padding = 20.0F;
        constexpr float card_radius = 8.0F;
        constexpr float card_gap = 16.0F;
        constexpr float icon_size = 56.0F;
        constexpr float tab_height = 38.0F;
        constexpr float button_height = 32.0F;
        constexpr float toggle_width = 44.0F;
        constexpr float toggle_height = 22.0F;
        constexpr float row_padding = 11.0F;
        constexpr float dot_size = 9.0F;
        constexpr float wheel_step = 72.0F;
        constexpr float key_step = 48.0F;

        // from this score on: "excellent", "good", "fair" (below: "poor")
        constexpr int excellent_score = 90;
        constexpr int good_score = 75;
        constexpr int fair_score = 50;

        enum class text_style : std::size_t
        {
            title,
            heading,
            body,
            strong,
            small_text,
            number,
            count
        };

        enum class tone : std::size_t
        {
            text,
            secondary,
            tertiary,
            accent,
            on_accent,
            good,
            attention,
            problem,
            background,
            card,
            border,
            control,
            control_hover,
            control_pressed,
            track,
            count
        };

        constexpr D2D1_COLOR_F rgb(unsigned int value)
        {
            return D2D1_COLOR_F{static_cast<float>((value >> 16) & 0xFF) / 255.0F,
                                static_cast<float>((value >> 8) & 0xFF) / 255.0F,
                                static_cast<float>(value & 0xFF) / 255.0F, 1.0F};
        }

        using palette = std::array<D2D1_COLOR_F, static_cast<std::size_t>(tone::count)>;

        // in the order of `tone`; the colours of Windows 11's own dark and light apps
        constexpr palette dark_palette{rgb(0xFFFFFF), rgb(0xC8C8C8), rgb(0x9A9A9A), rgb(0x60CDFF), rgb(0x00344D),
                                       rgb(0x6CCB5F), rgb(0xFCE100), rgb(0xFF99A4), rgb(0x202020), rgb(0x2B2B2B),
                                       rgb(0x3A3A3A), rgb(0x373737), rgb(0x424242), rgb(0x303030), rgb(0x454545)};
        constexpr palette light_palette{rgb(0x1A1A1A), rgb(0x5E5E5E), rgb(0x8A8A8A), rgb(0x005FB8), rgb(0xFFFFFF),
                                        rgb(0x0F7B0F), rgb(0x9D5D00), rgb(0xC42B1C), rgb(0xF3F3F3), rgb(0xFFFFFF),
                                        rgb(0xE5E5E5), rgb(0xFBFBFB), rgb(0xF0F0F0), rgb(0xE8E8E8), rgb(0xE0E0E0)};

        enum class action_kind
        {
            none,
            page,
            link,
            check_updates,
            automatic_updates,
            copy_report,
            measure_again,
            repair,         // number: the report_action of a row of "This PC"
            install_update, // fetch the newest release and put it in place
            apply_update    // put the release that is ready in place now
        };

        // a place the pointer or the keyboard can use
        struct hot_area
        {
            D2D1_RECT_F rect{};
            bool scrolls = false; // lies on the page (moves with it) and not in the fixed top part
            action_kind action = action_kind::none;
            int number = 0;      // page
            std::wstring target; // link: an https address, or a file or folder of this PC
        };

        enum class visual_kind
        {
            text,
            card,
            line,
            dot,
            ring,
            bar,
            button,
            toggle,
            link,
            tab
        };

        struct visual
        {
            visual_kind kind = visual_kind::text;
            D2D1_RECT_F rect{};
            ComPtr<IDWriteTextLayout> layout;
            tone color = tone::text;
            float share = 0.0F;    // ring, bar: 0..1; toggle: 1 = on
            int hot = -1;          // its hot_area
            bool selected = false; // tab: the open page; button: the one that stands out
            bool enabled = true;
        };

        [[nodiscard]] bool contains(D2D1_RECT_F const &rect, float x, float y)
        {
            return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
        }

        [[nodiscard]] D2D1_RECT_F moved(D2D1_RECT_F rect, float dx, float dy)
        {
            return D2D1_RECT_F{rect.left + dx, rect.top + dy, rect.right + dx, rect.bottom + dy};
        }

        [[nodiscard]] D2D1_RECT_F inflated(D2D1_RECT_F rect, float by)
        {
            return D2D1_RECT_F{rect.left - by, rect.top - by, rect.right + by, rect.bottom + by};
        }

        [[nodiscard]] bool windows_uses_dark_apps()
        {
            DWORD light = 1;
            DWORD size = sizeof(light);
            RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                         L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
            return light == 0;
        }

        // the user's own short date and time, in local time
        [[nodiscard]] std::wstring local_time_text(std::chrono::system_clock::time_point time)
        {
            using file_ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
            constexpr std::int64_t ticks_1601_to_1970 = 116'444'736'000'000'000LL;
            ULARGE_INTEGER ticks{};
            ticks.QuadPart = static_cast<ULONGLONG>(
                std::chrono::duration_cast<file_ticks>(time.time_since_epoch()).count() + ticks_1601_to_1970);
            FILETIME const stamp{ticks.LowPart, ticks.HighPart};
            SYSTEMTIME universal{};
            SYSTEMTIME local{};
            wchar_t day[64]{};
            wchar_t clock[64]{};
            if (FileTimeToSystemTime(&stamp, &universal) == FALSE ||
                SystemTimeToTzSpecificLocalTime(nullptr, &universal, &local) == FALSE ||
                GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, day, 64, nullptr) == 0 ||
                GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, clock, 64) == 0)
            {
                return {};
            }
            return std::format(L"{} {}", day, clock);
        }

        [[nodiscard]] bool copy_to_clipboard(HWND owner, std::wstring const &text)
        {
            if (OpenClipboard(owner) == FALSE)
            {
                return false;
            }
            EmptyClipboard();
            SIZE_T const bytes = (text.size() + 1) * sizeof(wchar_t);
            bool done = false;
            if (HGLOBAL const memory = GlobalAlloc(GMEM_MOVEABLE, bytes); memory != nullptr)
            {
                if (void *const place = GlobalLock(memory); place != nullptr)
                {
                    std::memcpy(place, text.c_str(), bytes);
                    GlobalUnlock(memory);
                    done = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
                }
                if (!done)
                {
                    GlobalFree(memory); // the clipboard owns it only once it took it
                }
            }
            CloseClipboard();
            return done;
        }

        void bring_to_front(HWND window, bool topmost)
        {
            SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            if (!topmost)
            {
                SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            }
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

        [[nodiscard]] std::wstring widened(std::string_view text)
        {
            return utf8_to_utf16(text);
        }
    }

    struct about_window::state
    {
        about_identity identity;
        about_events events;

        // ---- shared with the callers
        std::mutex show_mutex; // one show() or close at a time
        std::thread ui_thread; // guarded by show_mutex
        std::atomic<HWND> window{nullptr};
        std::mutex model_mutex;
        update_state pending_update;               // guarded by model_mutex
        install_state pending_install;             // guarded by model_mutex
        std::optional<system_facts> pending_facts; // guarded by model_mutex

        // ---- the window's thread only
        update_state update;
        install_state install;
        std::optional<system_facts> facts;
        std::optional<system_report> report;
        about_page page = about_page::overview;
        bool measuring = true; // facts were asked for and have not arrived
        // when the window last asked for the PC to be measured (GetTickCount64)
        ULONGLONG measured_at = 0;
        bool copied = false;   // "Copy report" just did
        bool dark = false;
        float dpi = 96.0F;
        int pixel_width = 0;
        int pixel_height = 0;

        ComPtr<ID2D1Factory> d2d_factory;
        ComPtr<IDWriteFactory> dwrite_factory;
        ComPtr<ID2D1DCRenderTarget> target;
        ComPtr<ID2D1SolidColorBrush> brush;
        ComPtr<ID2D1StrokeStyle> round_stroke;
        std::array<ComPtr<IDWriteTextFormat>, static_cast<std::size_t>(text_style::count)> formats;
        HDC memory_dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous_bitmap = nullptr;
        int surface_width = 0;
        int surface_height = 0;
        HICON icon = nullptr;
        int icon_pixels = 0;

        std::vector<visual> chrome;  // the fixed top part: name, version, pages
        std::vector<visual> content; // the open page
        std::vector<hot_area> hot;   // of both, the top part's first
        D2D1_RECT_F icon_place{};
        float chrome_height = 0.0F;
        float content_height = 0.0F;
        float scroll = 0.0F;
        int hovered = -1;
        int pressed = -1;
        int focused = -1;
        bool focus_visible = false; // the keyboard moved the focus: show where it is
        bool tracking_leave = false;
        bool dragging_thumb = false;
        float drag_grip = 0.0F; // where in the thumb the pointer holds it
        float pointer_x = -1.0F;
        float pointer_y = -1.0F;

        // ---- drawing resources
        [[nodiscard]] status create_resources();
        void release_resources();
        void ensure_surface();
        void release_surface();
        [[nodiscard]] D2D1_COLOR_F color(tone which) const;

        // ---- layout
        [[nodiscard]] float client_width() const;
        [[nodiscard]] float client_height() const;
        [[nodiscard]] float viewport_height() const;
        [[nodiscard]] float largest_scroll() const;
        [[nodiscard]] ComPtr<IDWriteTextLayout> make_layout(std::wstring_view text, text_style style, float width,
                                                            bool wrap = true, float height = 0.0F) const;
        [[nodiscard]] static D2D1_SIZE_F measured(IDWriteTextLayout *layout);
        float add_text(std::vector<visual> &list, std::wstring_view text, text_style style, tone color_of_text,
                       float x, float y, float width);
        int add_hot(D2D1_RECT_F rect, bool scrolls, action_kind action, int number = 0,
                    std::wstring target_of_link = {});
        void add_button(std::wstring_view label, float &x, float &y, float left, float right, action_kind action,
                        bool stands_out, bool enabled, std::wstring target_of_link = {}, int number = 0);
        float add_link(std::wstring_view label, float x, float y, float width, action_kind action, int number,
                       std::wstring target_of_link);
        void add_line(float x, float y, float width);
        [[nodiscard]] std::size_t begin_card();
        void end_card(std::size_t card, float x, float top, float width, float bottom);
        void relayout();
        void layout_chrome(float x, float width);
        void layout_overview(float x, float width);
        void layout_this_pc(float x, float width);
        void layout_components(float x, float width);

        // ---- behaviour
        void take_model();
        void measure_again();
        void set_page(about_page next);
        void scroll_to(float position);
        void reveal(hot_area const &area);
        [[nodiscard]] int hit(float x, float y) const;
        [[nodiscard]] D2D1_RECT_F thumb() const;
        void activate(int index);
        void open_target(std::wstring const &link_target) const;
        void copy_report();
        void move_focus(bool backward);
        void apply_dark_frame() const;
        void redraw() const;
        void draw_list(std::vector<visual> const &list, float dy, float top, float bottom);
        void paint(HDC window_dc);
        void run(about_page first_page, bool topmost, std::promise<status> &ready);
        LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);

        static LRESULT CALLBACK procedure(HWND handle_window, UINT message, WPARAM wparam, LPARAM lparam);
    };

    // ---- drawing resources ---------------------------------------------------

    status about_window::state::create_resources()
    {
        if (HRESULT const result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                                                     reinterpret_cast<void **>(d2d_factory.GetAddressOf()));
            FAILED(result))
        {
            return std::unexpected(make_hresult_failure("D2D1CreateFactory", result));
        }
        if (HRESULT const result = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                       reinterpret_cast<IUnknown **>(dwrite_factory.GetAddressOf()));
            FAILED(result))
        {
            return std::unexpected(make_hresult_failure("DWriteCreateFactory", result));
        }
        struct format_spec
        {
            text_style style;
            DWRITE_FONT_WEIGHT weight;
            float size;
        };
        constexpr std::array specs{format_spec{text_style::title, DWRITE_FONT_WEIGHT_SEMI_BOLD, 26.0F},
                                   format_spec{text_style::heading, DWRITE_FONT_WEIGHT_SEMI_BOLD, 15.0F},
                                   format_spec{text_style::body, DWRITE_FONT_WEIGHT_NORMAL, 14.0F},
                                   format_spec{text_style::strong, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0F},
                                   format_spec{text_style::small_text, DWRITE_FONT_WEIGHT_NORMAL, 12.5F},
                                   format_spec{text_style::number, DWRITE_FONT_WEIGHT_SEMI_BOLD, 22.0F}};
        for (format_spec const &spec : specs)
        {
            if (HRESULT const result = dwrite_factory->CreateTextFormat(
                    L"Segoe UI", nullptr, spec.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, spec.size,
                    ui_locale_name().data(), formats[static_cast<std::size_t>(spec.style)].GetAddressOf());
                FAILED(result))
            {
                return std::unexpected(make_hresult_failure("IDWriteFactory::CreateTextFormat", result));
            }
        }
        // software rasterizer: this window must not compete with the video for the GPU
        D2D1_RENDER_TARGET_PROPERTIES properties{};
        properties.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
        properties.pixelFormat = D2D1_PIXEL_FORMAT{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE};
        properties.usage = D2D1_RENDER_TARGET_USAGE_NONE;
        properties.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
        if (HRESULT const result = d2d_factory->CreateDCRenderTarget(&properties, target.GetAddressOf()); FAILED(result))
        {
            return std::unexpected(make_hresult_failure("ID2D1Factory::CreateDCRenderTarget", result));
        }
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        if (HRESULT const result = target->CreateSolidColorBrush(rgb(0xFFFFFF), brush.GetAddressOf()); FAILED(result))
        {
            return std::unexpected(make_hresult_failure("ID2D1RenderTarget::CreateSolidColorBrush", result));
        }
        D2D1_STROKE_STYLE_PROPERTIES stroke{};
        stroke.startCap = D2D1_CAP_STYLE_ROUND;
        stroke.endCap = D2D1_CAP_STYLE_ROUND;
        stroke.dashCap = D2D1_CAP_STYLE_ROUND;
        stroke.lineJoin = D2D1_LINE_JOIN_ROUND;
        stroke.miterLimit = 10.0F;
        stroke.dashStyle = D2D1_DASH_STYLE_SOLID;
        // (without it the score ring has flat ends: it still shows the score)
        d2d_factory->CreateStrokeStyle(&stroke, nullptr, 0, round_stroke.GetAddressOf());
        return {};
    }

    void about_window::state::release_surface()
    {
        if (memory_dc != nullptr && previous_bitmap != nullptr)
        {
            SelectObject(memory_dc, previous_bitmap);
        }
        if (bitmap != nullptr)
        {
            DeleteObject(bitmap);
        }
        if (memory_dc != nullptr)
        {
            DeleteDC(memory_dc);
        }
        memory_dc = nullptr;
        bitmap = nullptr;
        previous_bitmap = nullptr;
        surface_width = 0;
        surface_height = 0;
    }

    void about_window::state::release_resources()
    {
        chrome.clear();
        content.clear();
        hot.clear();
        for (ComPtr<IDWriteTextFormat> &format : formats)
        {
            format.Reset();
        }
        round_stroke.Reset();
        brush.Reset();
        target.Reset();
        dwrite_factory.Reset();
        d2d_factory.Reset();
        release_surface();
        if (icon != nullptr)
        {
            DestroyIcon(icon);
            icon = nullptr;
            icon_pixels = 0;
        }
    }

    void about_window::state::ensure_surface()
    {
        if (memory_dc != nullptr && surface_width == pixel_width && surface_height == pixel_height)
        {
            return;
        }
        release_surface();
        if (pixel_width <= 0 || pixel_height <= 0)
        {
            return;
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = pixel_width;
        info.bmiHeader.biHeight = -pixel_height; // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        memory_dc = CreateCompatibleDC(nullptr);
        void *bits = nullptr;
        bitmap = memory_dc != nullptr ? CreateDIBSection(memory_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (bitmap == nullptr)
        {
            log::warning("about window: {}", make_win32_failure("CreateDIBSection", GetLastError()).describe());
            release_surface();
            return;
        }
        previous_bitmap = SelectObject(memory_dc, bitmap);
        surface_width = pixel_width;
        surface_height = pixel_height;
    }

    D2D1_COLOR_F about_window::state::color(tone which) const
    {
        return (dark ? dark_palette : light_palette)[static_cast<std::size_t>(which)];
    }

    // ---- layout ---------------------------------------------------------------

    float about_window::state::client_width() const
    {
        return static_cast<float>(pixel_width) * 96.0F / dpi;
    }

    float about_window::state::client_height() const
    {
        return static_cast<float>(pixel_height) * 96.0F / dpi;
    }

    float about_window::state::viewport_height() const
    {
        return std::max(client_height() - chrome_height, 0.0F);
    }

    float about_window::state::largest_scroll() const
    {
        return std::max(content_height - viewport_height(), 0.0F);
    }

    ComPtr<IDWriteTextLayout> about_window::state::make_layout(std::wstring_view text, text_style style, float width,
                                                               bool wrap, float height) const
    {
        ComPtr<IDWriteTextLayout> layout;
        IDWriteTextFormat *const format = formats[static_cast<std::size_t>(style)].Get();
        if (format == nullptr ||
            FAILED(dwrite_factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format,
                                                    std::max(width, 1.0F), height > 0.0F ? height : 100000.0F,
                                                    layout.GetAddressOf())))
        {
            return layout;
        }
        // a word that is wider than the line (an address, a version) is broken
        layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_EMERGENCY_BREAK : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (height > 0.0F)
        {
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        return layout;
    }

    D2D1_SIZE_F about_window::state::measured(IDWriteTextLayout *layout)
    {
        DWRITE_TEXT_METRICS metrics{};
        if (layout == nullptr || FAILED(layout->GetMetrics(&metrics)))
        {
            return D2D1_SIZE_F{0.0F, 0.0F};
        }
        return D2D1_SIZE_F{std::ceil(metrics.widthIncludingTrailingWhitespace), std::ceil(metrics.height)};
    }

    // returns the height the text takes
    float about_window::state::add_text(std::vector<visual> &list, std::wstring_view text, text_style style,
                                        tone color_of_text, float x, float y, float width)
    {
        visual item;
        item.kind = visual_kind::text;
        item.layout = make_layout(text, style, width);
        item.color = color_of_text;
        D2D1_SIZE_F const size = measured(item.layout.Get());
        item.rect = D2D1_RECT_F{x, y, x + width, y + size.height};
        list.push_back(std::move(item));
        return size.height;
    }

    int about_window::state::add_hot(D2D1_RECT_F rect, bool scrolls, action_kind action, int number,
                                     std::wstring target_of_link)
    {
        hot_area area;
        area.rect = rect;
        area.scrolls = scrolls;
        area.action = action;
        area.number = number;
        area.target = std::move(target_of_link);
        hot.push_back(std::move(area));
        return static_cast<int>(hot.size()) - 1;
    }

    // A button on the page, in a row of buttons that starts at `left`: the
    // next free place is (x, y); a button that no longer fits before `right`
    // starts a new row.  Both are moved on.
    void about_window::state::add_button(std::wstring_view label, float &x, float &y, float left, float right,
                                         action_kind action, bool stands_out, bool enabled,
                                         std::wstring target_of_link, int number)
    {
        float const text_width = measured(make_layout(label, text_style::body, 600.0F, false).Get()).width;
        float const width = std::max(text_width + 32.0F, 96.0F);
        if (x > left && x + width > right)
        {
            x = left;
            y += button_height + 8.0F;
        }
        visual item;
        item.kind = visual_kind::button;
        item.rect = D2D1_RECT_F{x, y, x + width, y + button_height};
        item.layout = make_layout(label, text_style::body, width, false, button_height);
        item.selected = stands_out;
        item.enabled = enabled;
        if (enabled)
        {
            item.hot = add_hot(item.rect, true, action, number, std::move(target_of_link));
        }
        content.push_back(std::move(item));
        x += width + 8.0F;
    }

    // a link on the page; returns its height
    float about_window::state::add_link(std::wstring_view label, float x, float y, float width, action_kind action,
                                        int number, std::wstring target_of_link)
    {
        visual item;
        item.kind = visual_kind::link;
        item.layout = make_layout(label, text_style::body, width);
        item.color = tone::accent;
        D2D1_SIZE_F const size = measured(item.layout.Get());
        item.rect = D2D1_RECT_F{x, y, x + std::min(size.width, width), y + size.height};
        item.hot = add_hot(item.rect, true, action, number, std::move(target_of_link));
        content.push_back(std::move(item));
        return size.height;
    }

    void about_window::state::add_line(float x, float y, float width)
    {
        visual item;
        item.kind = visual_kind::line;
        item.rect = D2D1_RECT_F{x, y, x + width, y + 1.0F};
        item.color = tone::border;
        content.push_back(std::move(item));
    }

    // A card is drawn behind what stands on it, but how tall it is shows only
    // when that is laid out: its place in the list is taken first.
    std::size_t about_window::state::begin_card()
    {
        visual item;
        item.kind = visual_kind::card;
        content.push_back(std::move(item));
        return content.size() - 1;
    }

    void about_window::state::end_card(std::size_t card, float x, float top, float width, float bottom)
    {
        content[card].rect = D2D1_RECT_F{x, top, x + width, bottom};
    }

    void about_window::state::layout_chrome(float x, float width)
    {
        float y = 20.0F;
        icon_place = D2D1_RECT_F{x, y, x + icon_size, y + icon_size};
        float const text_x = x + icon_size + 16.0F;
        float const text_width = std::max(width - icon_size - 16.0F, 80.0F);
        float const title_height =
            add_text(chrome, L"" UXPLAY_PRODUCT_NAME, text_style::title, tone::text, text_x, y - 4.0F, text_width);
        std::array<std::wstring_view, 2> const version_words{identity.version, identity.release_date};
        float const version_top = y - 4.0F + title_height + 2.0F;
        float const version_height = add_text(chrome, ui_format(ui_text::about_version, version_words),
                                              text_style::body, tone::secondary, text_x, version_top, text_width);
        // (in a narrow window, or a language with long words, the version line takes two lines)
        y = std::max(y + icon_size, version_top + version_height) + 12.0F;
        y += add_text(chrome, ui_string(ui_text::about_tagline), text_style::body, tone::secondary, x, y, width) + 12.0F;

        constexpr std::array tabs{ui_text::about_tab_overview, ui_text::about_tab_this_pc,
                                  ui_text::about_tab_components};
        float tab_x = x - 12.0F;
        for (std::size_t index = 0; index < tabs.size(); ++index)
        {
            std::wstring_view const label = ui_string(tabs[index]);
            float const label_width = measured(make_layout(label, text_style::strong, 400.0F, false).Get()).width;
            visual item;
            item.kind = visual_kind::tab;
            item.rect = D2D1_RECT_F{tab_x, y, tab_x + label_width + 24.0F, y + tab_height};
            item.layout = make_layout(label, text_style::strong, label_width + 24.0F, false, tab_height);
            item.selected = static_cast<int>(index) == static_cast<int>(page);
            item.hot = add_hot(item.rect, false, action_kind::page, static_cast<int>(index));
            tab_x = item.rect.right + 4.0F;
            chrome.push_back(std::move(item));
        }
        y += tab_height;
        visual rule;
        rule.kind = visual_kind::line;
        rule.rect = D2D1_RECT_F{0.0F, y, client_width(), y + 1.0F};
        rule.color = tone::border;
        chrome.push_back(std::move(rule));
        chrome_height = y + 1.0F;
    }

    void about_window::state::layout_overview(float x, float width)
    {
        float const inner_x = x + card_padding;
        float const inner_width = width - 2.0F * card_padding;
        float y = 20.0F;

        // ---- updates
        {
            std::size_t const card = begin_card();
            float line = y + 18.0F;
            line += add_text(content, ui_string(ui_text::about_updates), text_style::heading, tone::text, inner_x, line,
                             inner_width) + 10.0F;
            std::wstring status_text;
            std::wstring detail;
            tone mark = tone::tertiary;
            std::wstring const running = widened(running_version().text());
            std::wstring newest_words;
            if (update.newest)
            {
                newest_words = widened(update.newest->version.text());
                if (!update.newest->date.empty())
                {
                    newest_words += std::format(L" ({})", widened(update.newest->date));
                }
            }
            switch (update.phase)
            {
            case update_phase::unknown:
                status_text = ui_string(ui_text::update_not_checked);
                break;
            case update_phase::checking:
                status_text = ui_string(ui_text::update_checking);
                break;
            case update_phase::up_to_date:
                status_text = ui_string(ui_text::update_up_to_date);
                mark = tone::good;
                if (update.newest)
                {
                    std::array<std::wstring_view, 1> const words{newest_words};
                    detail = ui_format(ui_text::update_newest_is, words);
                }
                break;
            case update_phase::newer_available:
                mark = tone::accent;
                if (update.newest)
                {
                    std::wstring const version = widened(update.newest->version.text());
                    std::array<std::wstring_view, 1> const words{version};
                    status_text = ui_format(ui_text::update_available, words);
                    if (!update.newest->date.empty())
                    {
                        std::wstring const date = widened(update.newest->date);
                        std::array<std::wstring_view, 2> const released{date, running};
                        detail = ui_format(ui_text::update_released, released);
                    }
                }
                break;
            case update_phase::failed:
                status_text = ui_string(ui_text::update_failed);
                detail = ui_string(ui_text::update_failed_text);
                mark = tone::problem;
                break;
            }
            // an installation that is under way, or how it ended, comes
            // before what the check says
            std::wstring const installing = widened(install.version.text());
            std::array<std::wstring_view, 1> const installing_words{installing};
            switch (install.phase)
            {
            case install_phase::idle:
                break;
            case install_phase::downloading:
                status_text = ui_format(ui_text::update_downloading, installing_words);
                detail = install.expected != 0
                             ? std::format(L"{:.1f} / {:.1f} MB", static_cast<double>(install.received) / 1e6,
                                           static_cast<double>(install.expected) / 1e6)
                             : std::format(L"{:.1f} MB", static_cast<double>(install.received) / 1e6);
                mark = tone::accent;
                break;
            case install_phase::checking:
                status_text = ui_format(ui_text::update_unpacking, installing_words);
                detail.clear();
                mark = tone::accent;
                break;
            case install_phase::ready:
                status_text = ui_format(ui_text::update_ready, installing_words);
                detail = ui_string(ui_text::update_ready_text);
                mark = tone::good;
                break;
            case install_phase::installed:
                status_text = ui_string(ui_text::update_restarting);
                detail.clear();
                mark = tone::good;
                break;
            case install_phase::failed:
                status_text = ui_string(ui_text::update_install_failed);
                mark = tone::problem;
                switch (install.problem)
                {
                case install_problem::no_signature:
                    detail = ui_string(ui_text::update_problem_no_signature);
                    break;
                case install_problem::download:
                    detail = ui_string(ui_text::update_problem_download);
                    break;
                case install_problem::not_genuine:
                    detail = ui_string(ui_text::update_problem_not_genuine);
                    break;
                case install_problem::package:
                    detail = ui_string(ui_text::update_problem_package);
                    break;
                case install_problem::folder:
                    detail = ui_string(ui_text::update_problem_folder);
                    break;
                case install_problem::none:
                case install_problem::other:
                    detail = ui_string(ui_text::update_problem_other);
                    break;
                }
                break;
            }
            bool const install_busy = install.phase == install_phase::downloading ||
                                      install.phase == install_phase::checking ||
                                      install.phase == install_phase::installed;
            visual dot;
            dot.kind = visual_kind::dot;
            dot.rect = D2D1_RECT_F{inner_x, line + 5.0F, inner_x + dot_size, line + 5.0F + dot_size};
            dot.color = mark;
            content.push_back(std::move(dot));
            float const text_x = inner_x + 20.0F;
            float const text_width = inner_width - 20.0F;
            line += add_text(content, status_text, text_style::strong, tone::text, text_x, line, text_width) + 2.0F;
            if (!detail.empty())
            {
                line += add_text(content, detail, text_style::body, tone::secondary, text_x, line, text_width) + 2.0F;
            }
            if (update.checked)
            {
                std::wstring const when = local_time_text(*update.checked);
                std::array<std::wstring_view, 1> const words{when};
                line += add_text(content, ui_format(ui_text::update_last_checked, words), text_style::small_text,
                                 tone::tertiary, text_x, line, text_width);
            }
            line += 14.0F;
            float button_x = text_x;
            float const buttons_right = inner_x + inner_width;
            if (install.phase == install_phase::ready)
            {
                add_button(ui_string(ui_text::update_restart_now), button_x, line, text_x, buttons_right,
                           action_kind::apply_update, true, true);
            }
            if (update.phase == update_phase::newer_available && update.newest)
            {
                // one click installs it; the package itself stays on offer for
                // whoever would rather unpack it by hand (and for when the
                // program cannot install it)
                if (install.phase != install_phase::ready)
                {
                    add_button(ui_string(install.phase == install_phase::failed ? ui_text::update_retry
                                                                                : ui_text::update_install),
                               button_x, line, text_x, buttons_right, action_kind::install_update,
                               install.phase != install_phase::failed, !install_busy);
                }
                add_button(ui_string(ui_text::update_download), button_x, line, text_x, buttons_right,
                           action_kind::link, install.phase == install_phase::failed, true,
                           widened(update.newest->package_url));
                add_button(ui_string(ui_text::update_whats_new), button_x, line, text_x, buttons_right,
                           action_kind::link, false, true, widened(update.newest->page_url));
            }
            add_button(ui_string(ui_text::update_check_now), button_x, line, text_x, buttons_right,
                       action_kind::check_updates, false, update.phase != update_phase::checking && !install_busy);
            line += button_height + 16.0F;
            add_line(inner_x, line, inner_width);
            line += 14.0F;

            visual toggle;
            toggle.kind = visual_kind::toggle;
            toggle.rect = D2D1_RECT_F{inner_x + inner_width - toggle_width, line, inner_x + inner_width,
                                      line + toggle_height};
            toggle.share = update.automatic ? 1.0F : 0.0F;
            toggle.hot = add_hot(toggle.rect, true, action_kind::automatic_updates);
            content.push_back(std::move(toggle));
            float const label_width = inner_width - toggle_width - 20.0F;
            line += add_text(content, ui_string(ui_text::update_automatic), text_style::body, tone::text, inner_x, line,
                             label_width) + 4.0F;
            line += add_text(content, ui_string(ui_text::update_automatic_note), text_style::small_text,
                             tone::secondary, inner_x, line, label_width);
            line += 18.0F;
            end_card(card, x, y, width, line);
            y = line + card_gap;
        }

        // ---- this PC
        {
            std::size_t const card = begin_card();
            float const top = y + 18.0F;
            constexpr float ring_size = 64.0F;
            visual ring;
            ring.kind = visual_kind::ring;
            ring.rect = D2D1_RECT_F{inner_x, top, inner_x + ring_size, top + ring_size};
            std::optional<int> const score = report ? report->score : std::nullopt;
            ring.share = score ? static_cast<float>(*score) / 100.0F : 0.0F;
            ring.color = !score                 ? tone::track
                         : *score >= good_score ? tone::good
                         : *score >= fair_score ? tone::attention
                                                : tone::problem;
            content.push_back(std::move(ring));
            if (score)
            {
                visual number;
                number.kind = visual_kind::text;
                number.rect = D2D1_RECT_F{inner_x, top, inner_x + ring_size, top + ring_size};
                number.layout = make_layout(std::to_wstring(*score), text_style::number, ring_size, false, ring_size);
                content.push_back(std::move(number));
            }
            float const text_x = inner_x + ring_size + 18.0F;
            float const text_width = inner_width - ring_size - 18.0F;
            float line = top + 2.0F;
            line += add_text(content, ui_string(ui_text::score_title), text_style::strong, tone::text, text_x, line,
                             text_width) + 4.0F;
            ui_text const verdict = !score                     ? ui_text::score_measuring
                                    : *score >= excellent_score ? ui_text::score_excellent
                                    : *score >= good_score      ? ui_text::score_good
                                    : *score >= fair_score      ? ui_text::score_fair
                                                                : ui_text::score_poor;
            line += add_text(content, ui_string(verdict), text_style::body, tone::secondary, text_x, line, text_width) +
                    6.0F;
            line += add_link(ui_string(ui_text::score_details), text_x, line, text_width, action_kind::page,
                             static_cast<int>(about_page::this_pc), {});
            float const bottom = std::max(top + ring_size, line) + 18.0F;
            end_card(card, x, y, width, bottom);
            y = bottom + card_gap;
        }

        // ---- links
        {
            std::size_t const card = begin_card();
            float line = y + 18.0F;
            line += add_text(content, ui_string(ui_text::about_links), text_style::heading, tone::text, inner_x, line,
                             inner_width) + 10.0F;
            struct link_row
            {
                ui_text label;
                std::wstring target;
            };
            std::vector<link_row> links{{ui_text::link_project, identity.project_url},
                                        {ui_text::link_releases, identity.project_url + L"/releases"},
                                        {ui_text::link_report_problem, identity.project_url + L"/issues"}};
            if (!identity.user_guide.empty())
            {
                links.push_back({ui_text::link_user_guide, identity.user_guide.wstring()});
            }
            if (!identity.licences.empty())
            {
                links.push_back({ui_text::link_licences, identity.licences.wstring()});
            }
            if (!identity.log_file.empty())
            {
                links.push_back({ui_text::open_log, identity.log_file.wstring()});
            }
            // two columns, filled row by row
            float const column_width = (inner_width - 16.0F) / 2.0F;
            for (std::size_t index = 0; index < links.size(); index += 2)
            {
                float height = add_link(ui_string(links[index].label), inner_x, line, column_width, action_kind::link,
                                        0, links[index].target);
                if (index + 1 < links.size())
                {
                    height = std::max(height, add_link(ui_string(links[index + 1].label),
                                                       inner_x + column_width + 16.0F, line, column_width,
                                                       action_kind::link, 0, links[index + 1].target));
                }
                line += height + 10.0F;
            }
            line += 8.0F;
            end_card(card, x, y, width, line);
            y = line + card_gap;
        }

        std::array<std::wstring_view, 1> const core{identity.core_version};
        y += add_text(content, ui_format(ui_text::about_licence, core), text_style::small_text, tone::secondary,
                      x + 4.0F, y, width - 8.0F) + 6.0F;
        y += add_text(content, ui_string(ui_text::about_trademarks), text_style::small_text, tone::tertiary, x + 4.0F,
                      y, width - 8.0F);
        content_height = y + 24.0F;
    }

    void about_window::state::layout_this_pc(float x, float width)
    {
        float const inner_x = x + card_padding;
        float const inner_width = width - 2.0F * card_padding;
        float y = 20.0F;
        constexpr std::array group_names{ui_text::group_system, ui_text::group_hardware, ui_text::group_drivers,
                                         ui_text::group_software};

        // ---- the scores
        {
            std::size_t const card = begin_card();
            float const top = y + 18.0F;
            constexpr float ring_size = 84.0F;
            std::optional<int> const score = report ? report->score : std::nullopt;
            visual ring;
            ring.kind = visual_kind::ring;
            ring.rect = D2D1_RECT_F{inner_x, top, inner_x + ring_size, top + ring_size};
            ring.share = score ? static_cast<float>(*score) / 100.0F : 0.0F;
            ring.color = !score                 ? tone::track
                         : *score >= good_score ? tone::good
                         : *score >= fair_score ? tone::attention
                                                : tone::problem;
            content.push_back(std::move(ring));
            if (score)
            {
                visual number;
                number.kind = visual_kind::text;
                number.rect = D2D1_RECT_F{inner_x, top, inner_x + ring_size, top + ring_size};
                number.layout = make_layout(std::to_wstring(*score), text_style::number, ring_size, false, ring_size);
                content.push_back(std::move(number));
            }
            float const text_x = inner_x + ring_size + 22.0F;
            float const text_width = inner_width - ring_size - 22.0F;
            float line = top;
            line += add_text(content, ui_string(ui_text::score_title), text_style::strong, tone::text, text_x, line,
                             text_width) + 4.0F;
            ui_text const verdict = !score || measuring         ? ui_text::score_measuring
                                    : *score >= excellent_score ? ui_text::score_excellent
                                    : *score >= good_score      ? ui_text::score_good
                                    : *score >= fair_score      ? ui_text::score_fair
                                                                : ui_text::score_poor;
            line += add_text(content, ui_string(verdict), text_style::body, tone::secondary, text_x, line, text_width) +
                    12.0F;
            if (report)
            {
                // per group: name, bar, score
                float const name_width = std::min(150.0F, text_width * 0.36F);
                constexpr float number_width = 40.0F;
                float const bar_width = std::max(text_width - name_width - number_width - 20.0F, 40.0F);
                for (std::size_t group = 0; group < group_names.size(); ++group)
                {
                    std::optional<int> const group_score = report->group_scores[group];
                    float const height = add_text(content, ui_string(group_names[group]), text_style::small_text,
                                                  tone::secondary, text_x, line, name_width);
                    visual bar;
                    bar.kind = visual_kind::bar;
                    bar.rect = D2D1_RECT_F{text_x + name_width + 10.0F, line + height / 2.0F - 3.0F,
                                           text_x + name_width + 10.0F + bar_width, line + height / 2.0F + 3.0F};
                    bar.share = group_score ? static_cast<float>(*group_score) / 100.0F : 0.0F;
                    bar.color = !group_score                 ? tone::track
                                : *group_score >= good_score ? tone::good
                                : *group_score >= fair_score ? tone::attention
                                                             : tone::problem;
                    content.push_back(std::move(bar));
                    add_text(content, group_score ? std::to_wstring(*group_score) : std::wstring{L"–"},
                             text_style::small_text, tone::text, text_x + name_width + 20.0F + bar_width, line,
                             number_width);
                    line += height + 6.0F;
                }
            }
            line = std::max(line, top + ring_size) + 14.0F;
            float button_x = inner_x;
            add_button(ui_string(copied ? ui_text::report_copied : ui_text::report_copy), button_x, line, inner_x,
                       inner_x + inner_width, action_kind::copy_report, false, facts.has_value());
            add_button(ui_string(ui_text::report_again), button_x, line, inner_x, inner_x + inner_width,
                       action_kind::measure_again, false, !measuring);
            line += button_height + 18.0F;
            end_card(card, x, y, width, line);
            y = line + card_gap;
        }

        if (!report)
        {
            content_height = y + 24.0F;
            return;
        }

        // ---- the rows, group by group
        float const label_width = std::clamp(inner_width * 0.30F, 120.0F, 210.0F);
        float const value_x = inner_x + 20.0F + label_width + 12.0F;
        float const value_width = std::max(inner_x + inner_width - value_x, 80.0F);
        for (std::size_t group = 0; group < group_names.size(); ++group)
        {
            auto const in_group = [group](report_entry const &entry)
            {
                return static_cast<std::size_t>(entry.group) == group;
            };
            if (std::ranges::none_of(report->entries, in_group))
            {
                continue;
            }
            y += 4.0F;
            y += add_text(content, ui_string(group_names[group]), text_style::heading, tone::text, x + 4.0F, y,
                          width - 8.0F) + 8.0F;
            std::size_t const card = begin_card();
            float line = y + 4.0F;
            bool first = true;
            for (report_entry const &entry : report->entries)
            {
                if (!in_group(entry))
                {
                    continue;
                }
                if (!std::exchange(first, false))
                {
                    add_line(inner_x, line, inner_width);
                }
                line += row_padding;
                visual dot;
                dot.kind = visual_kind::dot;
                dot.rect = D2D1_RECT_F{inner_x, line + 5.0F, inner_x + dot_size, line + 5.0F + dot_size};
                dot.color = entry.rating == report_rating::good        ? tone::good
                            : entry.rating == report_rating::attention ? tone::attention
                            : entry.rating == report_rating::problem   ? tone::problem
                                                                       : tone::track;
                content.push_back(std::move(dot));
                float const label_height = add_text(content, ui_string(entry.label), text_style::body, tone::secondary,
                                                    inner_x + 20.0F, line, label_width);
                float value_height =
                    add_text(content, entry.value, text_style::body, tone::text, value_x, line, value_width);
                if (!entry.advice.empty())
                {
                    value_height += 4.0F;
                    value_height += add_text(content, entry.advice, text_style::small_text,
                                             entry.rating == report_rating::problem     ? tone::problem
                                             : entry.rating == report_rating::attention ? tone::attention
                                                                                        : tone::secondary,
                                             value_x, line + value_height, value_width);
                }
                if (entry.action != report_action::none && !entry.action_label.empty())
                {
                    // what the program can do about it (not while the PC is
                    // being measured: the row may be about to change)
                    float button_x = value_x;
                    float button_y = line + value_height + 8.0F;
                    add_button(entry.action_label, button_x, button_y, value_x, value_x + value_width,
                               action_kind::repair, false, !measuring, {}, static_cast<int>(entry.action));
                    value_height = button_y + button_height - line;
                }
                line += std::max(label_height, value_height) + row_padding;
            }
            line += 4.0F;
            end_card(card, x, y, width, line);
            y = line + card_gap;
        }
        content_height = y + 8.0F;
    }

    void about_window::state::layout_components(float x, float width)
    {
        float const inner_x = x + card_padding;
        float const inner_width = width - 2.0F * card_padding;
        float y = 20.0F;
        // name, version (and revision), date, licence
        std::array<float, 4> const column_widths{inner_width * 0.36F, inner_width * 0.27F, inner_width * 0.14F,
                                                 inner_width * 0.23F};
        std::array<float, 4> column_x{};
        for (std::size_t column = 0, offset = 0; column < column_x.size(); ++column)
        {
            column_x[column] = inner_x + static_cast<float>(offset);
            offset += static_cast<std::size_t>(column_widths[column]);
        }
        constexpr float column_gap = 12.0F;

        struct section
        {
            build_input_kind kind;
            ui_text heading;
            bool with_versions; // version and date columns have something to say
        };
        constexpr std::array sections{section{build_input_kind::library, ui_text::components_libraries, true},
                                      section{build_input_kind::tool, ui_text::components_tools, true},
                                      section{build_input_kind::run_time, ui_text::components_run_time, true},
                                      section{build_input_kind::system, ui_text::components_system, false},
                                      section{build_input_kind::credit, ui_text::components_credits, false}};
        for (section const &part : sections)
        {
            auto const of_kind = [&part](build_input const &input)
            {
                return input.kind == part.kind;
            };
            if (std::ranges::none_of(build_inputs(), of_kind))
            {
                continue;
            }
            y += 4.0F;
            y += add_text(content, ui_string(part.heading), text_style::heading, tone::text, x + 4.0F, y, width - 8.0F) +
                 8.0F;
            std::size_t const card = begin_card();
            float line = y + 12.0F;
            if (part.with_versions)
            {
                constexpr std::array titles{ui_text::column_component, ui_text::column_version, ui_text::column_date,
                                            ui_text::column_licence};
                float height = 0.0F;
                for (std::size_t column = 0; column < titles.size(); ++column)
                {
                    height = std::max(height, add_text(content, ui_string(titles[column]), text_style::small_text,
                                                       tone::tertiary, column_x[column], line,
                                                       column_widths[column] - column_gap));
                }
                line += height + 8.0F;
                add_line(inner_x, line, inner_width);
            }
            bool first = !part.with_versions;
            for (build_input const &input : build_inputs())
            {
                if (!of_kind(input))
                {
                    continue;
                }
                if (!std::exchange(first, false))
                {
                    add_line(inner_x, line, inner_width);
                }
                line += row_padding - 2.0F;
                // without the two middle columns the name may use their room
                float const name_width = (part.with_versions ? column_widths[0]
                                                             : column_widths[0] + column_widths[1] + column_widths[2]) -
                                         column_gap;
                float height = input.homepage.empty()
                                   ? add_text(content, widened(input.name), text_style::body, tone::text, column_x[0],
                                              line, name_width)
                                   : add_link(widened(input.name), column_x[0], line, name_width, action_kind::link, 0,
                                              widened(input.homepage));
                if (part.with_versions)
                {
                    std::wstring version = widened(input.version);
                    if (!input.revision.empty())
                    {
                        if (!version.empty())
                        {
                            version += L" · ";
                        }
                        version += widened(input.revision.substr(0, 7));
                    }
                    height = std::max(height, add_text(content, version, text_style::body, tone::text, column_x[1], line,
                                                       column_widths[1] - column_gap));
                    height = std::max(height, add_text(content, widened(input.date), text_style::body, tone::secondary,
                                                       column_x[2], line, column_widths[2] - column_gap));
                }
                height = std::max(height, add_text(content, widened(input.licence), text_style::body, tone::secondary,
                                                   column_x[3], line, column_widths[3]));
                line += height + row_padding - 2.0F;
            }
            line += 6.0F;
            end_card(card, x, y, width, line);
            y = line + card_gap;
        }
        y += add_text(content, ui_string(ui_text::components_dates_note), text_style::small_text, tone::tertiary,
                      x + 4.0F, y - 6.0F, width - 8.0F) + 10.0F;

        // ---- when each version of the program was published
        y += add_text(content, ui_string(ui_text::components_releases), text_style::heading, tone::text, x + 4.0F, y,
                      width - 8.0F) + 8.0F;
        {
            std::size_t const card = begin_card();
            float line = y + 14.0F;
            constexpr std::size_t columns = 3;
            float const cell_width = inner_width / static_cast<float>(columns);
            std::span<release_record const> const releases = release_history();
            for (std::size_t index = 0; index < releases.size(); index += columns)
            {
                float height = 0.0F;
                for (std::size_t column = 0; column < columns && index + column < releases.size(); ++column)
                {
                    release_record const &release = releases[index + column];
                    float const cell_x = inner_x + cell_width * static_cast<float>(column);
                    float const version_width =
                        measured(make_layout(widened(release.version), text_style::strong, 200.0F, false).Get()).width;
                    visual version;
                    version.kind = visual_kind::text;
                    version.layout = make_layout(widened(release.version), text_style::strong, 200.0F, false);
                    version.rect = D2D1_RECT_F{cell_x, line, cell_x + version_width,
                                               line + measured(version.layout.Get()).height};
                    height = std::max(height, version.rect.bottom - version.rect.top);
                    content.push_back(std::move(version));
                    height = std::max(height, add_text(content, widened(release.date), text_style::body,
                                                       tone::secondary, cell_x + version_width + 10.0F, line,
                                                       std::max(cell_width - version_width - 18.0F, 40.0F)));
                }
                line += height + 8.0F;
            }
            line += 4.0F;
            line += add_link(ui_string(ui_text::link_releases), inner_x, line, inner_width, action_kind::link, 0,
                             identity.project_url + L"/releases");
            line += 16.0F;
            end_card(card, x, y, width, line);
            y = line + card_gap;
        }
        content_height = y + 8.0F;
    }

    void about_window::state::relayout()
    {
        // What is pressed or holds the focus is found again afterwards by what
        // it does: a page that is laid out anew while the button is down (an
        // answer arrived in that moment) must not lose the click.
        auto const remembered = [this](int index)
        {
            return index >= 0 && index < static_cast<int>(hot.size())
                       ? std::optional<hot_area>{hot[static_cast<std::size_t>(index)]}
                       : std::nullopt;
        };
        std::optional<hot_area> const was_pressed = remembered(pressed);
        std::optional<hot_area> const was_focused = remembered(focused);
        auto const found_again = [this](std::optional<hot_area> const &before)
        {
            if (before)
            {
                for (std::size_t index = 0; index < hot.size(); ++index)
                {
                    hot_area const &area = hot[index];
                    if (area.action == before->action && area.number == before->number &&
                        area.scrolls == before->scrolls && area.target == before->target)
                    {
                        return static_cast<int>(index);
                    }
                }
            }
            return -1;
        };
        chrome.clear();
        content.clear();
        hot.clear();
        pressed = -1;
        focused = -1;
        if (!dwrite_factory || pixel_width <= 0)
        {
            return;
        }
        float const width = std::min(client_width() - 2.0F * side_margin, widest_column);
        float const x = std::max((client_width() - width) / 2.0F, side_margin);
        layout_chrome(x, width);
        switch (page)
        {
        case about_page::overview:
            layout_overview(x, width);
            break;
        case about_page::this_pc:
            layout_this_pc(x, width);
            break;
        case about_page::components:
            layout_components(x, width);
            break;
        }
        scroll = std::clamp(scroll, 0.0F, largest_scroll());
        pressed = found_again(was_pressed);
        focused = found_again(was_focused);
        hovered = hit(pointer_x, pointer_y);
        redraw();
    }

    // ---- behaviour --------------------------------------------------------------

    void about_window::state::redraw() const
    {
        if (HWND const shown = window.load(); shown != nullptr)
        {
            InvalidateRect(shown, nullptr, FALSE);
        }
    }

    void about_window::state::take_model()
    {
        std::optional<system_facts> arrived;
        {
            std::scoped_lock lock{model_mutex};
            update = pending_update;
            install = pending_install;
            arrived = std::exchange(pending_facts, std::nullopt);
        }
        if (arrived)
        {
            facts = std::move(arrived);
            report = build_report(*facts, current_ui_language());
            measuring = false;
        }
        relayout();
    }

    // The PC is looked at again (the button, the return to the window).
    void about_window::state::measure_again()
    {
        measuring = true;
        measured_at = GetTickCount64();
        if (events.on_measure_again)
        {
            events.on_measure_again();
        }
        relayout();
    }

    void about_window::state::set_page(about_page next)
    {
        if (page == next)
        {
            return;
        }
        page = next;
        scroll = 0.0F;
        if (focused >= 3)
        {
            focused = -1; // (the three pages' tabs are the first places; the focus may stay on them)
        }
        relayout();
    }

    void about_window::state::scroll_to(float position)
    {
        float const clamped = std::clamp(position, 0.0F, largest_scroll());
        if (clamped != scroll)
        {
            scroll = clamped;
            hovered = hit(pointer_x, pointer_y);
            redraw();
        }
    }

    // scrolls so that a place the keyboard reached can be seen
    void about_window::state::reveal(hot_area const &area)
    {
        if (!area.scrolls)
        {
            return;
        }
        if (area.rect.top - 12.0F < scroll)
        {
            scroll_to(area.rect.top - 12.0F);
        }
        else if (area.rect.bottom + 12.0F > scroll + viewport_height())
        {
            scroll_to(area.rect.bottom + 12.0F - viewport_height());
        }
    }

    int about_window::state::hit(float x, float y) const
    {
        for (std::size_t index = 0; index < hot.size(); ++index)
        {
            hot_area const &area = hot[index];
            if (area.scrolls ? (y >= chrome_height && contains(area.rect, x, y - chrome_height + scroll))
                             : contains(area.rect, x, y))
            {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    // the scroll bar's thumb in window coordinates; empty when the page fits
    D2D1_RECT_F about_window::state::thumb() const
    {
        float const range = largest_scroll();
        float const view = viewport_height();
        if (range <= 0.0F || view <= 0.0F)
        {
            return D2D1_RECT_F{};
        }
        float const height = std::max(view * view / content_height, 32.0F);
        float const top = chrome_height + (view - height) * scroll / range;
        return D2D1_RECT_F{client_width() - 10.0F, top + 2.0F, client_width() - 4.0F, top + height - 2.0F};
    }

    void about_window::state::open_target(std::wstring const &link_target) const
    {
        // only a web address of the kind the program itself names, or something that is on this PC
        std::error_code ignored;
        bool const web = link_target.starts_with(L"https://");
        if (!web && !std::filesystem::exists(std::filesystem::path{link_target}, ignored))
        {
            log::warning("about window: a link was not opened: it is neither an https address nor a file of this PC");
            return;
        }
        auto const opened = reinterpret_cast<std::intptr_t>(
            ShellExecuteW(window.load(), L"open", link_target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (opened <= 32) // ShellExecute reports success with a value above 32
        {
            log::warning("about window: ShellExecute failed with code {} for a {}", opened,
                         web ? "web address" : "file of this PC");
        }
    }

    void about_window::state::copy_report()
    {
        if (!facts)
        {
            return;
        }
        if (!copy_to_clipboard(window.load(), report_text(*facts)))
        {
            log::warning("about window: the report was not copied: {}",
                         make_win32_failure("clipboard", GetLastError()).describe());
            return;
        }
        copied = true;
        SetTimer(window.load(), copied_timer_id, copied_feedback_ms, nullptr);
        relayout();
    }

    void about_window::state::activate(int index)
    {
        if (index < 0 || index >= static_cast<int>(hot.size()))
        {
            return;
        }
        // a copy: what follows may lay the page out anew
        hot_area const area = hot[static_cast<std::size_t>(index)];
        switch (area.action)
        {
        case action_kind::page:
            if (area.number >= 0 && area.number <= static_cast<int>(about_page::components))
            {
                set_page(static_cast<about_page>(area.number));
            }
            break;
        case action_kind::link:
            open_target(area.target);
            break;
        case action_kind::check_updates:
            if (events.on_check_for_updates)
            {
                events.on_check_for_updates();
            }
            break;
        case action_kind::automatic_updates:
            // shown at once; the checker's own state follows through set_update
            update.automatic = !update.automatic;
            {
                std::scoped_lock lock{model_mutex};
                pending_update.automatic = update.automatic;
            }
            if (events.on_automatic_updates)
            {
                events.on_automatic_updates(update.automatic);
            }
            relayout();
            break;
        case action_kind::copy_report:
            copy_report();
            break;
        case action_kind::measure_again:
            measure_again();
            break;
        case action_kind::repair:
            // the owner makes the repair and has the PC measured again; the
            // buttons wait for that
            if (events.on_report_action && area.number > static_cast<int>(report_action::none) &&
                area.number <= static_cast<int>(report_action::install_update))
            {
                measuring = true;
                measured_at = GetTickCount64();
                events.on_report_action(static_cast<report_action>(area.number));
                relayout();
            }
            break;
        case action_kind::install_update:
            if (events.on_install_update)
            {
                events.on_install_update();
            }
            break;
        case action_kind::apply_update:
            if (events.on_apply_update)
            {
                events.on_apply_update();
            }
            break;
        case action_kind::none:
            break;
        }
    }

    void about_window::state::move_focus(bool backward)
    {
        if (hot.empty())
        {
            return;
        }
        int const count = static_cast<int>(hot.size());
        focused = focused < 0 ? (backward ? count - 1 : 0) : (focused + (backward ? count - 1 : 1)) % count;
        focus_visible = true;
        reveal(hot[static_cast<std::size_t>(focused)]);
        redraw();
    }

    void about_window::state::apply_dark_frame() const
    {
        BOOL const use_dark = dark ? TRUE : FALSE;
        constexpr DWORD immersive_dark_mode = 20;          // DWMWA_USE_IMMERSIVE_DARK_MODE
        constexpr DWORD immersive_dark_mode_pre_20h1 = 19; // the same before Windows 10 20H1
        HWND const shown = window.load();
        if (FAILED(DwmSetWindowAttribute(shown, immersive_dark_mode, &use_dark, sizeof(use_dark))))
        {
            DwmSetWindowAttribute(shown, immersive_dark_mode_pre_20h1, &use_dark, sizeof(use_dark));
        }
    }

    // draws what of `list` lies between top and bottom (window coordinates), moved down by dy
    void about_window::state::draw_list(std::vector<visual> const &list, float dy, float top, float bottom)
    {
        for (visual const &item : list)
        {
            D2D1_RECT_F const place = moved(item.rect, 0.0F, dy);
            if (place.bottom < top - 4.0F || place.top > bottom + 4.0F)
            {
                continue;
            }
            bool const is_hovered = item.hot >= 0 && item.hot == hovered;
            bool const is_pressed = item.hot >= 0 && item.hot == pressed && is_hovered;
            bool const is_focused = item.hot >= 0 && item.hot == focused && focus_visible;
            switch (item.kind)
            {
            case visual_kind::text:
                if (item.layout)
                {
                    brush->SetColor(color(item.color));
                    target->DrawTextLayout(D2D1_POINT_2F{place.left, place.top}, item.layout.Get(), brush.Get(),
                                           D2D1_DRAW_TEXT_OPTIONS_NONE);
                }
                break;
            case visual_kind::card:
                brush->SetColor(color(tone::card));
                target->FillRoundedRectangle(D2D1_ROUNDED_RECT{place, card_radius, card_radius}, brush.Get());
                brush->SetColor(color(tone::border));
                target->DrawRoundedRectangle(D2D1_ROUNDED_RECT{inflated(place, -0.5F), card_radius, card_radius},
                                             brush.Get(), 1.0F);
                break;
            case visual_kind::line:
                brush->SetColor(color(item.color));
                target->FillRectangle(place, brush.Get());
                break;
            case visual_kind::dot:
            {
                float const radius = (place.right - place.left) / 2.0F;
                brush->SetColor(color(item.color));
                target->FillEllipse(D2D1_ELLIPSE{D2D1_POINT_2F{place.left + radius, place.top + radius}, radius, radius},
                                    brush.Get());
                break;
            }
            case visual_kind::ring:
            {
                constexpr float thickness = 7.0F;
                float const radius = (place.right - place.left) / 2.0F - thickness / 2.0F;
                D2D1_POINT_2F const centre{(place.left + place.right) / 2.0F, (place.top + place.bottom) / 2.0F};
                brush->SetColor(color(tone::track));
                target->DrawEllipse(D2D1_ELLIPSE{centre, radius, radius}, brush.Get(), thickness);
                float const share = std::clamp(item.share, 0.0F, 1.0F);
                brush->SetColor(color(item.color));
                if (share >= 0.999F)
                {
                    target->DrawEllipse(D2D1_ELLIPSE{centre, radius, radius}, brush.Get(), thickness);
                }
                else if (share > 0.0F)
                {
                    // from the top, clockwise
                    float const angle = share * 2.0F * std::numbers::pi_v<float>;
                    D2D1_POINT_2F const start{centre.x, centre.y - radius};
                    D2D1_POINT_2F const end{centre.x + radius * std::sin(angle), centre.y - radius * std::cos(angle)};
                    ComPtr<ID2D1PathGeometry> arc;
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(d2d_factory->CreatePathGeometry(arc.GetAddressOf())) &&
                        SUCCEEDED(arc->Open(sink.GetAddressOf())))
                    {
                        sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddArc(D2D1_ARC_SEGMENT{end, D2D1_SIZE_F{radius, radius}, 0.0F,
                                                      D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                                      share > 0.5F ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL});
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        if (SUCCEEDED(sink->Close()))
                        {
                            target->DrawGeometry(arc.Get(), brush.Get(), thickness, round_stroke.Get());
                        }
                    }
                }
                break;
            }
            case visual_kind::bar:
            {
                float const radius = (place.bottom - place.top) / 2.0F;
                brush->SetColor(color(tone::track));
                target->FillRoundedRectangle(D2D1_ROUNDED_RECT{place, radius, radius}, brush.Get());
                float const share = std::clamp(item.share, 0.0F, 1.0F);
                if (share > 0.0F)
                {
                    D2D1_RECT_F filled = place;
                    filled.right = place.left + std::max((place.right - place.left) * share, 2.0F * radius);
                    brush->SetColor(color(item.color));
                    target->FillRoundedRectangle(D2D1_ROUNDED_RECT{filled, radius, radius}, brush.Get());
                }
                break;
            }
            case visual_kind::button:
            {
                D2D1_ROUNDED_RECT const shape{place, 5.0F, 5.0F};
                if (item.selected && item.enabled)
                {
                    D2D1_COLOR_F fill = color(tone::accent);
                    fill.a = is_pressed ? 0.75F : is_hovered ? 0.88F : 1.0F;
                    brush->SetColor(fill);
                    target->FillRoundedRectangle(shape, brush.Get());
                }
                else
                {
                    brush->SetColor(color(is_pressed ? tone::control_pressed : is_hovered ? tone::control_hover
                                                                                          : tone::control));
                    target->FillRoundedRectangle(shape, brush.Get());
                    brush->SetColor(color(tone::border));
                    target->DrawRoundedRectangle(D2D1_ROUNDED_RECT{inflated(place, -0.5F), 5.0F, 5.0F}, brush.Get(),
                                                 1.0F);
                }
                if (item.layout)
                {
                    brush->SetColor(color(!item.enabled ? tone::tertiary : item.selected ? tone::on_accent : tone::text));
                    target->DrawTextLayout(D2D1_POINT_2F{place.left, place.top}, item.layout.Get(), brush.Get(),
                                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                break;
            }
            case visual_kind::toggle:
            {
                float const radius = (place.bottom - place.top) / 2.0F;
                bool const on = item.share > 0.5F;
                if (on)
                {
                    D2D1_COLOR_F fill = color(tone::accent);
                    fill.a = is_hovered ? 0.88F : 1.0F;
                    brush->SetColor(fill);
                    target->FillRoundedRectangle(D2D1_ROUNDED_RECT{place, radius, radius}, brush.Get());
                }
                else
                {
                    brush->SetColor(color(is_hovered ? tone::control_hover : tone::control));
                    target->FillRoundedRectangle(D2D1_ROUNDED_RECT{place, radius, radius}, brush.Get());
                    brush->SetColor(color(tone::tertiary));
                    target->DrawRoundedRectangle(D2D1_ROUNDED_RECT{inflated(place, -0.5F), radius, radius}, brush.Get(),
                                                 1.0F);
                }
                float const knob = radius - 4.5F;
                brush->SetColor(color(on ? tone::on_accent : tone::secondary));
                target->FillEllipse(D2D1_ELLIPSE{D2D1_POINT_2F{on ? place.right - radius : place.left + radius,
                                                               place.top + radius},
                                                 knob, knob},
                                    brush.Get());
                break;
            }
            case visual_kind::link:
                if (item.layout)
                {
                    brush->SetColor(color(tone::accent));
                    target->DrawTextLayout(D2D1_POINT_2F{place.left, place.top}, item.layout.Get(), brush.Get(),
                                           D2D1_DRAW_TEXT_OPTIONS_NONE);
                    if (is_hovered)
                    {
                        target->FillRectangle(D2D1_RECT_F{place.left, place.bottom - 1.0F, place.right, place.bottom},
                                              brush.Get());
                    }
                }
                break;
            case visual_kind::tab:
                if (is_hovered && !item.selected)
                {
                    brush->SetColor(color(tone::control_hover));
                    target->FillRoundedRectangle(
                        D2D1_ROUNDED_RECT{D2D1_RECT_F{place.left, place.top + 3.0F, place.right, place.bottom - 5.0F},
                                          5.0F, 5.0F},
                        brush.Get());
                }
                if (item.layout)
                {
                    brush->SetColor(color(item.selected ? tone::text : tone::secondary));
                    target->DrawTextLayout(D2D1_POINT_2F{place.left, place.top - 1.0F}, item.layout.Get(), brush.Get(),
                                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                if (item.selected)
                {
                    brush->SetColor(color(tone::accent));
                    target->FillRoundedRectangle(
                        D2D1_ROUNDED_RECT{D2D1_RECT_F{place.left + 12.0F, place.bottom - 3.0F, place.right - 12.0F,
                                                      place.bottom},
                                          1.5F, 1.5F},
                        brush.Get());
                }
                break;
            }
            if (is_focused)
            {
                brush->SetColor(color(tone::text));
                target->DrawRoundedRectangle(D2D1_ROUNDED_RECT{inflated(place, 3.0F), 6.0F, 6.0F}, brush.Get(), 1.5F);
            }
        }
    }

    void about_window::state::paint(HDC window_dc)
    {
        ensure_surface();
        if (memory_dc == nullptr || !target)
        {
            return;
        }
        RECT const bounds{0, 0, pixel_width, pixel_height};
        if (FAILED(target->BindDC(memory_dc, &bounds)))
        {
            return;
        }
        target->SetDpi(dpi, dpi);
        target->BeginDraw();
        target->Clear(color(tone::background));
        float const width = client_width();
        float const height = client_height();

        // the page, behind the fixed top part
        target->PushAxisAlignedClip(D2D1_RECT_F{0.0F, chrome_height, width, height}, D2D1_ANTIALIAS_MODE_ALIASED);
        draw_list(content, chrome_height - scroll, chrome_height, height);
        if (D2D1_RECT_F const bar = thumb(); bar.bottom > bar.top)
        {
            D2D1_COLOR_F shade = color(tone::tertiary);
            shade.a = dragging_thumb ? 0.9F : 0.55F;
            brush->SetColor(shade);
            target->FillRoundedRectangle(D2D1_ROUNDED_RECT{bar, 3.0F, 3.0F}, brush.Get());
        }
        target->PopAxisAlignedClip();

        draw_list(chrome, 0.0F, 0.0F, chrome_height);
        HRESULT const drawn = target->EndDraw();
        if (drawn == D2DERR_RECREATE_TARGET)
        {
            // rare for a software target; the next paint draws with a new one
            log::warning("about window: {}", make_hresult_failure("ID2D1RenderTarget::EndDraw", drawn).describe());
        }

        // the program's icon, straight from its resources
        int const wanted = static_cast<int>(std::lround(icon_size * dpi / 96.0F));
        if (icon == nullptr || icon_pixels != wanted)
        {
            if (icon != nullptr)
            {
                DestroyIcon(icon);
            }
            icon = static_cast<HICON>(
                LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, wanted, wanted, LR_DEFAULTCOLOR));
            icon_pixels = wanted;
        }
        if (icon != nullptr)
        {
            DrawIconEx(memory_dc, static_cast<int>(std::lround(icon_place.left * dpi / 96.0F)),
                       static_cast<int>(std::lround(icon_place.top * dpi / 96.0F)), icon, wanted, wanted, 0, nullptr,
                       DI_NORMAL);
        }
        BitBlt(window_dc, 0, 0, pixel_width, pixel_height, memory_dc, 0, 0, SRCCOPY);
    }

    LRESULT CALLBACK about_window::state::procedure(HWND handle_window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE)
        {
            auto const *creation = reinterpret_cast<CREATESTRUCTW const *>(lparam);
            SetWindowLongPtrW(handle_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        auto *owner = reinterpret_cast<state *>(GetWindowLongPtrW(handle_window, GWLP_USERDATA));
        if (owner == nullptr || owner->window.load() != handle_window)
        {
            return DefWindowProcW(handle_window, message, wparam, lparam);
        }
        return owner->handle(message, wparam, lparam);
    }

    LRESULT about_window::state::handle(UINT message, WPARAM wparam, LPARAM lparam)
    {
        HWND const shown = window.load();
        auto const pointer_from = [this](LPARAM position)
        {
            pointer_x = static_cast<float>(GET_X_LPARAM(position)) * 96.0F / dpi;
            pointer_y = static_cast<float>(GET_Y_LPARAM(position)) * 96.0F / dpi;
        };
        switch (message)
        {
        case message_model:
            take_model();
            return 0;
        case message_activate:
            if (IsIconic(shown))
            {
                ShowWindow(shown, SW_RESTORE);
            }
            bring_to_front(shown, lparam != 0);
            if (wparam <= static_cast<WPARAM>(about_page::components))
            {
                set_page(static_cast<about_page>(wparam));
            }
            return 0;
        case message_close:
        case WM_CLOSE:
            DestroyWindow(shown);
            return 0;
        case WM_DESTROY:
            KillTimer(shown, copied_timer_id);
            window.store(nullptr);
            PostQuitMessage(0);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT painting{};
            HDC const window_dc = BeginPaint(shown, &painting);
            paint(window_dc);
            EndPaint(shown, &painting);
            return 0;
        }
        case WM_PRINTCLIENT:
            // another program asks for the window's picture (a capture tool, a magnifier)
            paint(reinterpret_cast<HDC>(wparam));
            return 0;
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED)
            {
                pixel_width = LOWORD(lparam);
                pixel_height = HIWORD(lparam);
                relayout();
            }
            return 0;
        case WM_DPICHANGED:
        {
            dpi = static_cast<float>(HIWORD(wparam));
            auto const *suggested = reinterpret_cast<RECT const *>(lparam);
            SetWindowPos(shown, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            relayout();
            return 0;
        }
        case WM_GETMINMAXINFO:
        {
            auto *limits = reinterpret_cast<MINMAXINFO *>(lparam);
            RECT smallest{0, 0, static_cast<LONG>(std::lround(minimum_width * dpi / 96.0F)),
                          static_cast<LONG>(std::lround(minimum_height * dpi / 96.0F))};
            AdjustWindowRectExForDpi(&smallest, static_cast<DWORD>(GetWindowLongPtrW(shown, GWL_STYLE)), FALSE,
                                     static_cast<DWORD>(GetWindowLongPtrW(shown, GWL_EXSTYLE)), static_cast<UINT>(dpi));
            limits->ptMinTrackSize.x = smallest.right - smallest.left;
            limits->ptMinTrackSize.y = smallest.bottom - smallest.top;
            return 0;
        }
        case WM_ACTIVATE:
            // Back from somewhere else with the page "This PC" open: from the
            // Settings page a button opened, perhaps, where something was
            // changed.  The PC is measured again (not more often than every
            // few seconds, and not while it is being measured).
            if (LOWORD(wparam) != WA_INACTIVE && page == about_page::this_pc && !measuring && facts &&
                GetTickCount64() - measured_at >= return_measure_ms)
            {
                measure_again();
            }
            break;
        case WM_SETTINGCHANGE:
            // "ImmersiveColorSet": the user switched Windows between light and dark
            if (bool const now_dark = windows_uses_dark_apps(); now_dark != dark)
            {
                dark = now_dark;
                apply_dark_frame();
                redraw();
            }
            break;
        case WM_TIMER:
            if (wparam == copied_timer_id)
            {
                KillTimer(shown, copied_timer_id);
                copied = false;
                relayout();
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
        {
            pointer_from(lparam);
            if (!tracking_leave)
            {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = shown;
                tracking_leave = TrackMouseEvent(&track) != FALSE;
            }
            if (dragging_thumb)
            {
                D2D1_RECT_F const bar = thumb();
                float const travel = viewport_height() - (bar.bottom - bar.top + 4.0F);
                if (travel > 0.0F)
                {
                    scroll_to((pointer_y - drag_grip - chrome_height) * largest_scroll() / travel);
                }
                return 0;
            }
            if (int const under = hit(pointer_x, pointer_y); under != hovered)
            {
                hovered = under;
                redraw();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_leave = false;
            pointer_x = -1.0F;
            pointer_y = -1.0F;
            if (hovered != -1)
            {
                hovered = -1;
                redraw();
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        {
            pointer_from(lparam);
            focus_visible = false;
            SetCapture(shown);
            D2D1_RECT_F const bar = thumb();
            if (bar.bottom > bar.top && pointer_x >= client_width() - 16.0F && pointer_y >= chrome_height)
            {
                if (pointer_y < bar.top || pointer_y >= bar.bottom)
                {
                    // beside the thumb: a page towards the pointer
                    scroll_to(scroll + (pointer_y < bar.top ? -0.9F : 0.9F) * viewport_height());
                }
                else
                {
                    dragging_thumb = true;
                    drag_grip = pointer_y - bar.top + 2.0F;
                }
                redraw();
                return 0;
            }
            pressed = hit(pointer_x, pointer_y);
            hovered = pressed;
            redraw();
            return 0;
        }
        case WM_LBUTTONUP:
        {
            pointer_from(lparam);
            // (taken before the capture goes: giving it up sends WM_CAPTURECHANGED,
            // which forgets what was pressed)
            bool const was_dragging = std::exchange(dragging_thumb, false);
            int const was_pressed = std::exchange(pressed, -1);
            if (GetCapture() == shown)
            {
                ReleaseCapture();
            }
            if (was_dragging)
            {
                redraw();
                return 0;
            }
            int const released_on = hit(pointer_x, pointer_y);
            hovered = released_on;
            redraw();
            if (was_pressed >= 0 && was_pressed == released_on)
            {
                activate(was_pressed);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            dragging_thumb = false;
            pressed = -1;
            redraw();
            return 0;
        case WM_MOUSEWHEEL:
            scroll_to(scroll - static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * wheel_step);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT)
            {
                SetCursor(LoadCursorW(nullptr, hovered >= 0 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
        {
            bool const control = GetKeyState(VK_CONTROL) < 0;
            bool const shift = GetKeyState(VK_SHIFT) < 0;
            switch (wparam)
            {
            case VK_ESCAPE:
                DestroyWindow(shown);
                return 0;
            case VK_TAB:
                if (control)
                {
                    constexpr int pages = static_cast<int>(about_page::components) + 1;
                    set_page(static_cast<about_page>((static_cast<int>(page) + (shift ? pages - 1 : 1)) % pages));
                }
                else
                {
                    move_focus(shift);
                }
                return 0;
            case VK_RETURN:
            case VK_SPACE:
                if (focused >= 0)
                {
                    activate(focused);
                }
                return 0;
            case 'C':
                if (control)
                {
                    copy_report();
                }
                return 0;
            case VK_UP:
                scroll_to(scroll - key_step);
                return 0;
            case VK_DOWN:
                scroll_to(scroll + key_step);
                return 0;
            case VK_PRIOR:
                scroll_to(scroll - 0.9F * viewport_height());
                return 0;
            case VK_NEXT:
                scroll_to(scroll + 0.9F * viewport_height());
                return 0;
            case VK_HOME:
                scroll_to(0.0F);
                return 0;
            case VK_END:
                scroll_to(largest_scroll());
                return 0;
            default:
                break;
            }
            break;
        }
        default:
            break;
        }
        return DefWindowProcW(shown, message, wparam, lparam);
    }

    void about_window::state::run(about_page first_page, bool topmost, std::promise<status> &ready)
    {
        // ShellExecute (the links) wants an apartment
        HRESULT const com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto const leave = [com]
        {
            if (SUCCEEDED(com))
            {
                CoUninitialize();
            }
        };
        // a window that is opened again starts like the first one
        page = first_page;
        scroll = 0.0F;
        hovered = -1;
        pressed = -1;
        focused = -1;
        focus_visible = false;
        tracking_leave = false;
        dragging_thumb = false;
        copied = false;
        pointer_x = -1.0F;
        pointer_y = -1.0F;
        pixel_width = 0;
        pixel_height = 0;
        dark = windows_uses_dark_apps();

        if (status const created = create_resources(); !created)
        {
            release_resources();
            leave();
            ready.set_value(std::unexpected(created.error()));
            return;
        }
        HINSTANCE const instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_DBLCLKS;
        window_class.lpfnWndProc = state::procedure;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        window_class.lpszClassName = window_class_name;
        RegisterClassExW(&window_class); // (registered already when the window was open before)

        constexpr DWORD style = WS_OVERLAPPEDWINDOW & ~static_cast<DWORD>(WS_MAXIMIZEBOX);
        HWND const created_window =
            CreateWindowExW(0, window_class_name, std::wstring{ui_string(ui_text::about_title)}.c_str(), style,
                            CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, nullptr, nullptr, instance, this);
        if (created_window == nullptr)
        {
            failure const cause = make_win32_failure("CreateWindowExW", GetLastError());
            release_resources();
            leave();
            ready.set_value(std::unexpected(cause));
            return;
        }
        window.store(created_window);
        apply_dark_frame();

        // its size for the monitor it opened on, in the middle of that monitor's work area
        dpi = static_cast<float>(GetDpiForWindow(created_window));
        RECT outer{0, 0, static_cast<LONG>(std::lround(default_width * dpi / 96.0F)),
                   static_cast<LONG>(std::lround(default_height * dpi / 96.0F))};
        AdjustWindowRectExForDpi(&outer, style, FALSE, 0, static_cast<UINT>(dpi));
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(created_window, MONITOR_DEFAULTTOPRIMARY), &monitor);
        LONG const work_width = monitor.rcWork.right - monitor.rcWork.left;
        LONG const work_height = monitor.rcWork.bottom - monitor.rcWork.top;
        LONG const width = std::min(outer.right - outer.left, work_width);
        LONG const height = std::min(outer.bottom - outer.top, work_height - 32);
        SetWindowPos(created_window, nullptr, monitor.rcWork.left + (work_width - width) / 2,
                     monitor.rcWork.top + (work_height - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        RECT client{};
        GetClientRect(created_window, &client);
        pixel_width = client.right;
        pixel_height = client.bottom;
        take_model(); // lays the page out
        ShowWindow(created_window, SW_SHOWNORMAL);
        bring_to_front(created_window, topmost);
        ready.set_value(status{});

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        release_resources();
        leave();
    }

    // ---- the public side ----------------------------------------------------------

    about_window::about_window(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    about_window::~about_window()
    {
        close();
    }

    void about_window::close()
    {
        std::scoped_lock serial{impl->show_mutex};
        if (HWND const shown = impl->window.load(); shown != nullptr)
        {
            PostMessageW(shown, message_close, 0, 0);
        }
        if (impl->ui_thread.joinable())
        {
            impl->ui_thread.join();
        }
    }

    std::unique_ptr<about_window> about_window::create(about_identity identity, about_events events)
    {
        auto created = std::make_unique<state>();
        created->identity = std::move(identity);
        created->events = std::move(events);
        return std::unique_ptr<about_window>{new about_window{std::move(created)}};
    }

    status about_window::show(about_page page, bool topmost)
    {
        std::scoped_lock serial{impl->show_mutex};
        if (HWND const shown = impl->window.load(); shown != nullptr)
        {
            PostMessageW(shown, message_activate, static_cast<WPARAM>(page), topmost ? 1 : 0);
            return {};
        }
        if (impl->ui_thread.joinable())
        {
            impl->ui_thread.join(); // of a window the user has closed: it is ending or has ended
        }
        impl->measuring = true;
        std::promise<status> ready;
        std::future<status> created = ready.get_future();
        state *const shared = impl.get();
        impl->ui_thread = std::thread{[shared, page, topmost, &ready]
        {
            shared->run(page, topmost, ready);
        }};
        return created.get();
    }

    bool about_window::is_open() const
    {
        return impl->window.load() != nullptr;
    }

    void about_window::set_update(update_state const &state_of_check)
    {
        {
            std::scoped_lock lock{impl->model_mutex};
            impl->pending_update = state_of_check;
        }
        if (HWND const shown = impl->window.load(); shown != nullptr)
        {
            PostMessageW(shown, message_model, 0, 0);
        }
    }

    void about_window::set_install(install_state const &state_of_install)
    {
        {
            std::scoped_lock lock{impl->model_mutex};
            impl->pending_install = state_of_install;
        }
        if (HWND const shown = impl->window.load(); shown != nullptr)
        {
            PostMessageW(shown, message_model, 0, 0);
        }
    }

    void about_window::set_facts(system_facts facts)
    {
        {
            std::scoped_lock lock{impl->model_mutex};
            impl->pending_facts = std::move(facts);
        }
        if (HWND const shown = impl->window.load(); shown != nullptr)
        {
            PostMessageW(shown, message_model, 0, 0);
        }
    }
}
