// SPDX-License-Identifier: MIT
// User-interface text (tray menu, control bar, window status, notifications)
// in the Windows display language: Traditional Chinese (zh-TW, zh-HK, zh-MO,
// zh-Hant), Simplified Chinese (zh-CN, zh-SG, zh-Hans, zh) or English (every
// other language).  The language is chosen once, before the engine creates
// its window (native_media_start); log messages stay English.
export module uxplay_ui_text;

import std;
import uxplay_status;

export namespace uxplay
{
    enum class ui_language
    {
        traditional_chinese,
        simplified_chinese,
        english
    };

    enum class ui_text : std::size_t
    {
        // window and tray status
        waiting_for_connection,
        usb_checking,
        usb_charging_only_status,
        usb_not_connected,
        usb_adapter_disabled,
        usb_ready_at,
        usb_turn_on_hotspot,
        // control bar and tray menu
        prefer_usb_link,
        wifi_only,
        protect_mobile_data,
        protect_mobile_data_menu,
        enable_usb_adapter,
        enable_usb_adapter_menu,
        full_screen,
        exit_full_screen,
        more,
        mute,
        unmute,
        always_on_top,
        mode_quality,
        mode_balanced,
        mode_performance,
        start_with_windows,
        open_log,
        about,
        exit,
        // notifications: USB link
        data_guard_offer_title,
        data_guard_offer_text,
        adapter_disabled_title,
        adapter_disabled_text,
        usb_ready_title,
        usb_ready_text,
        usb_ready_text_in_session,
        iphone_attached_title,
        turn_on_hotspot_text,
        adapter_enabled_title,
        adapter_still_disabled_title,
        admin_declined,
        enable_failed,
        data_guard_on_title,
        data_guard_on_text,
        data_guard_off_title,
        apply_failed,
        switched_to_usb_first,
        switched_to_wifi_only,
        choose_receiver_again, // {}: receiver name
        usb_when_hotspot_on,
        usb_charging_only,
        // notifications: mode
        mode_quality_text,
        mode_balanced_text,
        mode_performance_text,
        mode_next_connection,
        // notifications: program
        ready_title,
        already_running_title,
        select_on_iphone, // {}: receiver name
        still_running_title,
        still_running_text,
        start_failed_title,
        start_failed_text, // {}: reason, {}: log file path
        autostart_failed,
        pairing_pin_title,
        about_text, // {}: version, {}: UxPlay core version, {}: project page
        // cropping
        crop_off,
        crop_automatic,
        crop_select,
        crop_select_hint,
        cropped, // {}: size of the part shown
        count
    };

    struct ui_language_detection
    {
        ui_language language = ui_language::english;
        std::wstring windows_language; // first preferred Windows display language, e.g. L"zh-TW"
    };

    // pure mapping of a BCP-47 language name to the interface language
    [[nodiscard]] ui_language ui_language_for(std::wstring_view windows_language);

    // the user's Windows display language (GetUserPreferredUILanguages)
    [[nodiscard]] outcome<ui_language_detection> detect_ui_language();

    // process-wide: set once before the engine starts its threads
    void set_ui_language(ui_language language);
    [[nodiscard]] ui_language current_ui_language();

    // text of the current language (static storage)
    [[nodiscard]] std::wstring_view ui_string(ui_text id);

    // text with its "{}" placeholders filled in, in order
    [[nodiscard]] std::wstring ui_format(ui_text id, std::span<std::wstring_view const> arguments);

    // locale for DirectWrite text formats (selects the matching CJK fallback font)
    [[nodiscard]] std::wstring_view ui_locale_name();

    // short name for the log: "zh-Hant", "zh-Hans" or "en"
    [[nodiscard]] std::string_view ui_language_name(ui_language language);
}
