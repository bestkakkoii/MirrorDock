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
            // window and tray status
            {ui_text::waiting_for_connection, L"等待連線", L"等待连接", L"Waiting for connection"},
            {ui_text::usb_checking, L"USB：檢查中…", L"USB：检查中…", L"USB: checking…"},
            {ui_text::usb_charging_only_status, L"USB：僅充電", L"USB：仅充电", L"USB: charging only"},
            {ui_text::usb_not_connected, L"USB：未接上", L"USB：未接入", L"USB: not plugged in"},
            {ui_text::usb_adapter_disabled, L"USB：網路卡已停用", L"USB：网卡已禁用", L"USB: network adapter disabled"},
            {ui_text::usb_ready_at, L"USB：已就緒", L"USB：已就绪", L"USB: ready"},
            {ui_text::usb_turn_on_hotspot, L"USB：請開啟個人熱點", L"USB：请打开个人热点",
             L"USB: turn on Personal Hotspot"},
            // control bar and tray menu
            {ui_text::prefer_usb_link, L"USB 優先", L"USB 优先", L"Prefer USB"},
            {ui_text::wifi_only, L"僅 Wi-Fi", L"仅 Wi-Fi", L"Wi-Fi only"},
            {ui_text::protect_mobile_data, L"省行動數據", L"节省手机流量", L"Save mobile data"},
            {ui_text::protect_mobile_data_menu, L"省行動數據…", L"节省手机流量…", L"Save mobile data…"},
            {ui_text::enable_usb_adapter, L"啟用 USB 網路卡", L"启用 USB 网卡", L"Enable USB adapter"},
            {ui_text::enable_usb_adapter_menu, L"啟用 USB 網路卡…", L"启用 USB 网卡…", L"Enable USB network adapter…"},
            {ui_text::full_screen, L"全螢幕", L"全屏", L"Full screen"},
            {ui_text::exit_full_screen, L"結束全螢幕", L"退出全屏", L"Exit full screen"},
            {ui_text::more, L"更多", L"更多", L"More"},
            {ui_text::mute, L"靜音", L"静音", L"Mute"},
            {ui_text::unmute, L"取消靜音", L"取消静音", L"Unmute"},
            {ui_text::always_on_top, L"最上層顯示", L"窗口置顶", L"Always on top"},
            {ui_text::mode_quality, L"畫質優先", L"画质优先", L"Quality"},
            {ui_text::mode_balanced, L"平衡（自動調整）", L"均衡（自动调整）", L"Balanced (adaptive)"},
            {ui_text::mode_performance, L"效能全開", L"性能全开", L"Performance"},
            {ui_text::start_with_windows, L"開機時啟動", L"开机时启动", L"Start with Windows"},
            {ui_text::open_log, L"開啟記錄檔", L"打开日志", L"Open log file"},
            {ui_text::about, L"關於…", L"关于…", L"About…"},
            {ui_text::exit, L"結束", L"退出", L"Exit"},
            // notifications: USB link
            {ui_text::data_guard_offer_title, L"電腦正在使用 iPhone 的行動網路", L"电脑正在使用 iPhone 的蜂窝网络",
             L"This PC is using the iPhone's mobile data"},
            {ui_text::data_guard_offer_text,
             L"點此讓電腦上網不再經過 iPhone（需要系統管理員權限）。USB 鏡像不受影響。",
             L"点击此处让电脑上网不再经过 iPhone（需要管理员权限）。USB 镜像不受影响。",
             L"Click to keep this PC's internet off the iPhone (needs administrator permission). "
             L"Mirroring over USB is not affected."},
            {ui_text::adapter_disabled_title, L"USB 網路卡已停用", L"USB 网卡已禁用", L"USB network adapter disabled"},
            {ui_text::adapter_disabled_text, L"點此重新啟用，才能經由 USB 鏡像。", L"点击此处重新启用，才能通过 USB 镜像。",
             L"Click to enable it again for mirroring over USB."},
            {ui_text::usb_ready_title, L"USB 已就緒", L"USB 已就绪", L"USB ready"},
            {ui_text::usb_ready_text, L"螢幕鏡像將經由 USB 線傳輸。", L"屏幕镜像将通过 USB 线传输。",
             L"Screen mirroring will use the USB cable."},
            {ui_text::usb_ready_text_in_session, L"目前的鏡像仍走 Wi-Fi；在 iPhone 重新選擇後會改走 USB。",
             L"当前镜像仍走 Wi-Fi；在 iPhone 上重新选择后会改走 USB。",
             L"This session stays on Wi-Fi; choose the receiver again on the iPhone to use USB."},
            {ui_text::iphone_attached_title, L"iPhone 已接上", L"iPhone 已接入", L"iPhone plugged in"},
            {ui_text::turn_on_hotspot_text, L"開啟「個人熱點」即可經由 USB 鏡像。", L"打开“个人热点”即可通过 USB 镜像。",
             L"Turn on Personal Hotspot to mirror over USB."},
            {ui_text::adapter_enabled_title, L"USB 網路卡已啟用", L"USB 网卡已启用", L"USB network adapter enabled"},
            {ui_text::adapter_still_disabled_title, L"USB 網路卡仍為停用狀態", L"USB 网卡仍处于禁用状态",
             L"USB network adapter is still disabled"},
            {ui_text::admin_declined, L"已取消系統管理員授權。", L"已取消管理员授权。",
             L"Administrator permission was declined."},
            {ui_text::enable_failed, L"啟用失敗，詳見記錄檔。", L"启用失败，详见日志。",
             L"Could not enable it; see the log file."},
            {ui_text::data_guard_on_title, L"電腦上網已不再經過 iPhone", L"电脑上网已不再经过 iPhone",
             L"This PC no longer uses the iPhone for internet"},
            {ui_text::data_guard_on_text, L"USB 線仍可用於鏡像。", L"USB 线仍可用于镜像。",
             L"The USB cable still carries mirroring."},
            {ui_text::data_guard_off_title, L"未能變更電腦的上網路徑", L"未能更改电脑的上网路径",
             L"The PC's internet route was not changed"},
            {ui_text::apply_failed, L"設定失敗，詳見記錄檔。", L"设置失败，详见日志。",
             L"Could not apply it; see the log file."},
            {ui_text::switched_to_usb_first, L"已切換為 USB 優先", L"已切换为 USB 优先", L"Switched to Prefer USB"},
            {ui_text::switched_to_wifi_only, L"已切換為僅 Wi-Fi", L"已切换为仅 Wi-Fi", L"Switched to Wi-Fi only"},
            {ui_text::choose_receiver_again, L"請在 iPhone 重新選擇「{}」。", L"请在 iPhone 上重新选择“{}”。",
             L"Choose \"{}\" again on the iPhone."},
            {ui_text::usb_when_hotspot_on, L"接上 USB 線並開啟「個人熱點」時，鏡像走 USB。",
             L"接上 USB 线并打开“个人热点”时，镜像走 USB。",
             L"Mirroring uses USB while the cable is plugged in and Personal Hotspot is on."},
            {ui_text::usb_charging_only, L"USB 線僅供充電；鏡像一律走 Wi-Fi。", L"USB 线仅用于充电；镜像一律走 Wi-Fi。",
             L"The USB cable only charges; mirroring always uses Wi-Fi."},
            // notifications: mode
            {ui_text::mode_quality_text, L"畫面最細緻：完整解析度，並多保留一點緩衝，不跳過任何畫面。",
             L"画面最细腻：完整分辨率，并多保留一点缓冲，不跳过任何画面。",
             L"The sharpest picture: full resolution and a little more buffering, no frame skipped."},
            {ui_text::mode_balanced_text, L"依螢幕、顯示卡與網路狀況自動調整。", L"根据屏幕、显卡和网络状况自动调整。",
             L"Adapts to the monitor, the GPU and the network."},
            {ui_text::mode_performance_text, L"延遲最低、最省資源：較輕的串流，緩衝減到最少。",
             L"延迟最低、最省资源：较轻的串流，缓冲降到最低。",
             L"The lowest delay and load: a lighter stream and minimal buffering."},
            {ui_text::mode_next_connection, L"串流解析度的變更會在下次連線時生效。", L"串流分辨率的变更会在下次连接时生效。",
             L"The stream resolution changes from the next connection."},
            // notifications: program
            {ui_text::ready_title, L"已就緒", L"已就绪", L"Ready"},
            {ui_text::already_running_title, L"已在執行中", L"已在运行", L"Already running"},
            {ui_text::select_on_iphone, L"在 iPhone 控制中心的「螢幕鏡像輸出」選擇「{}」。",
             L"在 iPhone 控制中心的“屏幕镜像”中选择“{}”。",
             L"On the iPhone, open Control Center > Screen Mirroring and choose \"{}\"."},
            {ui_text::still_running_title, L"仍在背景執行", L"仍在后台运行", L"Still running in the background"},
            {ui_text::still_running_text, L"可從通知區域的圖示結束程式。", L"可从通知区域的图标退出程序。",
             L"Use the notification-area icon to exit."},
            {ui_text::start_failed_title, L"無法啟動", L"无法启动", L"Could not start"},
            {ui_text::start_failed_text, L"{}\n\n詳細資訊請見記錄檔：\n{}", L"{}\n\n详细信息请见日志：\n{}",
             L"{}\n\nDetails are in the log file:\n{}"},
            {ui_text::autostart_failed, L"無法變更「開機時啟動」，詳見記錄檔。", L"无法更改“开机时启动”，详见日志。",
             L"Could not change \"Start with Windows\"; see the log file."},
            {ui_text::pairing_pin_title, L"配對 PIN 碼", L"配对 PIN 码", L"Pairing PIN"},
            {ui_text::about_text,
             L"iPhone／iPad 螢幕鏡像與音訊接收器（Windows 原生）\n\n版本 {}（協定核心：UxPlay {}）\n"
             L"授權：GPL-3.0；媒體引擎為 MIT\n{}",
             L"iPhone／iPad 屏幕镜像与音频接收器（Windows 原生）\n\n版本 {}（协议核心：UxPlay {}）\n"
             L"许可：GPL-3.0；媒体引擎为 MIT\n{}",
             L"Screen mirroring and audio receiver for iPhone and iPad (Windows-native)\n\n"
             L"Version {} (protocol core: UxPlay {})\nLicence: GPL-3.0; media engine MIT\n{}"},
            // cropping
            {ui_text::crop_off, L"不裁切", L"不裁剪", L"No cropping"},
            {ui_text::crop_automatic, L"自動裁切黑邊", L"自动裁剪黑边", L"Crop black bars automatically"},
            {ui_text::crop_select, L"框選裁切範圍…", L"框选裁剪范围…", L"Select the area to show…"},
            {ui_text::crop_select_hint, L"拖曳框出要保留的範圍 · Esc 取消", L"拖动框出要保留的范围 · Esc 取消",
             L"Drag to frame the area to keep · Esc cancels"},
            {ui_text::cropped, L"{} · 已裁切", L"{} · 已裁剪", L"{} · cropped"},
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

    std::wstring ui_format(ui_text id, std::span<std::wstring_view const> arguments)
    {
        std::wstring_view const pattern = ui_string(id);
        std::wstring result;
        result.reserve(pattern.size() + 64);
        std::size_t next_argument = 0;
        for (std::size_t index = 0; index < pattern.size(); ++index)
        {
            if (pattern[index] == L'{' && index + 1 < pattern.size() && pattern[index + 1] == L'}')
            {
                if (next_argument < arguments.size())
                {
                    result += arguments[next_argument];
                }
                ++next_argument;
                ++index;
            }
            else
            {
                result += pattern[index];
            }
        }
        return result;
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
