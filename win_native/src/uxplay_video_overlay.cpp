// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

module uxplay_video_overlay;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_ui_text;

namespace uxplay
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr wchar_t overlay_class_name[] = L"uxplay_video_overlay";
        constexpr UINT_PTR idle_timer_id = 1;
        constexpr UINT_PTR fade_timer_id = 2;
        // like a media player: controls leave after a few seconds without movement
        constexpr UINT idle_hide_ms = 3000;
        constexpr UINT fade_tick_ms = 15;
        constexpr int fade_in_step = 64;  // ~60 ms
        constexpr int fade_out_step = 24; // ~160 ms
        // the notice: shown at once, stays this long, then fades like the bar
        constexpr UINT_PTR notice_hold_timer_id = 3;
        constexpr UINT_PTR notice_fade_timer_id = 4;
        constexpr UINT notice_hold_ms = 1800;
        constexpr float notice_height = 40.0F;
        constexpr float notice_padding = 20.0F;
        constexpr float notice_top_margin = 24.0F;

        // layout in device-independent pixels (scaled by the owner's DPI)
        constexpr float bar_height = 84.0F;
        constexpr float row_height = 36.0F;
        constexpr float bottom_margin = 16.0F;
        constexpr float side_margin = 20.0F;
        constexpr float gap = 12.0F;
        constexpr float icon_gap = 4.0F;
        constexpr float icon_button_size = 36.0F;
        constexpr float text_padding = 14.0F;
        constexpr float chip_padding = 12.0F;
        constexpr float chip_icon_width = 18.0F;
        constexpr float corner_radius = 8.0F;

        // glyphs shared by Segoe Fluent Icons (Windows 11) and Segoe MDL2 Assets (Windows 10)
        constexpr std::wstring_view glyph_usb = L"";
        constexpr std::wstring_view glyph_wifi = L"";
        constexpr std::wstring_view glyph_fullscreen = L"";
        constexpr std::wstring_view glyph_back_to_window = L"";
        constexpr std::wstring_view glyph_volume = L"";
        constexpr std::wstring_view glyph_mute = L"";
        constexpr std::wstring_view glyph_more = L"";
        constexpr std::wstring_view glyph_shield = L"";

        // "Camera" in both icon fonts
        constexpr std::wstring_view glyph_camera = L"";

        enum class element
        {
            none,
            prefer_usb,
            wifi_only,
            data_guard,
            enable_adapter,
            capture,
            mute,
            more,
            fullscreen
        };

        struct hit_box
        {
            element id = element::none;
            D2D1_RECT_F rect{};
        };

        D2D1_COLOR_F rgba(float red, float green, float blue, float alpha)
        {
            return D2D1_COLOR_F{red, green, blue, alpha};
        }

        D2D1_COLOR_F white(float alpha)
        {
            return rgba(1.0F, 1.0F, 1.0F, alpha);
        }

        bool contains(D2D1_RECT_F const &rect, float x, float y)
        {
            return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
        }

        D2D1_ROUNDED_RECT rounded(D2D1_RECT_F rect, float radius)
        {
            return D2D1_ROUNDED_RECT{rect, radius, radius};
        }
    }

    struct video_overlay::state
    {
        HWND owner = nullptr;
        HWND window = nullptr;
        std::function<void(overlay_command command)> on_command;
        overlay_model model;

        ComPtr<ID2D1Factory> d2d_factory;
        ComPtr<IDWriteFactory> dwrite_factory;
        ComPtr<ID2D1DCRenderTarget> target;
        ComPtr<ID2D1SolidColorBrush> brush;
        ComPtr<IDWriteTextFormat> label_format;
        ComPtr<IDWriteTextFormat> detail_format;
        ComPtr<IDWriteTextFormat> button_format;
        ComPtr<IDWriteTextFormat> icon_format; // null: no icon font, text labels instead

        HDC memory_dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous_bitmap = nullptr;
        int pixel_width = 0;
        int pixel_height = 0;
        float dpi = 96.0F;
        float drawn_dpi = 0.0F;
        POINT position{};

        int alpha = 0;
        bool shown = false;
        bool fading_out = false;
        std::vector<hit_box> boxes;
        element hovered = element::none;
        element pressed = element::none;
        bool pointer_inside = false;

        // the notice (see video_overlay::notice): its own window and surface
        HWND notice_window = nullptr;
        HDC notice_dc = nullptr;
        HBITMAP notice_bitmap = nullptr;
        HGDIOBJ notice_previous_bitmap = nullptr;
        int notice_pixel_width = 0;
        int notice_pixel_height = 0;
        int notice_alpha = 0;
        bool notice_shown = false;

        state() = default;
        state(state const &) = delete;
        state &operator=(state const &) = delete;
        ~state();

        status create_resources();
        status create_target();
        void ensure_surface(int width, int height);
        void release_surface();
        float text_width(std::wstring_view text, IDWriteTextFormat *format) const;
        void draw_text(std::wstring_view text, IDWriteTextFormat *format, D2D1_RECT_F rect, D2D1_COLOR_F color,
                       DWRITE_TEXT_ALIGNMENT alignment);
        float button_width(std::wstring_view fallback_label) const;
        void draw_icon_button(element id, std::wstring_view glyph, std::wstring_view fallback_label, float right,
                              float top, float &consumed_left);
        void draw();
        void present();
        void follow_owner();
        void show();
        void begin_fade_out();
        void hide_now();
        void show_notice(std::wstring_view text);
        void place_notice();
        void hide_notice();
        void release_notice_surface();
        element hit(LPARAM lparam) const;
        void run(element id);
        LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);

        static LRESULT CALLBACK procedure(HWND handle_window, UINT message, WPARAM wparam, LPARAM lparam);
    };

    video_overlay::state::~state()
    {
        if (window != nullptr)
        {
            KillTimer(window, idle_timer_id);
            KillTimer(window, fade_timer_id);
            KillTimer(window, notice_hold_timer_id);
            KillTimer(window, notice_fade_timer_id);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            DestroyWindow(window);
        }
        if (notice_window != nullptr)
        {
            SetWindowLongPtrW(notice_window, GWLP_USERDATA, 0);
            DestroyWindow(notice_window);
        }
        release_surface();
        release_notice_surface();
    }

    status video_overlay::state::create_resources()
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
            ComPtr<IDWriteTextFormat> *format;
            wchar_t const *family;
            DWRITE_FONT_WEIGHT weight;
            float size;
        };
        std::array const specs{format_spec{&label_format, L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 15.0F},
                               format_spec{&detail_format, L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 13.0F},
                               format_spec{&button_format, L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0F}};
        for (format_spec const &spec : specs)
        {
            if (HRESULT const result = dwrite_factory->CreateTextFormat(
                    spec.family, nullptr, spec.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, spec.size,
                    ui_locale_name().data(), spec.format->GetAddressOf());
                FAILED(result))
            {
                return std::unexpected(make_hresult_failure("IDWriteFactory::CreateTextFormat", result));
            }
        }
        // icon font: Segoe Fluent Icons (Windows 11) or Segoe MDL2 Assets (Windows 10)
        ComPtr<IDWriteFontCollection> fonts;
        if (SUCCEEDED(dwrite_factory->GetSystemFontCollection(fonts.GetAddressOf(), FALSE)))
        {
            for (wchar_t const *family : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"})
            {
                UINT32 index = 0;
                BOOL exists = FALSE;
                if (SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists &&
                    SUCCEEDED(dwrite_factory->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                               16.0F, ui_locale_name().data(),
                                                               icon_format.GetAddressOf())))
                {
                    break;
                }
            }
        }
        for (IDWriteTextFormat *format : {label_format.Get(), detail_format.Get(), button_format.Get(), icon_format.Get()})
        {
            if (format != nullptr)
            {
                format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        }
        return create_target();
    }

    status video_overlay::state::create_target()
    {
        target.Reset();
        brush.Reset();
        // software rasterizer: a few redraws of a small bar must not compete
        // with video decode and presentation on the GPU
        D2D1_RENDER_TARGET_PROPERTIES properties{};
        properties.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
        properties.pixelFormat = D2D1_PIXEL_FORMAT{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
        properties.usage = D2D1_RENDER_TARGET_USAGE_NONE;
        properties.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
        if (HRESULT const result = d2d_factory->CreateDCRenderTarget(&properties, target.GetAddressOf()); FAILED(result))
        {
            return std::unexpected(make_hresult_failure("ID2D1Factory::CreateDCRenderTarget", result));
        }
        // a transparent surface has nothing to blend ClearType against
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (HRESULT const result = target->CreateSolidColorBrush(white(1.0F), brush.GetAddressOf()); FAILED(result))
        {
            return std::unexpected(make_hresult_failure("ID2D1RenderTarget::CreateSolidColorBrush", result));
        }
        return {};
    }

    void video_overlay::state::ensure_surface(int width, int height)
    {
        if (width == pixel_width && height == pixel_height && memory_dc != nullptr)
        {
            return;
        }
        release_surface();
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height; // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        memory_dc = CreateCompatibleDC(nullptr);
        void *bits = nullptr;
        bitmap = memory_dc != nullptr ? CreateDIBSection(memory_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (bitmap == nullptr)
        {
            log::warning("control bar: {}", make_win32_failure("CreateDIBSection", GetLastError()).describe());
            release_surface();
            return;
        }
        previous_bitmap = SelectObject(memory_dc, bitmap);
        pixel_width = width;
        pixel_height = height;
        drawn_dpi = 0.0F; // force a redraw
    }

    void video_overlay::state::release_surface()
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
        pixel_width = 0;
        pixel_height = 0;
    }

    float video_overlay::state::text_width(std::wstring_view text, IDWriteTextFormat *format) const
    {
        ComPtr<IDWriteTextLayout> layout;
        if (format == nullptr ||
            FAILED(dwrite_factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, 4096.0F,
                                                    64.0F, layout.GetAddressOf())))
        {
            return 0.0F;
        }
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        return std::ceil(metrics.widthIncludingTrailingWhitespace);
    }

    void video_overlay::state::draw_text(std::wstring_view text, IDWriteTextFormat *format, D2D1_RECT_F rect,
                                         D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT alignment)
    {
        if (format == nullptr)
        {
            return;
        }
        format->SetTextAlignment(alignment);
        brush->SetColor(color);
        target->DrawText(text.data(), static_cast<UINT32>(text.size()), format, rect, brush.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    float video_overlay::state::button_width(std::wstring_view fallback_label) const
    {
        return icon_format != nullptr ? icon_button_size
                                      : text_width(fallback_label, button_format.Get()) + 2.0F * text_padding;
    }

    void video_overlay::state::draw_icon_button(element id, std::wstring_view glyph, std::wstring_view fallback_label,
                                                float right, float top, float &consumed_left)
    {
        float const width = button_width(fallback_label);
        D2D1_RECT_F const rect{right - width, top, right, top + row_height};
        if (hovered == id || pressed == id)
        {
            brush->SetColor(white(pressed == id ? 0.28F : 0.16F));
            target->FillRoundedRectangle(rounded(rect, row_height / 2.0F), brush.Get());
        }
        if (icon_format != nullptr)
        {
            draw_text(glyph, icon_format.Get(), rect, white(0.95F), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        else
        {
            draw_text(fallback_label, button_format.Get(), rect, white(0.95F), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        boxes.push_back(hit_box{id, rect});
        consumed_left = rect.left;
    }

    void video_overlay::state::draw()
    {
        if (memory_dc == nullptr || target == nullptr)
        {
            return;
        }
        RECT const bounds{0, 0, pixel_width, pixel_height};
        if (FAILED(target->BindDC(memory_dc, &bounds)))
        {
            return;
        }
        target->SetDpi(dpi, dpi);
        float const width = static_cast<float>(pixel_width) * 96.0F / dpi;
        float const height = static_cast<float>(pixel_height) * 96.0F / dpi;
        boxes.clear();

        target->BeginDraw();
        target->Clear(rgba(0.0F, 0.0F, 0.0F, 0.0F));

        // backdrop: transparent at the top, dark at the bottom (keeps the video visible)
        std::array const stops{D2D1_GRADIENT_STOP{0.0F, rgba(0.0F, 0.0F, 0.0F, 0.0F)},
                               D2D1_GRADIENT_STOP{0.45F, rgba(0.0F, 0.0F, 0.0F, 0.32F)},
                               D2D1_GRADIENT_STOP{1.0F, rgba(0.0F, 0.0F, 0.0F, 0.78F)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> gradient;
        if (SUCCEEDED(target->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()),
                                                           collection.GetAddressOf())) &&
            SUCCEEDED(target->CreateLinearGradientBrush(
                D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES{D2D1_POINT_2F{0.0F, 0.0F}, D2D1_POINT_2F{0.0F, height}},
                collection.Get(), gradient.GetAddressOf())))
        {
            target->FillRectangle(D2D1_RECT_F{0.0F, 0.0F, width, height}, gradient.Get());
        }

        float const top = height - bottom_margin - row_height;
        float const bottom = top + row_height;

        // what fits: icons always; then the alert chips, the link switch, the
        // stream label, the stream detail
        // labels in the interface language: widths are measured, so longer
        // English labels push lower-priority elements out instead of clipping
        std::wstring_view const fullscreen_label =
            ui_string(model.fullscreen ? ui_text::exit_full_screen : ui_text::full_screen);
        std::wstring_view const more_label = ui_string(ui_text::more);
        std::wstring_view const mute_label = ui_string(model.muted ? ui_text::unmute : ui_text::mute);
        std::wstring_view const capture_label = ui_string(ui_text::screenshot);
        std::wstring_view const prefer_usb_label = ui_string(ui_text::prefer_usb_link);
        std::wstring_view const wifi_only_label = ui_string(ui_text::wifi_only);
        float const segment_one = text_width(prefer_usb_label, button_format.Get()) + 2.0F * text_padding;
        float const segment_two = text_width(wifi_only_label, button_format.Get()) + 2.0F * text_padding;
        float const segmented_width = segment_one + segment_two + 4.0F;
        auto chip_width = [this](std::wstring_view label)
        {
            return text_width(label, button_format.Get()) + chip_icon_width + 6.0F + 2.0F * chip_padding;
        };
        float const icons_width = button_width(fullscreen_label) + button_width(more_label) +
                                  button_width(mute_label) + button_width(capture_label) + 3.0F * icon_gap;
        float chips_width = 0.0F;
        if (model.offer_enable_adapter)
        {
            chips_width += chip_width(ui_string(ui_text::enable_usb_adapter)) + gap;
        }
        if (model.offer_data_guard)
        {
            chips_width += chip_width(ui_string(ui_text::protect_mobile_data)) + gap;
        }
        float const label_width = model.transport.empty()
                                      ? 0.0F
                                      : (icon_format != nullptr ? 26.0F : 0.0F) +
                                            text_width(model.transport, label_format.Get());
        float const detail_width = model.detail.empty() ? 0.0F : 10.0F + text_width(model.detail, detail_format.Get());
        float const available = width - 2.0F * side_margin;
        bool const show_chips = icons_width + chips_width <= available;
        bool const show_segmented = show_chips && icons_width + chips_width + segmented_width + gap <= available;
        bool const show_label = show_segmented && icons_width + chips_width + segmented_width + gap + label_width +
                                                          gap <=
                                                      available;
        bool const show_detail = show_label && icons_width + chips_width + segmented_width + gap + label_width +
                                                         detail_width + gap <=
                                                     available;

        // right cluster, from the right edge
        float right = width - side_margin;
        float left_edge = right;
        draw_icon_button(element::fullscreen, model.fullscreen ? glyph_back_to_window : glyph_fullscreen,
                         fullscreen_label, right, top, left_edge);
        right = left_edge - icon_gap;
        draw_icon_button(element::more, glyph_more, more_label, right, top, left_edge);
        right = left_edge - icon_gap;
        draw_icon_button(element::mute, model.muted ? glyph_mute : glyph_volume, mute_label, right, top, left_edge);
        right = left_edge - icon_gap;
        draw_icon_button(element::capture, glyph_camera, capture_label, right, top, left_edge);
        right = left_edge - gap;

        if (show_segmented)
        {
            D2D1_RECT_F const container{right - segmented_width, top, right, bottom};
            brush->SetColor(white(0.14F));
            target->FillRoundedRectangle(rounded(container, corner_radius), brush.Get());
            D2D1_RECT_F const first{container.left + 2.0F, top + 2.0F, container.left + 2.0F + segment_one, bottom - 2.0F};
            D2D1_RECT_F const second{first.right, top + 2.0F, first.right + segment_two, bottom - 2.0F};
            auto segment = [this](element id, D2D1_RECT_F rect, std::wstring_view label, bool selected)
            {
                if (selected)
                {
                    brush->SetColor(white(0.92F));
                    target->FillRoundedRectangle(rounded(rect, corner_radius - 2.0F), brush.Get());
                }
                else if (hovered == id || pressed == id)
                {
                    brush->SetColor(white(pressed == id ? 0.24F : 0.14F));
                    target->FillRoundedRectangle(rounded(rect, corner_radius - 2.0F), brush.Get());
                }
                draw_text(label, button_format.Get(), rect,
                          selected ? rgba(0.08F, 0.08F, 0.08F, 1.0F) : white(0.88F), DWRITE_TEXT_ALIGNMENT_CENTER);
                boxes.push_back(hit_box{id, rect});
            };
            segment(element::prefer_usb, first, prefer_usb_label, model.prefer_usb_link);
            segment(element::wifi_only, second, wifi_only_label, !model.prefer_usb_link);
            right = container.left - gap;
        }

        if (show_chips)
        {
            auto chip = [&](element id, std::wstring_view label)
            {
                float const chip_w = chip_width(label);
                D2D1_RECT_F const rect{right - chip_w, top + 3.0F, right, bottom - 3.0F};
                brush->SetColor(rgba(0.98F, 0.72F, 0.18F, hovered == id || pressed == id ? 1.0F : 0.92F));
                target->FillRoundedRectangle(rounded(rect, (rect.bottom - rect.top) / 2.0F), brush.Get());
                D2D1_RECT_F const icon_rect{rect.left + chip_padding, rect.top, rect.left + chip_padding + chip_icon_width,
                                            rect.bottom};
                if (icon_format != nullptr)
                {
                    draw_text(glyph_shield, icon_format.Get(), icon_rect, rgba(0.1F, 0.08F, 0.0F, 1.0F),
                              DWRITE_TEXT_ALIGNMENT_CENTER);
                }
                D2D1_RECT_F const text_rect{icon_rect.right + 6.0F, rect.top, rect.right - chip_padding, rect.bottom};
                draw_text(label, button_format.Get(), text_rect, rgba(0.1F, 0.08F, 0.0F, 1.0F),
                          DWRITE_TEXT_ALIGNMENT_LEADING);
                boxes.push_back(hit_box{id, rect});
                right = rect.left - gap;
            };
            if (model.offer_data_guard)
            {
                chip(element::data_guard, ui_string(ui_text::protect_mobile_data));
            }
            if (model.offer_enable_adapter)
            {
                chip(element::enable_adapter, ui_string(ui_text::enable_usb_adapter));
            }
        }

        // left cluster: which link carries the mirror, and the stream size
        if (show_label)
        {
            float x = side_margin;
            if (icon_format != nullptr)
            {
                draw_text(model.transport == L"USB" ? glyph_usb : glyph_wifi, icon_format.Get(),
                          D2D1_RECT_F{x, top, x + 20.0F, bottom}, white(0.92F), DWRITE_TEXT_ALIGNMENT_CENTER);
                x += 26.0F;
            }
            float const transport_width = text_width(model.transport, label_format.Get());
            draw_text(model.transport, label_format.Get(), D2D1_RECT_F{x, top, x + transport_width + 2.0F, bottom},
                      white(0.96F), DWRITE_TEXT_ALIGNMENT_LEADING);
            x += transport_width + 10.0F;
            if (show_detail)
            {
                draw_text(model.detail, detail_format.Get(), D2D1_RECT_F{x, top, right, bottom}, white(0.62F),
                          DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        }

        HRESULT const result = target->EndDraw();
        if (result == D2DERR_RECREATE_TARGET)
        {
            if (status const recreated = create_target(); !recreated)
            {
                log::warning("control bar: {}", recreated.error().describe());
            }
        }
        drawn_dpi = dpi;
    }

    void video_overlay::state::present()
    {
        if (memory_dc == nullptr)
        {
            return;
        }
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = static_cast<BYTE>(std::clamp(alpha, 0, 255));
        blend.AlphaFormat = AC_SRC_ALPHA;
        POINT source{0, 0};
        SIZE size{pixel_width, pixel_height};
        UpdateLayeredWindow(window, nullptr, &position, &size, memory_dc, &source, 0, &blend, ULW_ALPHA);
    }

    void video_overlay::state::follow_owner()
    {
        if (!IsWindowVisible(owner) || IsIconic(owner) != FALSE)
        {
            hide_now();
            return;
        }
        RECT client{};
        GetClientRect(owner, &client);
        POINT origin{0, 0};
        ClientToScreen(owner, &origin);
        dpi = static_cast<float>(GetDpiForWindow(owner));
        int const height = std::min(static_cast<int>(client.bottom), static_cast<int>(std::lround(bar_height * dpi / 96.0F)));
        int const width = static_cast<int>(client.right);
        if (width <= 0 || height <= 0)
        {
            hide_now();
            return;
        }
        position = POINT{origin.x, origin.y + client.bottom - height};
        ensure_surface(width, height);
        if (shown)
        {
            if (drawn_dpi != dpi)
            {
                draw();
            }
            present();
        }
    }

    void video_overlay::state::show()
    {
        if (!IsWindowVisible(owner) || IsIconic(owner) != FALSE)
        {
            return;
        }
        fading_out = false;
        if (!shown)
        {
            shown = true;
            alpha = 0;
            follow_owner();
            draw();
            present();
            ShowWindow(window, SW_SHOWNOACTIVATE);
        }
        if (alpha < 255)
        {
            SetTimer(window, fade_timer_id, fade_tick_ms, nullptr);
        }
        SetTimer(window, idle_timer_id, idle_hide_ms, nullptr);
    }

    void video_overlay::state::begin_fade_out()
    {
        if (!shown)
        {
            return;
        }
        fading_out = true;
        SetTimer(window, fade_timer_id, fade_tick_ms, nullptr);
    }

    void video_overlay::state::hide_now()
    {
        KillTimer(window, idle_timer_id);
        KillTimer(window, fade_timer_id);
        if (shown)
        {
            ShowWindow(window, SW_HIDE);
        }
        shown = false;
        fading_out = false;
        alpha = 0;
        hovered = element::none;
        pressed = element::none;
        pointer_inside = false;
    }

    element video_overlay::state::hit(LPARAM lparam) const
    {
        float const x = static_cast<float>(GET_X_LPARAM(lparam)) * 96.0F / dpi;
        float const y = static_cast<float>(GET_Y_LPARAM(lparam)) * 96.0F / dpi;
        for (hit_box const &box : boxes)
        {
            if (contains(box.rect, x, y))
            {
                return box.id;
            }
        }
        return element::none;
    }

    void video_overlay::state::run(element id)
    {
        if (!on_command)
        {
            return;
        }
        switch (id)
        {
        case element::prefer_usb:
            if (!model.prefer_usb_link)
            {
                on_command(overlay_command::prefer_usb_link);
            }
            break;
        case element::wifi_only:
            if (model.prefer_usb_link)
            {
                on_command(overlay_command::wifi_only);
            }
            break;
        case element::data_guard:
            on_command(overlay_command::apply_data_guard);
            break;
        case element::enable_adapter:
            on_command(overlay_command::enable_link_adapter);
            break;
        case element::mute:
            on_command(overlay_command::toggle_mute);
            break;
        case element::capture:
            on_command(overlay_command::save_screenshot);
            break;
        case element::more:
            on_command(overlay_command::open_menu);
            break;
        case element::fullscreen:
            on_command(overlay_command::toggle_fullscreen);
            break;
        case element::none:
            break;
        }
    }

    LRESULT video_overlay::state::handle(UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE; // keep keyboard focus (F11, Esc) on the video window
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, hovered != element::none ? IDC_HAND : IDC_ARROW));
            return TRUE;
        case WM_MOUSEMOVE:
        {
            if (!pointer_inside)
            {
                pointer_inside = true;
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = window;
                TrackMouseEvent(&track);
            }
            show(); // cancels a fade-out, restarts the idle timer
            element const now = hit(lparam);
            if (now != hovered)
            {
                hovered = now;
                draw();
                present();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            pointer_inside = false;
            if (hovered != element::none || pressed != element::none)
            {
                hovered = element::none;
                pressed = element::none;
                draw();
                present();
            }
            SetTimer(window, idle_timer_id, idle_hide_ms, nullptr);
            return 0;
        case WM_LBUTTONDOWN:
            pressed = hit(lparam);
            SetCapture(window);
            draw();
            present();
            return 0;
        case WM_LBUTTONUP:
        {
            ReleaseCapture();
            element const released = hit(lparam);
            element const was_pressed = std::exchange(pressed, element::none);
            draw();
            present();
            if (released == was_pressed)
            {
                run(released);
            }
            return 0;
        }
        case WM_RBUTTONUP:
            run(element::more);
            return 0;
        case WM_TIMER:
            if (wparam == idle_timer_id)
            {
                KillTimer(window, idle_timer_id);
                if (pointer_inside || pressed != element::none)
                {
                    SetTimer(window, idle_timer_id, idle_hide_ms, nullptr);
                }
                else
                {
                    begin_fade_out();
                }
                return 0;
            }
            if (wparam == fade_timer_id)
            {
                if (fading_out)
                {
                    alpha -= fade_out_step;
                    if (alpha <= 0)
                    {
                        hide_now();
                        return 0;
                    }
                }
                else
                {
                    alpha = std::min(255, alpha + fade_in_step);
                    if (alpha == 255)
                    {
                        KillTimer(window, fade_timer_id);
                    }
                }
                present();
                return 0;
            }
            if (wparam == notice_hold_timer_id)
            {
                KillTimer(window, notice_hold_timer_id);
                SetTimer(window, notice_fade_timer_id, fade_tick_ms, nullptr);
                return 0;
            }
            if (wparam == notice_fade_timer_id)
            {
                notice_alpha -= fade_out_step;
                if (notice_alpha <= 0)
                {
                    hide_notice();
                }
                else
                {
                    place_notice();
                }
                return 0;
            }
            break;
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    // ---- the notice ------------------------------------------------------------

    void video_overlay::state::release_notice_surface()
    {
        if (notice_dc != nullptr && notice_previous_bitmap != nullptr)
        {
            SelectObject(notice_dc, notice_previous_bitmap);
        }
        if (notice_bitmap != nullptr)
        {
            DeleteObject(notice_bitmap);
        }
        if (notice_dc != nullptr)
        {
            DeleteDC(notice_dc);
        }
        notice_dc = nullptr;
        notice_bitmap = nullptr;
        notice_previous_bitmap = nullptr;
        notice_pixel_width = 0;
        notice_pixel_height = 0;
    }

    void video_overlay::state::hide_notice()
    {
        KillTimer(window, notice_hold_timer_id);
        KillTimer(window, notice_fade_timer_id);
        if (notice_shown && notice_window != nullptr)
        {
            ShowWindow(notice_window, SW_HIDE);
        }
        notice_shown = false;
        notice_alpha = 0;
        release_notice_surface(); // drawn anew for the next text
    }

    // Puts the notice where it belongs on the owner (top, centred) with its
    // present opacity.
    void video_overlay::state::place_notice()
    {
        if (!notice_shown || notice_window == nullptr || notice_dc == nullptr)
        {
            return;
        }
        if (!IsWindowVisible(owner) || IsIconic(owner) != FALSE)
        {
            hide_notice();
            return;
        }
        RECT client{};
        GetClientRect(owner, &client);
        POINT origin{0, 0};
        ClientToScreen(owner, &origin);
        float const scale = static_cast<float>(GetDpiForWindow(owner)) / 96.0F;
        POINT place{origin.x + (static_cast<int>(client.right) - notice_pixel_width) / 2,
                    origin.y + static_cast<int>(std::lround(notice_top_margin * scale))};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = static_cast<BYTE>(std::clamp(notice_alpha, 0, 255));
        blend.AlphaFormat = AC_SRC_ALPHA;
        POINT source{0, 0};
        SIZE size{notice_pixel_width, notice_pixel_height};
        UpdateLayeredWindow(notice_window, nullptr, &place, &size, notice_dc, &source, 0, &blend, ULW_ALPHA);
    }

    void video_overlay::state::show_notice(std::wstring_view text)
    {
        if (text.empty() || target == nullptr || !IsWindowVisible(owner) || IsIconic(owner) != FALSE)
        {
            return;
        }
        if (notice_window == nullptr)
        {
            // lets the mouse through: what is under it keeps getting the clicks
            notice_window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                            overlay_class_name, L"", WS_POPUP, 0, 0, 1, 1, owner, nullptr,
                                            GetModuleHandleW(nullptr), this);
            if (notice_window == nullptr)
            {
                log::warning("notice: {}", make_win32_failure("CreateWindowExW(notice)", GetLastError()).describe());
                return;
            }
        }
        hide_notice();
        float const notice_dpi = static_cast<float>(GetDpiForWindow(owner));
        float const scale = notice_dpi / 96.0F;
        RECT client{};
        GetClientRect(owner, &client);
        float const widest = std::max(static_cast<float>(client.right) / scale - 2.0F * side_margin, 80.0F);
        float const width = std::min(text_width(text, label_format.Get()) + 2.0F * notice_padding, widest);
        int const pixel_w = std::max(static_cast<int>(std::ceil(width * scale)), 1);
        int const pixel_h = std::max(static_cast<int>(std::ceil(notice_height * scale)), 1);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = pixel_w;
        info.bmiHeader.biHeight = -pixel_h; // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        notice_dc = CreateCompatibleDC(nullptr);
        void *bits = nullptr;
        notice_bitmap =
            notice_dc != nullptr ? CreateDIBSection(notice_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (notice_bitmap == nullptr)
        {
            log::warning("notice: {}", make_win32_failure("CreateDIBSection", GetLastError()).describe());
            release_notice_surface();
            return;
        }
        notice_previous_bitmap = SelectObject(notice_dc, notice_bitmap);
        notice_pixel_width = pixel_w;
        notice_pixel_height = pixel_h;

        RECT const bounds{0, 0, pixel_w, pixel_h};
        if (FAILED(target->BindDC(notice_dc, &bounds)))
        {
            release_notice_surface();
            return;
        }
        target->SetDpi(notice_dpi, notice_dpi);
        target->BeginDraw();
        target->Clear(rgba(0.0F, 0.0F, 0.0F, 0.0F));
        D2D1_RECT_F const pill{0.0F, 0.0F, width, notice_height};
        brush->SetColor(rgba(0.07F, 0.07F, 0.07F, 0.86F));
        target->FillRoundedRectangle(rounded(pill, notice_height / 2.0F), brush.Get());
        draw_text(text, label_format.Get(),
                  D2D1_RECT_F{notice_padding / 2.0F, 0.0F, width - notice_padding / 2.0F, notice_height}, white(0.96F),
                  DWRITE_TEXT_ALIGNMENT_CENTER);
        if (target->EndDraw() == D2DERR_RECREATE_TARGET)
        {
            if (status const recreated = create_target(); !recreated)
            {
                log::warning("notice: {}", recreated.error().describe());
            }
            release_notice_surface();
            return;
        }
        notice_shown = true;
        notice_alpha = 255;
        place_notice();
        ShowWindow(notice_window, SW_SHOWNOACTIVATE);
        SetTimer(window, notice_hold_timer_id, notice_hold_ms, nullptr);
    }

    LRESULT CALLBACK video_overlay::state::procedure(HWND handle_window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE)
        {
            auto const *creation = reinterpret_cast<CREATESTRUCTW const *>(lparam);
            SetWindowLongPtrW(handle_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        auto *owner_state = reinterpret_cast<state *>(GetWindowLongPtrW(handle_window, GWLP_USERDATA));
        // (the notice is a window of this class too: it only shows, and its
        // timers run on the bar's window)
        if (owner_state == nullptr || owner_state->window == nullptr || handle_window != owner_state->window)
        {
            return DefWindowProcW(handle_window, message, wparam, lparam);
        }
        return owner_state->handle(message, wparam, lparam);
    }

    video_overlay::video_overlay(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    video_overlay::~video_overlay() = default;

    outcome<std::unique_ptr<video_overlay>> video_overlay::create(void *owner_window,
                                                                  std::function<void(overlay_command command)> on_command)
    {
        auto created = std::make_unique<state>();
        created->owner = static_cast<HWND>(owner_window);
        created->on_command = std::move(on_command);
        if (status const resources = created->create_resources(); !resources)
        {
            return std::unexpected(resources.error());
        }

        static std::once_flag registered;
        std::call_once(registered, []
        {
            WNDCLASSEXW window_class{};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = state::procedure;
            window_class.hInstance = GetModuleHandleW(nullptr);
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.lpszClassName = overlay_class_name;
            RegisterClassExW(&window_class);
        });
        state *const shared = created.get();
        HWND const window = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, overlay_class_name, L"",
                                            WS_POPUP, 0, 0, 1, 1, created->owner, nullptr, GetModuleHandleW(nullptr),
                                            shared);
        if (window == nullptr)
        {
            return std::unexpected(make_win32_failure("CreateWindowExW(control bar)", GetLastError()));
        }
        created->window = window;
        return std::unique_ptr<video_overlay>{new video_overlay{std::move(created)}};
    }

    void video_overlay::set_model(overlay_model model)
    {
        if (model == impl->model)
        {
            return;
        }
        impl->model = std::move(model);
        if (impl->shown)
        {
            impl->draw();
            impl->present();
        }
    }

    void video_overlay::activity()
    {
        impl->show();
    }

    void video_overlay::owner_changed()
    {
        if (impl->shown)
        {
            impl->follow_owner();
        }
        if (!IsWindowVisible(impl->owner) || IsIconic(impl->owner) != FALSE)
        {
            impl->hide_now();
        }
        impl->place_notice(); // follows the owner, or goes with it
    }

    void video_overlay::hide()
    {
        impl->hide_now();
        impl->hide_notice();
    }

    void video_overlay::notice(std::wstring text)
    {
        impl->show_notice(text);
    }
}
