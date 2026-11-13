// SPDX-License-Identifier: MIT
// "This PC" in the About window: what of the system, the hardware, the
// drivers and the software matters to this program, and how well it suits.
//
// Three steps, kept apart:
//   measure_system / measure_phone   look at the PC (and at an iPhone on the
//                                    cable): facts, no judgement, no text
//   build_report                     judges the facts and words them in one
//                                    interface language: rows with a rating,
//                                    a score per group and an overall score
//   report_text                      the same as plain English text, for the
//                                    clipboard (a bug report)
//
// The score says how well this PC is set up for mirroring with this program
// right now.  It is no benchmark: every row that is rated has a weight, "good"
// counts in full, "attention" half, "problem" not at all, and the score is the
// share of the weight that was reached.  Rows that cannot be rated (no iPhone
// on the cable, the update check never ran) do not count.
//
// Nothing here leaves the PC, and nothing that identifies a device gets into
// the facts: no serial number, no device name, no network address.  (The
// iPhone's identifier is used, as in remote control, to open the session it
// is asked through, and device instance identifiers to find Apple's drivers:
// neither is kept, shown or logged.)
export module uxplay_system_report;

import std;
import uxplay_status;
import uxplay_ui_text;

export namespace uxplay
{
    enum class report_group : std::size_t
    {
        system,
        hardware,
        drivers,
        software,
        count
    };

    enum class report_rating
    {
        good,
        attention,
        problem,
        none // information; does not count in the score
    };

    // What the program can do about a row that is not in order, at the press
    // of a button (uxplay_system_repair, and for the last two the parts of the
    // program they belong to).  A setting that is the user's to choose is
    // never changed: such a button opens the page of Windows' Settings where
    // it is chosen.
    enum class report_action
    {
        none,
        allow_through_firewall, // Windows Firewall lets connections to this program in (administrator)
        open_firewall_settings, // Windows Security: there all incoming connections are blocked
        open_network_settings,  // Settings: the network (connect; make it a private one)
        open_display_settings,  // Settings: advanced display (the refresh rate)
        open_power_settings,    // Settings: power (energy saver)
        open_sound_settings,    // Settings: sound (no output device)
        open_windows_update,    // Settings: Windows Update (Windows itself, the graphics driver)
        install_apple_support,  // Apple's USB support: the installer that comes with the program, or the Store
        start_apple_service,    // Apple Mobile Device Service (administrator)
        enable_usb_adapter,     // the iPhone's network adapter, disabled in Windows (uxplay_usb_link)
        install_update          // the newer release (uxplay_update_install)
    };

    struct report_entry
    {
        report_group group = report_group::system;
        ui_text label = ui_text::about;
        std::wstring value;
        report_rating rating = report_rating::none;
        unsigned int weight = 0; // its share in the score (0 for a row that is not rated)
        std::wstring advice;     // what to do about a row that is not good; else empty
        // the button beside that advice: what it does and what it says (none, empty: no button)
        report_action action = report_action::none;
        std::wstring action_label;
    };

    struct system_report
    {
        std::vector<report_entry> entries; // in the order shown, group by group
        std::array<std::optional<int>, static_cast<std::size_t>(report_group::count)> group_scores;
        std::optional<int> score; // 0..100; nothing while no row is rated
    };

    // ---- facts -----------------------------------------------------------------

    struct graphics_adapter_facts
    {
        std::wstring name;
        std::uint64_t dedicated_memory = 0;
        bool in_use = false;   // the one the engine decodes and shows with
        bool software = false; // Windows' software renderer (no graphics card with a working driver)
        std::wstring driver_version;
        std::wstring driver_date; // ISO 8601
    };

    // a monitor that shows the desktop the program's window opens on
    struct display_facts
    {
        std::wstring name; // empty: the monitor gives none
        bool built_in = false;
        double hz = 0.0;
    };

    enum class network_category
    {
        unknown,
        public_network,
        private_network,
        domain_network
    };

    enum class firewall_verdict
    {
        unknown,
        off,
        allowed,
        blocked, // a rule keeps connections to this program out
        no_rule,
        shut     // every incoming connection is blocked, whatever the rules say
    };

    // Windows Firewall and this program, as far as a repair needs it
    // (bits: NET_FW_PROFILE2_DOMAIN 1, _PRIVATE 2, _PUBLIC 4).
    struct firewall_facts
    {
        firewall_verdict verdict = firewall_verdict::unknown;
        long active_profiles = 0;   // of the networks the PC is on now
        long allowing_profiles = 0; // in which an enabled rule lets connections to this program in
        bool inbound_rule = false;  // a rule for connections to this program exists (allowing or blocking)
    };

    // Looks at the firewall's rules for this executable.  COM must be
    // initialized on the calling thread.
    [[nodiscard]] firewall_facts measure_firewall_rules();

    enum class phone_condition
    {
        not_asked,   // measure_phone has not run yet
        absent,      // no iPhone on the cable (or no Apple service to ask)
        not_trusted, // it does not trust this PC
        unreachable, // it is there and does not answer
        present
    };

    enum class update_knowledge
    {
        unknown,
        up_to_date,
        newer_available
    };

    // What only the running program knows; the caller fills it in.
    struct program_facts
    {
        std::wstring version;      // "1.11.0"
        // the graphics card the engine uses (name as DXGI gives it)
        std::wstring adapter_name;
        bool software_renderer = false;
        bool h264_hardware = false;
        bool hevc_hardware = false;
        // The state of the iPhone's USB network link, as one of the status
        // texts the menu shows (nothing: no link monitor), and for
        // ui_text::usb_ready_at the PC's address on the link.
        std::optional<ui_text> usb_link;
        std::wstring usb_link_address;
        // the user wants USB first, and its adapter is disabled in Windows
        bool usb_adapter_disabled = false;
        update_knowledge update = update_knowledge::unknown;
        std::wstring newest_version;
        // a stream is on screen: "USB", "H.265", size, and how it is decoded
        bool stream_active = false;
        std::wstring stream_link;
        std::wstring stream_codec;
        int stream_width = 0;
        int stream_height = 0;
        bool stream_hardware_decoding = false;
    };

    struct system_facts
    {
        program_facts program;
        // system
        std::wstring windows_name; // "Windows 11 Pro 24H2"
        std::uint32_t windows_build = 0;
        std::uint32_t windows_revision = 0;
        int display_width = 0;
        int display_height = 0;
        int display_scale_percent = 0;
        std::vector<display_facts> displays; // more than one: the desktop is duplicated
        bool battery_present = false;
        bool on_battery = false;
        bool energy_saver = false;
        int battery_percent = -1; // -1: not known
        bool network_connected = false;
        std::wstring network_name; // "Wi-Fi · Intel(R) Wi-Fi 6 AX200"
        std::uint64_t network_bits_per_second = 0;
        network_category network = network_category::unknown;
        std::optional<bool> metered;
        firewall_verdict firewall = firewall_verdict::unknown;
        // hardware
        std::wstring processor_name;
        unsigned int processor_cores = 0;
        unsigned int processor_threads = 0;
        std::uint64_t memory_total = 0;     // bytes
        std::uint64_t memory_available = 0; // bytes
        std::vector<graphics_adapter_facts> adapters; // the one in use first
        bool sound_device = false;
        std::wstring sound_name;
        int sound_rate = 0;
        // Apple's USB support
        bool apple_support_installed = false;
        std::wstring apple_support_version;
        bool apple_service_reachable = false;
        std::wstring apple_usb_driver;     // "Apple Mobile Device USB Composite Device 538.0.0.0 (2023-06-14)"
        std::wstring apple_network_driver; // the same for "Apple Mobile Device Ethernet"
        // the iPhone on the cable
        phone_condition phone = phone_condition::not_asked;
        std::wstring phone_model;  // "iPhone17,2"
        std::wstring phone_system; // "27.2.1"
        std::optional<bool> developer_mode;
    };

    // Looks at this PC.  Blocks for some hundred milliseconds; COM is entered
    // and left inside.  What cannot be found out stays at its default.
    [[nodiscard]] system_facts measure_system(program_facts program);

    // Asks the iPhone on the cable for its model, its iOS version and
    // Developer Mode.  Blocks for up to some seconds.
    void measure_phone(system_facts &facts);

    [[nodiscard]] system_report build_report(system_facts const &facts, ui_language language);

    // The report as plain text in English, with the program's version on top.
    [[nodiscard]] std::wstring report_text(system_facts const &facts);

    // Measures on a thread of its own and hands the facts over twice: first
    // without the iPhone (phone_condition::not_asked), then with it.  A request
    // that arrives while one is measured replaces it: its facts are the ones
    // handed over.
    class report_collector
    {
    public:
        // on_facts: called on the collector's thread; must not block
        [[nodiscard]] static std::unique_ptr<report_collector> create(
            std::function<void(system_facts const &facts)> on_facts);

        // Waits for the look at this PC that is in progress (some hundred
        // milliseconds), not for the iPhone: a question to it that is still
        // open is left to end alone and its answer is dropped.
        ~report_collector();
        report_collector(report_collector const &) = delete;
        report_collector &operator=(report_collector const &) = delete;

        void request(program_facts program);

    private:
        struct state;

        explicit report_collector(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
