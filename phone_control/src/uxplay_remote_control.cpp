// SPDX-License-Identifier: GPL-3.0-or-later
module uxplay_remote_control;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_byte_stream;
import uxplay_property_list;
import uxplay_usbmux;
import uxplay_lockdown;
import uxplay_device_tunnel;
import uxplay_remote_xpc;
import uxplay_core_device;
import uxplay_cryptex;
import uxplay_developer_image;

// The report layouts and the registration of the keyboard follow what the
// pymobiledevice3 project found out about them
// (https://github.com/doronz88/pymobiledevice3, GPL-3.0).
namespace uxplay
{
    namespace
    {
        // ---- the phone's services ----------------------------------------------------------
        constexpr std::string_view input_service_name = "com.apple.coredevice.hid.universalhidservice";
        constexpr std::string_view input_feature = "com.apple.coredevice.feature.remote.universalhidservice";
        constexpr std::string_view button_service_name = "com.apple.coredevice.hid.indigo";
        constexpr std::string_view button_feature = "com.apple.coredevice.feature.remote.hid.button";
        constexpr std::string_view orientation_service_name = "com.apple.springboardservices";
        constexpr std::string_view developer_mode_service_name = "com.apple.amfi.lockdown";
        constexpr std::string_view developer_mode_domain = "com.apple.security.mac.amfi";
        // the stream of the screen that lets input through exists from this version of iOS on
        constexpr int first_ios_with_input = 27;

        // the touch screen as the phone registers it (a phone with one screen)
        constexpr std::uint64_t touch_screen_service = 257;
        // The keyboard is a service this end registers; the high bit marks an
        // identifier chosen by a client.
        constexpr std::uint64_t keyboard_service = 0x100002001ULL;

        constexpr std::size_t touch_report_bytes = 58;
        constexpr std::uint8_t touch_report_identifier = 0x09;
        constexpr std::size_t touch_contacts = 5; // the slots of a touch report
        constexpr std::size_t keyboard_report_bytes = 39;
        constexpr std::uint8_t keyboard_report_identifier = 0x01;
        constexpr unsigned keyboard_usages = 240;       // the bits of a keyboard report
        constexpr std::uint8_t contact_touching = 0xC0; // "touching" and "in range"
        constexpr std::uint64_t button_down = 1;
        constexpr std::uint64_t button_up = 2;

        // The phone matches its input surfaces against the screen stream a
        // moment after the stream started; a touch sent before that is dropped.
        constexpr auto input_settle = std::chrono::milliseconds{500};
        // ... and attaches a newly registered keyboard a moment after it was
        // registered; a Shift that arrives before that stays latched.
        constexpr auto keyboard_settle = std::chrono::milliseconds{300};
        constexpr auto call_patience = std::chrono::seconds{15};
        constexpr auto keyboard_patience = std::chrono::seconds{5};
        constexpr auto stop_patience = std::chrono::seconds{4};
        // which way the phone is held is asked this often, and at once when the picture changed
        constexpr auto orientation_interval = std::chrono::milliseconds{1000};
        // ... and this much later after it did not say
        constexpr auto orientation_retry = std::chrono::seconds{5};
        // how much of the screen stream arrived is said in the log this often (debug level)
        constexpr auto stream_watch_interval = std::chrono::seconds{5};
        // the services of a Developer Disk Image that was just installed are
        // listed a moment later: the phone's directory is read again this often, this many times
        constexpr auto image_services_step = std::chrono::milliseconds{500};
        constexpr int image_services_attempts = 20;

        // The fingers 0..3 are the user's.  A scroll or a swipe is a finger of
        // this end's own: a scroll rests before it lifts, so that the content
        // stops where it was dragged to; a swipe lifts at once and is flung on.
        constexpr int user_fingers = 4;
        constexpr int gesture_finger = 4;
        constexpr auto scroll_travel = std::chrono::milliseconds{80};
        constexpr auto scroll_rest = std::chrono::milliseconds{70};
        constexpr auto swipe_travel = std::chrono::milliseconds{100};
        constexpr auto gesture_step = std::chrono::milliseconds{10};
        // shorter than this the phone takes it for a tap (about 1.5 % of the screen)
        constexpr int shortest_gesture = 1000;
        constexpr int full_scale = 65535;

        // waiting commands; more means the phone has stopped taking input
        constexpr std::size_t longest_queue = 512;

        // a HID keyboard as the phone expects one to describe itself
        constexpr auto keyboard_descriptor = std::to_array<std::uint8_t>(
            {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x95,
             0x08, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x05, 0x07, 0x19, 0x00, 0x29, 0xFF,
             0x15, 0x00, 0x26, 0xFF, 0x00, 0x95, 0x06, 0x75, 0x08, 0x81, 0x00, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
             0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0xC0});

        struct button_code
        {
            phone_button button;
            std::uint64_t usage_page; // the Consumer page, as the phone's own buttons report
            std::uint64_t usage;
            std::chrono::milliseconds held;
        };

        constexpr std::array button_codes{
            button_code{phone_button::home, 0x0C, 0x40, std::chrono::milliseconds{50}},
            button_code{phone_button::lock, 0x0C, 0x30, std::chrono::milliseconds{500}},
            button_code{phone_button::volume_up, 0x0C, 0xE9, std::chrono::milliseconds{50}},
            button_code{phone_button::volume_down, 0x0C, 0xEA, std::chrono::milliseconds{50}},
            button_code{phone_button::mute, 0x0C, 0xE2, std::chrono::milliseconds{50}},
        };

        struct problem_name
        {
            remote_control_problem problem;
            std::string_view name;
        };

        constexpr std::array problem_names{
            problem_name{remote_control_problem::none, "none"},
            problem_name{remote_control_problem::no_device, "no_device"},
            problem_name{remote_control_problem::not_trusted, "not_trusted"},
            problem_name{remote_control_problem::developer_mode_off, "developer_mode_off"},
            problem_name{remote_control_problem::image_mount_failed, "image_mount_failed"},
            problem_name{remote_control_problem::tunnel_failed, "tunnel_failed"},
            problem_name{remote_control_problem::unsupported_ios, "unsupported_ios"},
            problem_name{remote_control_problem::media_in_use, "media_in_use"},
            problem_name{remote_control_problem::stream_failed, "stream_failed"},
            problem_name{remote_control_problem::connection_lost, "connection_lost"},
            problem_name{remote_control_problem::internal, "internal"},
        };

        struct scan_code_usage
        {
            std::uint8_t scan_code;
            std::uint8_t usage;
        };

        // PS/2 set 1 scan codes (what Windows reports) to HID usage page 7
        constexpr std::array plain_keys{
            scan_code_usage{0x01, 0x29}, // Esc
            scan_code_usage{0x02, 0x1E}, scan_code_usage{0x03, 0x1F}, scan_code_usage{0x04, 0x20},
            scan_code_usage{0x05, 0x21}, scan_code_usage{0x06, 0x22}, scan_code_usage{0x07, 0x23},
            scan_code_usage{0x08, 0x24}, scan_code_usage{0x09, 0x25}, scan_code_usage{0x0A, 0x26},
            scan_code_usage{0x0B, 0x27}, // 1 .. 0
            scan_code_usage{0x0C, 0x2D}, scan_code_usage{0x0D, 0x2E}, // - =
            scan_code_usage{0x0E, 0x2A}, scan_code_usage{0x0F, 0x2B}, // Backspace Tab
            scan_code_usage{0x10, 0x14}, scan_code_usage{0x11, 0x1A}, scan_code_usage{0x12, 0x08},
            scan_code_usage{0x13, 0x15}, scan_code_usage{0x14, 0x17}, scan_code_usage{0x15, 0x1C},
            scan_code_usage{0x16, 0x18}, scan_code_usage{0x17, 0x0C}, scan_code_usage{0x18, 0x12},
            scan_code_usage{0x19, 0x13}, // Q W E R T Y U I O P
            scan_code_usage{0x1A, 0x2F}, scan_code_usage{0x1B, 0x30}, // [ ]
            scan_code_usage{0x1C, 0x28}, scan_code_usage{0x1D, 0xE0}, // Enter, left Ctrl
            scan_code_usage{0x1E, 0x04}, scan_code_usage{0x1F, 0x16}, scan_code_usage{0x20, 0x07},
            scan_code_usage{0x21, 0x09}, scan_code_usage{0x22, 0x0A}, scan_code_usage{0x23, 0x0B},
            scan_code_usage{0x24, 0x0D}, scan_code_usage{0x25, 0x0E}, scan_code_usage{0x26, 0x0F}, // A .. L
            scan_code_usage{0x27, 0x33}, scan_code_usage{0x28, 0x34}, scan_code_usage{0x29, 0x35}, // ; ' `
            scan_code_usage{0x2A, 0xE1}, scan_code_usage{0x2B, 0x31}, // left Shift, backslash
            scan_code_usage{0x2C, 0x1D}, scan_code_usage{0x2D, 0x1B}, scan_code_usage{0x2E, 0x06},
            scan_code_usage{0x2F, 0x19}, scan_code_usage{0x30, 0x05}, scan_code_usage{0x31, 0x11},
            scan_code_usage{0x32, 0x10}, // Z X C V B N M
            scan_code_usage{0x33, 0x36}, scan_code_usage{0x34, 0x37}, scan_code_usage{0x35, 0x38}, // , . /
            scan_code_usage{0x36, 0xE5}, scan_code_usage{0x37, 0x55}, // right Shift, keypad *
            scan_code_usage{0x38, 0xE2}, scan_code_usage{0x39, 0x2C}, scan_code_usage{0x3A, 0x39}, // left Alt, Space, Caps Lock
            scan_code_usage{0x3B, 0x3A}, scan_code_usage{0x3C, 0x3B}, scan_code_usage{0x3D, 0x3C},
            scan_code_usage{0x3E, 0x3D}, scan_code_usage{0x3F, 0x3E}, scan_code_usage{0x40, 0x3F},
            scan_code_usage{0x41, 0x40}, scan_code_usage{0x42, 0x41}, scan_code_usage{0x43, 0x42},
            scan_code_usage{0x44, 0x43}, // F1 .. F10
            scan_code_usage{0x47, 0x5F}, scan_code_usage{0x48, 0x60}, scan_code_usage{0x49, 0x61},
            scan_code_usage{0x4A, 0x56}, scan_code_usage{0x4B, 0x5C}, scan_code_usage{0x4C, 0x5D},
            scan_code_usage{0x4D, 0x5E}, scan_code_usage{0x4E, 0x57}, scan_code_usage{0x4F, 0x59},
            scan_code_usage{0x50, 0x5A}, scan_code_usage{0x51, 0x5B}, scan_code_usage{0x52, 0x62},
            scan_code_usage{0x53, 0x63}, // keypad
            scan_code_usage{0x56, 0x64}, // the key next to left Shift on ISO keyboards
            scan_code_usage{0x57, 0x44}, scan_code_usage{0x58, 0x45}, // F11 F12
        };

        // the same with the "extended key" flag (E0 prefix)
        constexpr std::array extended_keys{
            scan_code_usage{0x1C, 0x58}, // keypad Enter
            scan_code_usage{0x1D, 0xE4}, // right Ctrl
            scan_code_usage{0x35, 0x54}, // keypad /
            scan_code_usage{0x38, 0xE6}, // right Alt
            scan_code_usage{0x47, 0x4A}, scan_code_usage{0x48, 0x52}, scan_code_usage{0x49, 0x4B}, // Home Up PageUp
            scan_code_usage{0x4B, 0x50}, scan_code_usage{0x4D, 0x4F}, // Left Right
            scan_code_usage{0x4F, 0x4D}, scan_code_usage{0x50, 0x51}, scan_code_usage{0x51, 0x4E}, // End Down PageDown
            scan_code_usage{0x52, 0x49}, scan_code_usage{0x53, 0x4C}, // Insert Delete
        };

        // what the user did, waiting for the worker thread
        struct command
        {
            enum class kind
            {
                touch,
                scroll,
                swipe,
                keys,
                button,
                picture_changed,
                release_all // what waited was dropped: nothing may stay pressed
            };

            kind type = kind::touch;
            std::array<touch_point, user_fingers> points{}; // touch
            std::size_t point_count = 0;
            unsigned moved_fingers = 0; // touch: a bit per identity when the report only moves fingers
            std::uint16_t x = 0;        // scroll, swipe
            std::uint16_t y = 0;
            int delta_x = 0;
            int delta_y = 0;
            std::vector<std::uint8_t> usages; // keys
            phone_button button = phone_button::home;
        };

        // one finger on the phone's own panel
        struct contact
        {
            int identity = 0;
            std::uint16_t x = 0;
            std::uint16_t y = 0;
            bool touching = false;
        };

        // why the session with the phone ended or did not begin; `none`: it was asked to stop
        struct session_problem
        {
            remote_control_problem problem = remote_control_problem::internal;
            std::string detail;
        };

        using session_status = std::expected<void, session_problem>;

        [[nodiscard]] std::unexpected<session_problem> problem_of(remote_control_problem problem,
                                                                 std::string_view what, failure const &cause)
        {
            return std::unexpected(session_problem{problem, std::format("{}: {}", what, cause.describe())});
        }

        // the position (u, v) of the picture as the phone shows it, on the phone's own (upright) panel
        [[nodiscard]] std::pair<std::uint16_t, std::uint16_t> to_panel(int orientation, int u, int v)
        {
            u = std::clamp(u, 0, full_scale);
            v = std::clamp(v, 0, full_scale);
            int x = u;
            int y = v;
            switch (orientation)
            {
            case 2: // upside down
                x = full_scale - u;
                y = full_scale - v;
                break;
            case 3: // sideways, its bottom on the right
                x = full_scale - v;
                y = u;
                break;
            case 4: // sideways, its bottom on the left
                x = v;
                y = full_scale - u;
                break;
            default:
                break;
            }
            return {static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y)};
        }

        [[nodiscard]] std::string_view orientation_words(int orientation)
        {
            switch (orientation)
            {
            case 2:
                return "upside down";
            case 3:
                return "sideways (its bottom on the right)";
            case 4:
                return "sideways (its bottom on the left)";
            default:
                return "upright";
            }
        }

        // 48 bits of a steady clock in nanoseconds: the phone looks at the order and the distances
        void put_time(std::uint8_t *target)
        {
            auto const now = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            for (int index = 0; index < 6; ++index)
            {
                target[index] = static_cast<std::uint8_t>(now >> (8 * index));
            }
        }

        // The report of the touch screen: every finger that is down, and the
        // ones that lift with it (at most touch_contacts).
        [[nodiscard]] std::array<std::uint8_t, touch_report_bytes> touch_report(std::span<contact const> contacts)
        {
            std::array<std::uint8_t, touch_report_bytes> report{};
            report[0] = touch_report_identifier;
            report[1] = static_cast<std::uint8_t>(contacts.size());
            report[2] = static_cast<std::uint8_t>(touch_contacts);
            std::size_t offset = 3;
            for (contact const &one : contacts)
            {
                report[offset] = static_cast<std::uint8_t>((one.touching ? contact_touching : 0) | one.identity);
                report[offset + 1] = static_cast<std::uint8_t>(one.x);
                report[offset + 2] = static_cast<std::uint8_t>(one.x >> 8);
                report[offset + 3] = static_cast<std::uint8_t>(one.y);
                report[offset + 4] = static_cast<std::uint8_t>(one.y >> 8);
                offset += 5;
            }
            report[40] = 0x02;
            put_time(report.data() + 44);
            return report;
        }

        // the report of the keyboard: a bit per usage that is held down
        [[nodiscard]] std::array<std::uint8_t, keyboard_report_bytes> keyboard_report(
            std::span<std::uint8_t const> usages)
        {
            std::array<std::uint8_t, keyboard_report_bytes> report{};
            report[0] = keyboard_report_identifier;
            for (std::uint8_t const usage : usages)
            {
                if (usage < keyboard_usages)
                {
                    report[1 + usage / 8] = static_cast<std::uint8_t>(report[1 + usage / 8] | (1U << (usage % 8)));
                }
            }
            put_time(report.data() + 31);
            return report;
        }

        // for the log: the bytes of a report, two digits each
        [[nodiscard]] std::string hexadecimal(std::span<std::uint8_t const> bytes)
        {
            std::string text;
            text.reserve(bytes.size() * 2);
            for (std::uint8_t const byte : bytes)
            {
                std::format_to(std::back_inserter(text), "{:02x}", byte);
            }
            return text;
        }

        // one report to one input surface of the phone
        [[nodiscard]] status send_report(remote_xpc_connection &input, std::uint64_t surface,
                                         std::span<std::uint8_t const> report)
        {
            xpc_value pair = xpc_value::of_dictionary();
            pair.set("_0", xpc_value::of_data(report));
            pair.set("_1", xpc_value::of_unsigned(surface));
            xpc_value payload = xpc_value::of_dictionary();
            payload.set("send", std::move(pair));
            xpc_value message = xpc_value::of_dictionary();
            message.set("featureIdentifier", xpc_value::of_text(input_feature));
            message.set("messageType", xpc_value::of_text("Request"));
            message.set("payload", std::move(payload));
            return input.send(message);
        }

        // {"int": n} and the like: how the properties of a registered service are wrapped
        [[nodiscard]] xpc_value tagged(std::string_view tag, xpc_value value)
        {
            xpc_value wrapped = xpc_value::of_dictionary();
            wrapped.set(tag, std::move(value));
            return wrapped;
        }

        // The request that registers a keyboard with the phone (the way a Mac
        // lends its keyboard to an iPad).
        [[nodiscard]] xpc_value keyboard_registration()
        {
            constexpr std::string_view product = "MirrorDock keyboard";
            xpc_value usage_pair = xpc_value::of_dictionary();
            usage_pair.set("DeviceUsage", xpc_value::of_signed(6));
            usage_pair.set("DeviceUsagePage", xpc_value::of_signed(1));
            xpc_value stored_pair = xpc_value::of_dictionary();
            stored_pair.set("DeviceUsage", tagged("int", xpc_value::of_signed(6)));
            stored_pair.set("DeviceUsagePage", tagged("int", xpc_value::of_signed(1)));

            xpc_value storage = xpc_value::of_dictionary();
            storage.set("Manufacturer", tagged("string", xpc_value::of_text("MirrorDock")));
            storage.set("Product", tagged("string", xpc_value::of_text(product)));
            storage.set("ProductID", tagged("int", xpc_value::of_signed(0x0250)));
            storage.set("VendorID", tagged("int", xpc_value::of_signed(0x05AC)));
            storage.set("PrimaryUsage", tagged("int", xpc_value::of_signed(6)));
            storage.set("PrimaryUsagePage", tagged("int", xpc_value::of_signed(1)));
            storage.set("DeviceUsagePairs",
                        tagged("array", xpc_value::of_array({tagged("dictionary", std::move(stored_pair))})));
            storage.set("Transport", tagged("string", xpc_value::of_text("USB")));
            storage.set("ReportDescriptor", tagged("data", xpc_value::of_data(keyboard_descriptor)));
            storage.set("UniversalControlVirtualService", tagged("bool", xpc_value::of_flag(true)));
            storage.set("_ServiceID", tagged("uint", xpc_value::of_unsigned(keyboard_service)));

            xpc_value description = xpc_value::of_dictionary();
            description.set("DeviceUsagePairs", xpc_value::of_array({std::move(usage_pair)}));
            description.set("PrimaryUsage", xpc_value::of_unsigned(6));
            description.set("PrimaryUsagePage", xpc_value::of_unsigned(1));
            description.set("Product", xpc_value::of_text(product));
            description.set("ProductID", xpc_value::of_signed(0x0250));
            description.set("VendorID", xpc_value::of_signed(0x05AC));
            description.set("_CoreDevice_codablePropertyStorage", std::move(storage));
            description.set("_ServiceID", xpc_value::of_unsigned(keyboard_service));

            xpc_value create = xpc_value::of_dictionary();
            create.set("_0", std::move(description));
            xpc_value payload = xpc_value::of_dictionary();
            payload.set("createService", std::move(create));
            xpc_value message = xpc_value::of_dictionary();
            message.set("featureIdentifier", xpc_value::of_text(input_feature));
            message.set("messageType", xpc_value::of_text("Request"));
            message.set("payload", std::move(payload));
            return message;
        }

        [[nodiscard]] xpc_value button_event(button_code const &code, std::uint64_t button_state)
        {
            xpc_value payload = xpc_value::of_dictionary();
            payload.set("state", xpc_value::of_unsigned(button_state));
            payload.set("usagePage", xpc_value::of_unsigned(code.usage_page));
            payload.set("usageCode", xpc_value::of_unsigned(code.usage));
            xpc_value message = xpc_value::of_dictionary();
            message.set("messageType", xpc_value::of_text("IndigoButtonEvent"));
            message.set("payload", std::move(payload));
            message.set("featureIdentifier", xpc_value::of_text(button_feature));
            return message;
        }

        // a text the phone's lockdown knows about itself (general domain); empty when it does not say
        [[nodiscard]] std::string general_value(lockdown_session &session, std::string_view key)
        {
            outcome<property_list> const reply = session.value("", key);
            return reply ? reply->root().item("Value").text().value_or("") : std::string{};
        }

        // 27 of "27.2.1"; 0 when the text does not begin with a number
        [[nodiscard]] int leading_number(std::string_view text)
        {
            int number = 0;
            std::from_chars(text.data(), text.data() + text.size(), number);
            return number;
        }
    }

    std::string_view remote_control_problem_name(remote_control_problem problem)
    {
        for (problem_name const &entry : problem_names)
        {
            if (entry.problem == problem)
            {
                return entry.name;
            }
        }
        return "internal";
    }

    std::optional<std::uint8_t> keyboard_usage(unsigned scan_code, bool extended)
    {
        auto const find_in = [scan_code](auto const &table) -> std::optional<std::uint8_t>
        {
            for (scan_code_usage const &entry : table)
            {
                if (entry.scan_code == scan_code)
                {
                    return entry.usage;
                }
            }
            return std::nullopt;
        };
        return extended ? find_in(extended_keys) : find_in(plain_keys);
    }

    struct remote_control::state
    {
        status_callback on_status;
        std::filesystem::path image_cache;
        std::atomic<bool> silent{false}; // the owner is going away: nothing is reported to it any more
        std::thread worker;

        std::mutex mutex;
        std::condition_variable wake;
        // ---- guarded by mutex -----------------------------------------------------------------
        std::deque<command> queue;
        bool stopping = false;
        bool accepting = false;    // input reaches the phone: commands are taken
        unsigned fingers_down = 0; // of the touches taken so far: a bit per identity

        // ---- the session with the phone (worker thread only) ---------------------------------
        usbmux_device device;
        std::unique_ptr<lockdown_session> lockdown;
        std::unique_ptr<device_tunnel> tunnel;
        std::unique_ptr<service_directory> directory;
        std::unique_ptr<remote_xpc_connection> display;
        std::unique_ptr<remote_xpc_connection> input;
        std::unique_ptr<byte_stream> orientation_service; // opened with the first question
        std::string phone_words;                          // "iPhone17,2 with iOS 27.2.1"
        bool stream_running = false;
        bool keyboard_registered = false;
        bool keyboard_refused = false; // not asked for again in this session
        std::uint64_t keyboard = keyboard_service;
        int orientation = 1; // 1 upright, 2 upside down, 3 and 4 sideways
        bool orientation_unknown = false; // the last question got no answer (said once in the log)
        bool buttons_missing = false;     // said once in the log
        bool touch_report_logged = false; // the first report of each kind goes to the log (debug)
        bool key_report_logged = false;
        // the user's fingers that are down, where they are on the phone's panel
        std::array<std::optional<std::pair<std::uint16_t, std::uint16_t>>, touch_contacts> down{};

        void report(remote_control_state reported, remote_control_problem problem, std::string detail)
        {
            if (!silent.load() && on_status)
            {
                on_status(remote_control_status{reported, problem, std::move(detail)});
            }
        }

        // (with the mutex held)
        void push_locked(command next)
        {
            if (stopping || !accepting)
            {
                return; // what comes before the phone is ready is stale by the time it is
            }
            if (next.type == command::kind::touch && next.moved_fingers != 0 && !queue.empty() &&
                queue.back().type == command::kind::touch && queue.back().moved_fingers == next.moved_fingers)
            {
                queue.back() = std::move(next); // only the newest position of a move matters
                return;
            }
            if (queue.size() >= longest_queue)
            {
                // The phone has not taken input for long.  What waits is
                // stale: it is dropped, and nothing stays pressed.
                log::warning("remote control: the phone does not take input ({} commands waited); they are dropped",
                             queue.size());
                queue.clear();
                command release;
                release.type = command::kind::release_all;
                queue.push_back(std::move(release));
                fingers_down = 0;
                return;
            }
            queue.push_back(std::move(next));
        }

        void push(command next)
        {
            {
                std::scoped_lock lock{mutex};
                push_locked(std::move(next));
            }
            wake.notify_one();
        }

        [[nodiscard]] bool stop_requested()
        {
            std::scoped_lock lock{mutex};
            return stopping;
        }

        // false: the session was asked to stop while waiting
        [[nodiscard]] bool pause(std::chrono::milliseconds duration)
        {
            std::unique_lock lock{mutex};
            return !wake.wait_for(lock, duration, [this] { return stopping; });
        }

        [[nodiscard]] session_status connect();
        [[nodiscard]] session_status require_developer_mode();
        [[nodiscard]] bool offers_input_services() const;
        [[nodiscard]] session_status install_developer_image();
        [[nodiscard]] status open_orientation_service();
        [[nodiscard]] bool ask_orientation();
        [[nodiscard]] status send_touches(std::span<touch_point const> points);
        [[nodiscard]] status send_gesture(command const &gesture, std::chrono::milliseconds travel,
                                          std::chrono::milliseconds rest);
        [[nodiscard]] status send_keys(std::span<std::uint8_t const> usages);
        void press_button(phone_button button);
        void release_everything();
        [[nodiscard]] status handle(command const &next);
        [[nodiscard]] std::optional<session_problem> serve();
        void disconnect();
        void run();
    };

    // Developer Mode has to be on.  When it is off its switch is made to show
    // in the phone's settings (it is hidden on a phone that never met a
    // developer tool), so that the user finds it where MirrorDock says.
    session_status remote_control::state::require_developer_mode()
    {
        outcome<property_list> const mode = lockdown->value(developer_mode_domain, "DeveloperModeStatus");
        if (!mode)
        {
            return problem_of(remote_control_problem::internal, "the state of Developer Mode", mode.error());
        }
        if (mode->root().item("Value").flag().value_or(false))
        {
            return {};
        }
        std::string shown = "its switch is shown in Settings > Privacy & Security";
        outcome<std::unique_ptr<byte_stream>> const service = lockdown->start_service(developer_mode_service_name);
        if (service)
        {
            property_list reveal;
            reveal.set_number("action", 0); // "show the switch"
            status const sent = send_property_list(**service, reveal);
            outcome<property_list> const answer =
                sent ? receive_property_list(**service) : outcome<property_list>{std::unexpected(sent.error())};
            if (!answer)
            {
                shown = std::format("its switch could not be shown ({})", answer.error().describe());
            }
            else if (!answer->root().item("success").present())
            {
                shown = "the phone did not confirm that its switch is shown";
            }
        }
        else
        {
            shown = std::format("its switch could not be shown ({})", service.error().describe());
        }
        return std::unexpected(session_problem{remote_control_problem::developer_mode_off,
                                               std::format("Developer Mode is switched off on the iPhone; {}", shown)});
    }

    bool remote_control::state::offers_input_services() const
    {
        return directory->offers(display_service_name) && directory->offers(input_service_name);
    }

    // The services that carry the screen stream and the input come with the
    // Developer Disk Image.  A phone gets it once per version of iOS; this
    // installs it and reads the phone's directory again.
    session_status remote_control::state::install_developer_image()
    {
        if (!offers_cryptexes(*directory))
        {
            return std::unexpected(session_problem{
                remote_control_problem::image_mount_failed,
                std::format("the phone lists {} developer service(s): neither the ones for the screen and for input "
                            "nor the one that installs the Developer Disk Image they come with",
                            directory->size())});
        }
        report(remote_control_state::preparing, remote_control_problem::none,
               "the iPhone lacks the Developer Disk Image: installing it");
        outcome<bool> const installed = ensure_developer_image(
            *directory, image_cache, [](std::string_view words) { log::info("remote control: {}", words); },
            [this] { return stop_requested(); });
        if (stop_requested())
        {
            return std::unexpected(session_problem{remote_control_problem::none, {}});
        }
        if (!installed)
        {
            return problem_of(remote_control_problem::image_mount_failed, "the Developer Disk Image",
                              installed.error());
        }
        log::info("remote control: the Developer Disk Image {}",
                  *installed ? "was installed" : "is installed already; waiting for its services");
        for (int attempt = 0; attempt < image_services_attempts; ++attempt)
        {
            outcome<std::unique_ptr<service_directory>> again =
                service_directory::open(*tunnel, lockdown->host_identity());
            if (!again)
            {
                return problem_of(remote_control_problem::tunnel_failed, "the directory of the developer services",
                                  again.error());
            }
            directory = std::move(*again);
            if (offers_input_services())
            {
                return {};
            }
            if (!pause(image_services_step))
            {
                return std::unexpected(session_problem{remote_control_problem::none, {}});
            }
        }
        return std::unexpected(session_problem{
            remote_control_problem::image_mount_failed,
            std::format("the Developer Disk Image is installed, but the phone does not list its services for the "
                        "screen and for input ({} service(s) listed); restarting the iPhone may help",
                        directory->size())});
    }

    // Everything from the cable to "input reaches the phone".
    session_status remote_control::state::connect()
    {
        auto const asked_to_stop = [this]() -> std::optional<session_problem>
        {
            return stop_requested() ? std::optional{session_problem{remote_control_problem::none, {}}} : std::nullopt;
        };

        outcome<std::vector<usbmux_device>> const devices = usbmux_devices();
        if (!devices)
        {
            return problem_of(remote_control_problem::no_device, "the list of attached iPhones", devices.error());
        }
        auto const on_cable = std::ranges::find_if(*devices, &usbmux_device::over_cable);
        if (on_cable == devices->end())
        {
            return std::unexpected(
                session_problem{remote_control_problem::no_device,
                                devices->empty() ? "no iPhone is attached to this PC"
                                                 : "the iPhone is attached over the network only, not on a cable"});
        }
        device = *on_cable;

        outcome<std::unique_ptr<lockdown_session>> session = lockdown_session::open(device);
        if (!session)
        {
            return problem_of(pairing_refused(session.error()) ? remote_control_problem::not_trusted
                                                               : remote_control_problem::no_device,
                              "the session with the iPhone", session.error());
        }
        lockdown = std::move(*session);
        std::string const product = general_value(*lockdown, "ProductType");
        std::string const version = general_value(*lockdown, "ProductVersion");
        phone_words = std::format("{} with iOS {}", product.empty() ? "an iPhone" : product,
                                  version.empty() ? "of an unknown version" : version);
        report(remote_control_state::starting, remote_control_problem::none,
               std::format("{} is on the cable", phone_words));
        if (int const major = leading_number(version); major != 0 && major < first_ios_with_input)
        {
            return std::unexpected(session_problem{
                remote_control_problem::unsupported_ios,
                std::format("{}: input from a PC goes through services iOS has from version {} on", phone_words,
                            first_ios_with_input)});
        }
        if (session_status const allowed = require_developer_mode(); !allowed)
        {
            return allowed;
        }
        if (auto stop = asked_to_stop(); stop)
        {
            return std::unexpected(std::move(*stop));
        }

        outcome<std::unique_ptr<device_tunnel>> opened_tunnel = device_tunnel::open(*lockdown);
        if (!opened_tunnel)
        {
            return problem_of(remote_control_problem::tunnel_failed, "the developer tunnel", opened_tunnel.error());
        }
        tunnel = std::move(*opened_tunnel);
        outcome<std::unique_ptr<service_directory>> opened_directory =
            service_directory::open(*tunnel, lockdown->host_identity());
        if (!opened_directory)
        {
            return problem_of(remote_control_problem::tunnel_failed, "the directory of the developer services",
                              opened_directory.error());
        }
        directory = std::move(*opened_directory);
        if (!offers_input_services())
        {
            if (session_status const installed = install_developer_image(); !installed)
            {
                return installed;
            }
        }
        if (auto stop = asked_to_stop(); stop)
        {
            return std::unexpected(std::move(*stop));
        }

        // the stream of the screen that lets input through; its packets are dropped in the tunnel
        outcome<std::unique_ptr<remote_xpc_connection>> opened_display = directory->connect(display_service_name);
        if (!opened_display)
        {
            return problem_of(remote_control_problem::stream_failed, "the display service", opened_display.error());
        }
        display = std::move(*opened_display);
        outcome<xpc_value> offer =
            video_stream_request(tunnel->local_address(), tunnel->discard_port(), tunnel->phone_address());
        if (!offer)
        {
            return problem_of(remote_control_problem::internal, "the offer for the screen stream", offer.error());
        }
        outcome<xpc_value> const started =
            core_device_invoke(*display, start_stream_feature, start_stream_action, std::move(*offer), call_patience);
        if (!started)
        {
            return problem_of(started.error().code == media_in_use_code ? remote_control_problem::media_in_use
                                                                         : remote_control_problem::stream_failed,
                              "the start of the screen stream", started.error());
        }
        stream_running = true;
        if (!pause(input_settle))
        {
            return std::unexpected(session_problem{remote_control_problem::none, {}});
        }
        outcome<std::unique_ptr<remote_xpc_connection>> opened_input = directory->connect(input_service_name);
        if (!opened_input)
        {
            return problem_of(remote_control_problem::stream_failed, "the input service", opened_input.error());
        }
        input = std::move(*opened_input);
        // (touches are right from the first one on when the phone says now how it is held)
        static_cast<void>(ask_orientation());
        return {};
    }

    status remote_control::state::open_orientation_service()
    {
        if (lockdown)
        {
            if (outcome<std::unique_ptr<byte_stream>> opened = lockdown->start_service(orientation_service_name);
                opened)
            {
                orientation_service = std::move(*opened);
                return {};
            }
        }
        // the session with lockdown has been idle for long and may have ended: a new one
        outcome<std::unique_ptr<lockdown_session>> session = lockdown_session::open(device);
        if (!session)
        {
            lockdown.reset();
            return std::unexpected(session.error());
        }
        lockdown = std::move(*session);
        outcome<std::unique_ptr<byte_stream>> opened = lockdown->start_service(orientation_service_name);
        if (!opened)
        {
            return std::unexpected(opened.error());
        }
        orientation_service = std::move(*opened);
        return {};
    }

    // Asks the phone which way it is held.  Without an answer the last one
    // stays in use; false then.
    bool remote_control::state::ask_orientation()
    {
        auto const unanswered = [this](failure const &cause)
        {
            orientation_service.reset(); // the next question connects again
            if (!std::exchange(orientation_unknown, true))
            {
                log::warning("remote control: the phone does not say which way it is held ({}); touches go on as "
                             "for a phone held {}",
                             cause.describe(), orientation_words(orientation));
            }
            return false;
        };
        if (!orientation_service)
        {
            if (status const opened = open_orientation_service(); !opened)
            {
                return unanswered(opened.error());
            }
        }
        property_list question;
        question.set_text("command", "getInterfaceOrientation");
        if (status const sent = send_property_list(*orientation_service, question); !sent)
        {
            return unanswered(sent.error());
        }
        outcome<property_list> const answer = receive_property_list(*orientation_service);
        if (!answer)
        {
            return unanswered(answer.error());
        }
        std::uint64_t const held = answer->root().item("interfaceOrientation").number().value_or(0);
        if (held < 1 || held > 4)
        {
            return unanswered(make_failure("interface orientation", static_cast<std::int64_t>(held),
                                           "the phone's answer names none of the four"));
        }
        orientation_unknown = false;
        if (std::exchange(orientation, static_cast<int>(held)) != static_cast<int>(held))
        {
            log::info("remote control: the phone is held {}", orientation_words(orientation));
        }
        return true;
    }

    status remote_control::state::send_touches(std::span<touch_point const> points)
    {
        std::array<std::optional<contact>, touch_contacts> listed{};
        for (touch_point const &point : points)
        {
            auto const [x, y] = to_panel(orientation, point.x, point.y);
            auto const slot = static_cast<std::size_t>(point.identity);
            listed[slot] = contact{point.identity, x, y, point.touching};
            down[slot] = point.touching ? std::optional{std::pair{x, y}} : std::nullopt;
        }
        // every finger that is still down is in every report
        std::array<contact, touch_contacts> contacts{};
        std::size_t count = 0;
        for (std::size_t slot = 0; slot < touch_contacts; ++slot)
        {
            if (listed[slot])
            {
                contacts[count++] = *listed[slot];
            }
            else if (down[slot])
            {
                contacts[count++] = contact{static_cast<int>(slot), down[slot]->first, down[slot]->second, true};
            }
        }
        if (count == 0)
        {
            return {};
        }
        std::array<std::uint8_t, touch_report_bytes> const report = touch_report(std::span{contacts}.first(count));
        if (!std::exchange(touch_report_logged, true))
        {
            // (positions on the panel and a timestamp: nothing that names the phone)
            log::debug("remote control: the first touch report: {}", hexadecimal(report));
        }
        return send_report(*input, touch_screen_service, report);
    }

    // A finger of this end's own goes down at (x, y) of the picture, travels
    // by the gesture's deltas in `travel` and lifts after `rest`.  Where it is
    // depends on the time that has passed, not on how many reports the system's
    // timer let through: the phone reads the speed from positions and times.
    status remote_control::state::send_gesture(command const &gesture, std::chrono::milliseconds travel,
                                               std::chrono::milliseconds rest)
    {
        if (std::ranges::any_of(down, [](auto const &finger) { return finger.has_value(); }))
        {
            return {}; // not while the user holds a finger down
        }
        int const start_u = gesture.x;
        int const start_v = gesture.y;
        int const end_u = std::clamp(start_u + gesture.delta_x, 0, full_scale);
        int const end_v = std::clamp(start_v + gesture.delta_y, 0, full_scale);
        if (std::abs(end_u - start_u) < shortest_gesture && std::abs(end_v - start_v) < shortest_gesture)
        {
            return {};
        }
        int const held = orientation; // one answer for the whole gesture
        auto const finger_at = [&](double share, bool touching) -> status
        {
            auto const [x, y] = to_panel(held, start_u + static_cast<int>(std::lround((end_u - start_u) * share)),
                                         start_v + static_cast<int>(std::lround((end_v - start_v) * share)));
            contact const finger{gesture_finger, x, y, touching};
            return send_report(*input, touch_screen_service, touch_report(std::span{&finger, 1}));
        };
        auto const began = std::chrono::steady_clock::now();
        for (;;)
        {
            double const share =
                std::min(1.0, std::chrono::duration<double>(std::chrono::steady_clock::now() - began) / travel);
            if (status const sent = finger_at(share, true); !sent)
            {
                return sent;
            }
            if (share >= 1.0)
            {
                break;
            }
            if (!pause(gesture_step))
            {
                return finger_at(share, false); // asked to stop: the finger lifts where it is
            }
        }
        if (rest > std::chrono::milliseconds::zero())
        {
            static_cast<void>(pause(rest));
        }
        return finger_at(1.0, false);
    }

    status remote_control::state::send_keys(std::span<std::uint8_t const> usages)
    {
        if (keyboard_refused)
        {
            return {};
        }
        if (!keyboard_registered)
        {
            outcome<xpc_value> const answer = input->request(keyboard_registration(), keyboard_patience);
            if (!answer)
            {
                // (when the connection is gone, the next report to the phone says so)
                keyboard_refused = true;
                log::warning("remote control: the phone did not register a keyboard ({}); keys do not reach it in "
                             "this session",
                             answer.error().describe());
                return {};
            }
            if (xpc_value const *const named = answer->find("serviceID"); named != nullptr)
            {
                keyboard = named->as_unsigned().value_or(keyboard);
            }
            keyboard_registered = true;
            log::info("remote control: a keyboard is registered with the phone");
            log::debug("remote control: the phone's answer to the registration: {}", answer->describe());
            static_cast<void>(pause(keyboard_settle));
        }
        std::array<std::uint8_t, keyboard_report_bytes> const report = keyboard_report(usages);
        if (!std::exchange(key_report_logged, true))
        {
            log::debug("remote control: the first keyboard report ({} key(s) down, {} bytes)", usages.size(),
                       report.size());
        }
        return send_report(*input, keyboard, report);
    }

    // One press of a hardware button.  Its service is another one than the
    // touch screen's; what goes wrong with it does not end the session.
    void remote_control::state::press_button(phone_button button)
    {
        auto const code = std::ranges::find(button_codes, button, &button_code::button);
        if (code == button_codes.end())
        {
            return;
        }
        if (!directory->offers(button_service_name))
        {
            if (!std::exchange(buttons_missing, true))
            {
                log::warning("remote control: the phone offers no service for its buttons ({})", button_service_name);
            }
            return;
        }
        outcome<std::unique_ptr<remote_xpc_connection>> const buttons = directory->connect(button_service_name);
        if (!buttons)
        {
            log::warning("remote control: the button was not pressed ({})", buttons.error().describe());
            return;
        }
        status sent = (*buttons)->send(button_event(*code, button_down));
        if (sent)
        {
            static_cast<void>(pause(code->held));
            sent = (*buttons)->send(button_event(*code, button_up));
        }
        if (!sent)
        {
            log::warning("remote control: the button was not pressed ({})", sent.error().describe());
        }
    }

    // Nothing stays pressed on the phone.  (As far as the phone can be told:
    // this is also what a session does last.)
    void remote_control::state::release_everything()
    {
        if (!input)
        {
            return;
        }
        std::array<contact, touch_contacts> lifting{};
        std::size_t count = 0;
        for (std::size_t slot = 0; slot < touch_contacts; ++slot)
        {
            if (down[slot])
            {
                lifting[count++] = contact{static_cast<int>(slot), down[slot]->first, down[slot]->second, false};
                down[slot].reset();
            }
        }
        if (count != 0)
        {
            static_cast<void>(
                send_report(*input, touch_screen_service, touch_report(std::span{lifting}.first(count))));
        }
        if (keyboard_registered)
        {
            static_cast<void>(send_report(*input, keyboard, keyboard_report({})));
        }
    }

    // A failure: the phone no longer takes input (the session is over).
    status remote_control::state::handle(command const &next)
    {
        switch (next.type)
        {
        case command::kind::touch:
            return send_touches(std::span{next.points}.first(next.point_count));
        case command::kind::scroll:
            return send_gesture(next, scroll_travel, scroll_rest);
        case command::kind::swipe:
            return send_gesture(next, swipe_travel, std::chrono::milliseconds::zero());
        case command::kind::keys:
            return send_keys(next.usages);
        case command::kind::button:
            press_button(next.button);
            return {};
        case command::kind::picture_changed:
            static_cast<void>(ask_orientation());
            return {};
        case command::kind::release_all:
            release_everything();
            return {};
        }
        return {};
    }

    // Commands to reports until the session is asked to stop (nothing then),
    // or until the phone is gone.
    std::optional<session_problem> remote_control::state::serve()
    {
        auto next_question = std::chrono::steady_clock::now() + orientation_interval;
        auto next_stream_look = std::chrono::steady_clock::now() + stream_watch_interval;
        std::uint64_t stream_datagrams = tunnel->discarded_datagrams();
        for (;;)
        {
            std::optional<command> next;
            {
                std::unique_lock lock{mutex};
                wake.wait_until(lock, next_question, [this] { return stopping || !queue.empty(); });
                if (stopping)
                {
                    return std::nullopt;
                }
                if (!queue.empty())
                {
                    next = std::move(queue.front());
                    queue.pop_front();
                    // the turns of the wheel that wait already are one gesture
                    while (next->type == command::kind::scroll && !queue.empty() &&
                           queue.front().type == command::kind::scroll)
                    {
                        next->delta_x += queue.front().delta_x;
                        next->delta_y += queue.front().delta_y;
                        queue.pop_front();
                    }
                }
            }
            if (!tunnel->alive())
            {
                return session_problem{remote_control_problem::connection_lost,
                                       "the connection to the iPhone ended (cable pulled?)"};
            }
            if (next)
            {
                if (status const done = handle(*next); !done)
                {
                    return session_problem{remote_control_problem::connection_lost, done.error().describe()};
                }
                if (next->type == command::kind::picture_changed)
                {
                    next_question = std::chrono::steady_clock::now() + orientation_interval;
                }
            }
            if (std::chrono::steady_clock::now() >= next_question)
            {
                bool const answered = ask_orientation();
                next_question = std::chrono::steady_clock::now() +
                                (answered ? std::chrono::milliseconds{orientation_interval}
                                          : std::chrono::milliseconds{orientation_retry});
            }
            if (std::chrono::steady_clock::now() >= next_stream_look)
            {
                std::uint64_t const arrived = tunnel->discarded_datagrams();
                log::debug("remote control: {} datagram(s) of the screen stream in the last {} s",
                           arrived - stream_datagrams, stream_watch_interval.count());
                stream_datagrams = arrived;
                next_stream_look = std::chrono::steady_clock::now() + stream_watch_interval;
            }
        }
    }

    void remote_control::state::disconnect()
    {
        if (stream_running && display && tunnel && tunnel->alive())
        {
            // (a stream that goes on keeps the phone's camera and microphone busy)
            xpc_value everything = xpc_value::of_dictionary();
            everything.set("stopAll", xpc_value::of_flag(true));
            outcome<xpc_value> const stopped = core_device_invoke(*display, stop_stream_feature, stop_stream_action,
                                                                  std::move(everything), stop_patience);
            if (!stopped)
            {
                log::warning("remote control: the phone's screen stream could not be stopped ({})",
                             stopped.error().describe());
            }
        }
        stream_running = false;
        input.reset();
        display.reset();
        directory.reset();
        orientation_service.reset();
        tunnel.reset();
        lockdown.reset();
    }

    void remote_control::state::run()
    {
        auto const began = std::chrono::steady_clock::now();
        report(remote_control_state::starting, remote_control_problem::none, "looking for the iPhone on the cable");
        std::optional<session_problem> ended;
        if (session_status connected = connect(); connected)
        {
            {
                std::scoped_lock lock{mutex};
                accepting = true;
                fingers_down = 0;
            }
            report(remote_control_state::ready, remote_control_problem::none,
                   std::format("{} on the cable, held {}; ready after {} ms", phone_words,
                               orientation_words(orientation),
                               std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - began)
                                   .count()));
            ended = serve();
            {
                std::scoped_lock lock{mutex};
                accepting = false;
                queue.clear();
            }
            if (!ended)
            {
                release_everything(); // asked to stop: the phone is still there
            }
        }
        else if (connected.error().problem != remote_control_problem::none)
        {
            ended = std::move(connected.error());
        }
        disconnect();
        if (ended)
        {
            report(remote_control_state::failed, ended->problem, std::move(ended->detail));
        }
    }

    remote_control::remote_control(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    outcome<std::unique_ptr<remote_control>> remote_control::start(status_callback on_status,
                                                                   std::filesystem::path image_cache)
    {
        auto created = std::make_unique<state>();
        created->on_status = std::move(on_status);
        created->image_cache = std::move(image_cache);
        state *const running = created.get();
        try
        {
            created->worker = std::thread{[running] { running->run(); }};
        }
        catch (std::system_error const &error)
        {
            return std::unexpected(make_failure("remote control thread", error.code().value(), error.what()));
        }
        return std::unique_ptr<remote_control>{new remote_control{std::move(created)}};
    }

    remote_control::~remote_control()
    {
        // nothing is reported from here on: the owner is going away (a
        // report that is under way is waited for below, with the worker)
        impl->silent.store(true);
        {
            std::scoped_lock lock{impl->mutex};
            impl->stopping = true;
        }
        impl->wake.notify_all();
        if (impl->worker.joinable())
        {
            impl->worker.join();
        }
    }

    void remote_control::touch(std::span<touch_point const> points)
    {
        command next;
        next.type = command::kind::touch;
        unsigned touching = 0;
        unsigned lifting = 0;
        for (touch_point const &point : points)
        {
            if (point.identity < 0 || point.identity >= user_fingers)
            {
                continue;
            }
            unsigned const finger = 1U << point.identity;
            if (((touching | lifting) & finger) != 0)
            {
                continue; // one entry per finger
            }
            next.points[next.point_count++] = point;
            (point.touching ? touching : lifting) |= finger;
        }
        if (next.point_count == 0)
        {
            return;
        }
        {
            std::scoped_lock lock{impl->mutex};
            // a move: every finger of the report was down before, and none lifts
            if (lifting == 0 && touching == impl->fingers_down)
            {
                next.moved_fingers = touching;
            }
            if (impl->accepting)
            {
                impl->fingers_down = (impl->fingers_down | touching) & ~lifting;
            }
            impl->push_locked(std::move(next));
        }
        impl->wake.notify_one();
    }

    void remote_control::scroll(std::uint16_t x, std::uint16_t y, int delta_x, int delta_y)
    {
        if (delta_x == 0 && delta_y == 0)
        {
            return;
        }
        command next;
        next.type = command::kind::scroll;
        next.x = x;
        next.y = y;
        next.delta_x = delta_x;
        next.delta_y = delta_y;
        impl->push(std::move(next));
    }

    void remote_control::swipe(std::uint16_t x, std::uint16_t y, int delta_x, int delta_y)
    {
        if (delta_x == 0 && delta_y == 0)
        {
            return;
        }
        command next;
        next.type = command::kind::swipe;
        next.x = x;
        next.y = y;
        next.delta_x = delta_x;
        next.delta_y = delta_y;
        impl->push(std::move(next));
    }

    void remote_control::keys(std::span<std::uint8_t const> usages)
    {
        command next;
        next.type = command::kind::keys;
        next.usages.assign(usages.begin(), usages.end());
        impl->push(std::move(next));
    }

    void remote_control::press(phone_button button)
    {
        command next;
        next.type = command::kind::button;
        next.button = button;
        impl->push(std::move(next));
    }

    void remote_control::picture_changed()
    {
        command next;
        next.type = command::kind::picture_changed;
        impl->push(std::move(next));
    }
}
