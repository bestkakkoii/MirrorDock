// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

module uxplay_ui_text;

import std;
import uxplay_status;

namespace uxplay
{
    namespace
    {
        struct text_row
        {
            ui_text id;
            std::wstring_view traditional_chinese;
            std::wstring_view simplified_chinese;
            std::wstring_view english;
        };

        constexpr std::array<text_row, static_cast<std::size_t>(ui_text::count)> text_table{{
            {ui_text::waiting_for_connection, L"等待連線", L"等待连接", L"Waiting for connection"},
            {ui_text::usb_checking, L"USB：檢查中…", L"USB：检查中…", L"USB: checking…"},
            {ui_text::usb_connected_unused, L"USB：已連線（未使用）", L"USB：已连接（未使用）",
             L"USB: connected (not used)"},
            {ui_text::usb_attached_charging_only, L"USB：已連接（僅充電）", L"USB：已接入（仅充电）",
             L"USB: attached (charging only)"},
            {ui_text::usb_not_connected, L"USB：未連接", L"USB：未连接", L"USB: not connected"},
            {ui_text::usb_adapter_disabled, L"USB：網路卡已停用", L"USB：网卡已禁用", L"USB: network adapter disabled"},
            {ui_text::usb_connected_at, L"USB：已連線", L"USB：已连接", L"USB: connected"},
            {ui_text::usb_turn_on_hotspot, L"USB：請開啟個人熱點", L"USB：请打开个人热点",
             L"USB: turn on Personal Hotspot"},
            {ui_text::prefer_usb_link, L"USB 優先", L"USB 优先", L"USB first"},
            {ui_text::wifi_only, L"僅 Wi-Fi", L"仅 Wi-Fi", L"Wi-Fi only"},
            {ui_text::protect_mobile_data, L"保護行動數據", L"保护移动数据", L"Protect mobile data"},
            {ui_text::protect_mobile_data_menu, L"保護行動數據…", L"保护移动数据…", L"Protect mobile data…"},
            {ui_text::enable_usb_adapter, L"啟用 USB 網路卡", L"启用 USB 网卡", L"Enable USB adapter"},
            {ui_text::enable_usb_adapter_menu, L"啟用 USB 網路卡…", L"启用 USB 网卡…", L"Enable USB network adapter…"},
            {ui_text::full_screen, L"全螢幕", L"全屏", L"Full screen"},
            {ui_text::exit_full_screen, L"結束全螢幕", L"退出全屏", L"Exit full screen"},
            {ui_text::more, L"更多", L"更多", L"More"},
            {ui_text::mute, L"靜音", L"静音", L"Mute"},
            {ui_text::unmute, L"取消靜音", L"取消静音", L"Unmute"},
            {ui_text::open_log, L"記錄檔", L"日志", L"Log file"},
            {ui_text::exit, L"結束", L"退出", L"Exit"},
            {ui_text::data_guard_offer_title, L"行動數據保護", L"移动数据保护", L"Mobile data protection"},
            {ui_text::data_guard_offer_text, L"電腦正經由 iPhone 行動數據上網。點此改為僅使用 Wi-Fi。",
             L"电脑正在通过 iPhone 移动数据上网。点击此处改为仅使用 Wi-Fi。",
             L"This PC is using the iPhone's mobile data for internet. Click to use Wi-Fi only."},
            {ui_text::adapter_disabled_title, L"USB 網路卡已停用", L"USB 网卡已禁用", L"USB network adapter disabled"},
            {ui_text::adapter_disabled_text, L"點此重新啟用，以使用 USB 投影。", L"点击此处重新启用，以使用 USB 投屏。",
             L"Click to enable it again for mirroring over USB."},
            {ui_text::usb_connected_title, L"USB 已連線", L"USB 已连接", L"USB connected"},
            {ui_text::usb_connected_text, L"螢幕鏡像將經由 USB 傳輸。", L"屏幕镜像将通过 USB 传输。",
             L"Screen mirroring will use the USB cable."},
            {ui_text::iphone_attached_title, L"iPhone 已連接", L"iPhone 已连接", L"iPhone connected"},
            {ui_text::iphone_attached_text, L"開啟「個人熱點」即可使用 USB 投影。", L"打开“个人热点”即可使用 USB 投屏。",
             L"Turn on Personal Hotspot to mirror over USB."},
            {ui_text::adapter_enabled_title, L"USB 網路卡已啟用", L"USB 网卡已启用", L"USB network adapter enabled"},
            {ui_text::adapter_enabled_text, L"開啟個人熱點後即可使用 USB 投影。", L"打开个人热点后即可使用 USB 投屏。",
             L"Turn on Personal Hotspot to mirror over USB."},
            {ui_text::adapter_still_disabled_title, L"USB 網路卡仍停用", L"USB 网卡仍处于禁用状态",
             L"USB network adapter still disabled"},
            {ui_text::admin_declined, L"已取消系統管理員授權。", L"已取消管理员授权。",
             L"Administrator permission was declined."},
            {ui_text::enable_failed, L"啟用失敗，詳見記錄檔。", L"启用失败，详见日志。",
             L"Could not enable it; see the log file."},
            {ui_text::data_guard_on_title, L"行動數據保護已啟用", L"移动数据保护已启用", L"Mobile data protection on"},
            {ui_text::data_guard_on_text, L"電腦上網僅使用 Wi-Fi。", L"电脑上网仅使用 Wi-Fi。",
             L"This PC uses only Wi-Fi for internet."},
            {ui_text::data_guard_off_title, L"行動數據保護未啟用", L"移动数据保护未启用",
             L"Mobile data protection not applied"},
            {ui_text::apply_failed, L"設定失敗，詳見記錄檔。", L"设置失败，详见日志。",
             L"Could not apply it; see the log file."},
            {ui_text::switched_to_usb_first, L"已切換為 USB 優先", L"已切换为 USB 优先", L"Switched to USB first"},
            {ui_text::switched_to_wifi_only, L"已切換為僅 Wi-Fi", L"已切换为仅 Wi-Fi", L"Switched to Wi-Fi only"},
            {ui_text::choose_uxplay_again, L"請在 iPhone 重新選擇 UxPlay。", L"请在 iPhone 上重新选择 UxPlay。",
             L"Choose UxPlay again on the iPhone."},
            {ui_text::usb_when_hotspot_on, L"連接 USB 並開啟個人熱點時使用 USB。", L"连接 USB 并打开个人热点时使用 USB。",
             L"USB is used while the cable is connected and Personal Hotspot is on."},
            {ui_text::usb_charging_only, L"USB 連線僅供充電。", L"USB 连接仅用于充电。",
             L"The USB cable is used only for charging."},
        }};

        consteval bool text_table_is_complete()
        {
            for (std::size_t index = 0; index < text_table.size(); ++index)
            {
                text_row const &row = text_table[index];
                if (static_cast<std::size_t>(row.id) != index || row.traditional_chinese.empty() ||
                    row.simplified_chinese.empty() || row.english.empty())
                {
                    return false;
                }
            }
            return true;
        }
        static_assert(text_table_is_complete(),
                      "every ui_text needs one row, in enum order, with all three languages");

        // set once by native_media_start before the engine threads exist
        std::atomic<ui_language> active_language{ui_language::english};

        bool equals_ascii_lowercase(std::wstring_view subtag, std::wstring_view lowercase)
        {
            if (subtag.size() != lowercase.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < subtag.size(); ++index)
            {
                wchar_t const character = subtag[index];
                wchar_t const lowered = character >= L'A' && character <= L'Z'
                                            ? static_cast<wchar_t>(character - L'A' + L'a')
                                            : character;
                if (lowered != lowercase[index])
                {
                    return false;
                }
            }
            return true;
        }
    }

    ui_language ui_language_for(std::wstring_view windows_language)
    {
        // BCP-47: language[-script][-region]...; '_' accepted as a separator too
        std::vector<std::wstring_view> subtags;
        std::size_t start = 0;
        while (start <= windows_language.size())
        {
            std::size_t const end = windows_language.find_first_of(L"-_", start);
            std::size_t const stop = end == std::wstring_view::npos ? windows_language.size() : end;
            subtags.push_back(windows_language.substr(start, stop - start));
            if (end == std::wstring_view::npos)
            {
                break;
            }
            start = end + 1;
        }
        if (subtags.empty() || !equals_ascii_lowercase(subtags.front(), L"zh"))
        {
            return ui_language::english;
        }
        // the script subtag decides; otherwise Taiwan, Hong Kong and Macao write Traditional Chinese
        for (std::wstring_view const subtag : std::span{subtags}.subspan(1))
        {
            if (equals_ascii_lowercase(subtag, L"hant"))
            {
                return ui_language::traditional_chinese;
            }
            if (equals_ascii_lowercase(subtag, L"hans"))
            {
                return ui_language::simplified_chinese;
            }
        }
        for (std::wstring_view const subtag : std::span{subtags}.subspan(1))
        {
            if (equals_ascii_lowercase(subtag, L"tw") || equals_ascii_lowercase(subtag, L"hk") ||
                equals_ascii_lowercase(subtag, L"mo"))
            {
                return ui_language::traditional_chinese;
            }
        }
        return ui_language::simplified_chinese;
    }

    outcome<ui_language_detection> detect_ui_language()
    {
        ULONG language_count = 0;
        ULONG buffer_length = 0;
        if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &language_count, nullptr, &buffer_length))
        {
            return std::unexpected(make_win32_failure("GetUserPreferredUILanguages (size)", GetLastError()));
        }
        std::wstring buffer(buffer_length, L'\0');
        if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &language_count, buffer.data(), &buffer_length))
        {
            return std::unexpected(make_win32_failure("GetUserPreferredUILanguages", GetLastError()));
        }
        // a multi-string: the first (most preferred) name ends at the first NUL
        std::wstring first{buffer.c_str()};
        if (language_count == 0 || first.empty())
        {
            return std::unexpected(make_win32_failure("GetUserPreferredUILanguages (empty list)", ERROR_NO_DATA));
        }
        ui_language_detection detection;
        detection.language = ui_language_for(first);
        detection.windows_language = std::move(first);
        return detection;
    }

    void set_ui_language(ui_language language)
    {
        active_language.store(language, std::memory_order_relaxed);
    }

    ui_language current_ui_language()
    {
        return active_language.load(std::memory_order_relaxed);
    }

    std::wstring_view ui_string(ui_text id)
    {
        auto const index = static_cast<std::size_t>(id);
        if (index >= text_table.size())
        {
            return L"?";
        }
        text_row const &row = text_table[index];
        switch (current_ui_language())
        {
        case ui_language::traditional_chinese:
            return row.traditional_chinese;
        case ui_language::simplified_chinese:
            return row.simplified_chinese;
        case ui_language::english:
            break;
        }
        return row.english;
    }

    std::wstring_view ui_locale_name()
    {
        switch (current_ui_language())
        {
        case ui_language::traditional_chinese:
            return L"zh-TW";
        case ui_language::simplified_chinese:
            return L"zh-CN";
        case ui_language::english:
            break;
        }
        return L"en-US";
    }

    std::string_view ui_language_name(ui_language language)
    {
        switch (language)
        {
        case ui_language::traditional_chinese:
            return "zh-Hant";
        case ui_language::simplified_chinese:
            return "zh-Hans";
        case ui_language::english:
            break;
        }
        return "en";
    }
}
