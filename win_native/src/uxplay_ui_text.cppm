// SPDX-License-Identifier: MIT
// User-interface text (tray menu, control bar, window status, notifications,
// the About window) in the Windows display language: Traditional Chinese (zh-TW, zh-HK, zh-MO,
// zh-Hant), Simplified Chinese (zh-CN, zh-SG, zh-Hans, zh), Japanese, Korean,
// Spanish, German, French, Vietnamese, Thai, Russian, or English (also for
// every other language).  The language is chosen once, before the engine creates its
// window (native_media_start); log messages stay English.
export module uxplay_ui_text;

import std;
import uxplay_status;

export namespace uxplay
{
    // The order is the order of the texts in every row of the table, and
    // (plus one) the numbering of native_media_settings::ui_language.
    enum class ui_language : std::size_t
    {
        traditional_chinese,
        simplified_chinese,
        english,
        japanese,
        korean,
        spanish,
        german,
        french,
        vietnamese,
        thai,
        russian,
        count
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
        // cropping
        crop_off,
        crop_automatic,
        crop_select,
        crop_select_hint,
        cropped, // {}: size of the part shown
        // remote control of the phone
        control_phone,
        control_connecting,
        control_ready_title,
        control_ready_text,
        control_preparing_title,
        control_preparing_text,
        control_failed_title,
        control_needs_cable,
        control_needs_developer_mode,
        control_image_failed,
        control_camera_in_use,
        control_needs_ios,
        control_failed_text,
        controlled, // {}: what the window shows (link and size)
        // update check: menu and notification
        check_for_updates,
        update_available,      // {}: the newer version
        update_available_text, // {}: the version that runs
        // About window: frame and first page
        about_title,
        about_tab_overview,
        about_tab_this_pc,
        about_tab_components,
        about_tagline,
        about_version, // {}: version, {}: release date
        about_licence, // {}: UxPlay core version
        about_trademarks,
        about_updates,
        update_not_checked,
        update_checking,
        update_up_to_date,
        update_released,  // {}: release date of the newer version, {}: the version that runs
        update_newest_is, // {}: version (and date) of the newest release
        update_failed,
        update_failed_text,
        update_last_checked, // {}: date and time
        update_check_now,
        update_download,
        update_whats_new,
        update_automatic,
        update_automatic_note,
        about_links,
        link_project,
        link_releases,
        link_report_problem,
        link_user_guide,
        link_licences,
        // About window: "This PC" (uxplay_system_report)
        score_title,
        score_details,
        score_measuring,
        score_excellent,
        score_good,
        score_fair,
        score_poor,
        report_copy,
        report_copied,
        report_again,
        group_system,
        group_hardware,
        group_drivers,
        group_software,
        row_windows,
        row_display,
        row_power,
        row_network,
        row_firewall,
        row_processor,
        row_memory,
        row_graphics,
        row_other_graphics,
        row_decoding,
        row_sound,
        row_graphics_driver,
        row_apple_support,
        row_apple_usb_driver,
        row_apple_network_driver,
        row_usb_link,
        row_program,
        row_stream,
        row_iphone,
        value_display, // {}: width, {}: height, {}: scale in per cent
        value_built_in_display,
        value_external_display,
        value_plugged_in,
        value_on_battery,   // {}: charge in per cent
        value_energy_saver, // {}: charge in per cent
        value_no_battery,
        value_not_connected,
        value_private_network,
        value_public_network,
        value_domain_network,
        value_metered,
        value_firewall_allows,
        value_firewall_blocks,
        value_firewall_no_rule,
        value_firewall_off,
        value_processor, // {}: cores, {}: threads
        value_memory,    // {}: installed, {}: free (GB)
        value_in_use,
        value_decoding, // {}: how H.265 is decoded, {}: how H.264 is (value_hardware / value_software)
        value_hardware,
        value_software,
        value_none,
        value_service_running,
        value_service_stopped,
        value_not_installed,
        value_up_to_date,
        value_newer, // {}: the newer version
        value_hardware_decoding,
        value_software_decoding,
        value_checking,
        value_no_iphone,
        value_not_trusted,
        value_developer_mode_on,
        value_developer_mode_off,
        advice_windows,
        advice_display,
        advice_display_duplicate,
        advice_energy_saver,
        advice_no_network,
        advice_public_network,
        advice_slow_network,
        advice_firewall,
        advice_processor,
        advice_memory,
        advice_software_renderer,
        advice_h264_only,
        advice_no_hardware_decoding,
        advice_no_sound,
        advice_apple_support,
        advice_apple_service,
        advice_usb_adapter,
        advice_update,
        // About window: "Components"
        components_libraries,
        components_tools,
        components_run_time,
        components_system,
        components_credits,
        components_releases,
        components_dates_note,
        column_component,
        column_version,
        column_date,
        column_licence,
        screenshot,
        screenshot_saved_copied,
        screenshot_saved,
        screenshot_failed,
        update_install,
        update_retry,
        update_downloading,
        update_unpacking,
        update_ready,
        update_ready_text,
        update_restart_now,
        update_restarting,
        update_install_failed,
        update_problem_no_signature,
        update_problem_download,
        update_problem_not_genuine,
        update_problem_package,
        update_problem_folder,
        update_problem_other,
        update_installed_title,
        update_installed_text,
        fix_allow_firewall,
        fix_open_firewall,
        fix_open_network,
        fix_open_display,
        fix_open_power,
        fix_open_sound,
        fix_open_windows_update,
        fix_install_apple_support,
        fix_start_apple_service,
        fix_failed_text,
        value_firewall_shut,
        advice_firewall_shut,
        count
    };

    struct ui_language_detection
    {
        ui_language language = ui_language::english;
        std::wstring windows_language; // first preferred Windows display language, e.g. L"zh-TW"
    };

    // pure mapping of a BCP-47 language name to the interface language
    [[nodiscard]] ui_language ui_language_for(std::wstring_view windows_language);

    // the language of an option value such as "ja" or "zh-TW" (the codes of
    // ui_language_name, also "zh-TW"/"zh-CN"; case-insensitive), if it is one
    [[nodiscard]] std::optional<ui_language> ui_language_from_code(std::string_view code);

    // the user's Windows display language (GetUserPreferredUILanguages)
    [[nodiscard]] outcome<ui_language_detection> detect_ui_language();

    // process-wide: set once before the engine starts its threads
    void set_ui_language(ui_language language);
    [[nodiscard]] ui_language current_ui_language();

    // text of the current language (static storage)
    [[nodiscard]] std::wstring_view ui_string(ui_text id);
    // text of one language, whatever the current one is (a report in English)
    [[nodiscard]] std::wstring_view ui_string(ui_text id, ui_language language);

    // text with its "{}" placeholders filled in, in order
    [[nodiscard]] std::wstring ui_format(ui_text id, std::span<std::wstring_view const> arguments);
    [[nodiscard]] std::wstring ui_format(ui_text id, ui_language language,
                                         std::span<std::wstring_view const> arguments);

    // locale for DirectWrite text formats (selects the matching CJK fallback font)
    [[nodiscard]] std::wstring_view ui_locale_name();

    // short name for the log: "zh-Hant", "zh-Hans", "en", "ja", "ko", "es",
    // "de", "fr", "vi", "th" or "ru"
    [[nodiscard]] std::string_view ui_language_name(ui_language language);
}
