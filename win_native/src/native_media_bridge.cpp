// SPDX-License-Identifier: MIT
// C entry points (native_media.h) over the C++ media engine.
//
// The C interface replaces UxPlay's process-wide GStreamer renderer
// functions, so its state is one process-wide instance by design; it is
// created by native_media_start() and destroyed by native_media_stop().
//
// Engine creation (GPU device, window, presenter, WASAPI, USB) runs on a
// background thread so the AirPlay server and its mDNS advertisement go live
// immediately; media calls wait (bounded) for the engine the first time.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include "native_media.h"
#include "uxplay_version.h"

import std;
import uxplay_status;
import uxplay_log;
import uxplay_clock;
import uxplay_media_types;
import uxplay_media_engine;
import uxplay_usb_link;
import uxplay_video_decoder;
import uxplay_audio_decoder;
import uxplay_system_tuning;
import uxplay_gpu_device;
import uxplay_video_window;
import uxplay_ui_text;
import uxplay_settings;
import uxplay_crash_report;

namespace
{
    using uxplay::media_source;

    constexpr auto engine_start_wait = std::chrono::seconds{3};
    // The display query at start decides the codec offer and the stream size
    // for the whole run from what the GPU can decode: it waits for the engine
    // much longer than a media call does (a first start can be slow while the
    // program is scanned or a GPU wakes up).
    constexpr auto display_query_wait = std::chrono::seconds{20};

    // per-user settings (uxplay_settings)
    constexpr std::wstring_view setting_prefer_usb_link = L"prefer_usb_link";
    constexpr std::wstring_view setting_performance_mode = L"performance_mode";
    constexpr std::wstring_view setting_crop_mode = L"crop_mode"; // 0 off, 1 automatic
    constexpr std::wstring_view setting_ready_hint_shown = L"ready_hint_shown";
    constexpr std::wstring_view setting_close_hint_shown = L"close_hint_shown";
    // how long Windows' shutdown waits for the network goodbye
    constexpr DWORD session_end_wait_ms = 2000;

    // interface text in the language chosen at native_media_start
    std::wstring ui_text_string(uxplay::ui_text id)
    {
        return std::wstring{uxplay::ui_string(id)};
    }

    std::wstring ui_text_with(uxplay::ui_text id, std::initializer_list<std::wstring_view> arguments)
    {
        return uxplay::ui_format(id, std::span<std::wstring_view const>{arguments.begin(), arguments.size()});
    }

    std::atomic<bool> ui_language_applied{false};

    // native_media_settings::ui_language: 0 follows the Windows display
    // language; 1..10 force one (the order of uxplay::ui_language, plus one)
    void apply_ui_language(int requested)
    {
        ui_language_applied.store(true);
        if (requested >= 1 && requested <= static_cast<int>(uxplay::ui_language::count))
        {
            auto const forced = static_cast<uxplay::ui_language>(requested - 1);
            uxplay::set_ui_language(forced);
            uxplay::log::info("interface language: {} (set with -ui)", uxplay::ui_language_name(forced));
            return;
        }
        auto detected = uxplay::detect_ui_language();
        if (!detected)
        {
            uxplay::set_ui_language(uxplay::ui_language::english);
            uxplay::log::warning("interface language: the Windows display language is unavailable ({}); using English",
                                 detected.error().describe());
            return;
        }
        uxplay::set_ui_language(detected->language);
        uxplay::log::info("interface language: {} (Windows display language {})",
                          uxplay::ui_language_name(detected->language), uxplay::utf16_to_utf8(detected->windows_language));
    }

    struct bridge_state
    {
        std::mutex lifecycle_mutex;
        std::thread start_thread;
        std::mutex ready_mutex;
        std::condition_variable ready_wake;
        std::atomic<bool> start_requested{false};
        std::atomic<bool> start_finished{false};
        std::unique_ptr<uxplay::media_engine> engine_owner;
        std::unique_ptr<uxplay::usb_link_monitor> usb_link_owner;
        std::atomic<uxplay::media_engine *> engine{nullptr};
        std::atomic<uxplay::usb_link_monitor *> usb_link{nullptr};
        // USB link monitor thread only
        std::optional<uxplay::usb_link_state> last_usb_link;
        bool hotspot_hint_shown = false;
        // AirPlay link preference (persisted in HKCU)
        std::atomic<bool> prefer_usb_link{true};
        // uxplay::performance_mode in force
        std::atomic<int> mode{static_cast<int>(uxplay::performance_mode::balanced)};
        std::mutex display_mutex;
        std::optional<uxplay::display_mode_info> display; // guarded by display_mutex
        native_media_event_callback event_callback = nullptr;
        void *event_user_data = nullptr;
        std::wstring log_path;
        // set once in native_media_start, read-only afterwards
        std::wstring receiver_name;
        std::wstring product_version;
        std::wstring core_version;
        std::wstring project_url;
        HANDLE network_stopped_event = nullptr;
        // why the engine did not start (written before start_finished)
        std::string failure_text;
        // warnings raised before native_media_start installed the log sink
        // (host main thread only); logged by native_media_start
        std::vector<std::string> early_warnings;

        // AirPlay: remote -> local mapping used until NTP sync is available
        std::mutex mapping_mutex;
        std::optional<std::int64_t> remote_to_local_offset;
        std::atomic<std::uint64_t> invalid_video_units{0};
    };

    bridge_state &bridge()
    {
        static bridge_state instance;
        return instance;
    }

    uxplay::media_engine *engine(std::chrono::seconds longest_wait = engine_start_wait)
    {
        bridge_state &state = bridge();
        if (uxplay::media_engine *const ready = state.engine.load(std::memory_order_acquire); ready != nullptr)
        {
            return ready;
        }
        if (state.start_finished.load(std::memory_order_acquire) || !state.start_requested.load(std::memory_order_acquire))
        {
            return nullptr;
        }
        std::unique_lock lock{state.ready_mutex};
        state.ready_wake.wait_for(lock, longest_wait, [&state]
        {
            return state.start_finished.load(std::memory_order_acquire);
        });
        return state.engine.load(std::memory_order_acquire);
    }

    void emit(native_media_event event)
    {
        bridge_state &state = bridge();
        if (state.event_callback != nullptr)
        {
            state.event_callback(event, state.event_user_data);
        }
    }

    std::int64_t airplay_media_time(std::uint64_t local_time_ns, std::uint64_t remote_time_ns)
    {
        if (local_time_ns != 0)
        {
            return static_cast<std::int64_t>(local_time_ns);
        }
        bridge_state &state = bridge();
        std::scoped_lock lock{state.mapping_mutex};
        if (!state.remote_to_local_offset)
        {
            state.remote_to_local_offset = uxplay::clock::now_ns() - static_cast<std::int64_t>(remote_time_ns);
        }
        return static_cast<std::int64_t>(remote_time_ns) + *state.remote_to_local_offset;
    }

    uxplay::performance_mode mode_from_number(int value)
    {
        switch (value)
        {
        case NATIVE_MEDIA_MODE_QUALITY:
            return uxplay::performance_mode::quality;
        case NATIVE_MEDIA_MODE_PERFORMANCE:
            return uxplay::performance_mode::performance;
        default:
            return uxplay::performance_mode::balanced;
        }
    }

    uxplay::ui_text mode_label(uxplay::performance_mode mode)
    {
        switch (mode)
        {
        case uxplay::performance_mode::quality:
            return uxplay::ui_text::mode_quality;
        case uxplay::performance_mode::performance:
            return uxplay::ui_text::mode_performance;
        case uxplay::performance_mode::balanced:
            break;
        }
        return uxplay::ui_text::mode_balanced;
    }

    uxplay::ui_text mode_description(uxplay::performance_mode mode)
    {
        switch (mode)
        {
        case uxplay::performance_mode::quality:
            return uxplay::ui_text::mode_quality_text;
        case uxplay::performance_mode::performance:
            return uxplay::ui_text::mode_performance_text;
        case uxplay::performance_mode::balanced:
            break;
        }
        return uxplay::ui_text::mode_balanced_text;
    }

    // ---- AirPlay link preference (USB first / Wi-Fi only) --------------------

    bool load_prefer_usb_link()
    {
        // default: USB first
        return uxplay::settings::stored_number(setting_prefer_usb_link).value_or(1) != 0;
    }

    void save_prefer_usb_link(bool prefer_usb_link)
    {
        uxplay::settings::store_number(setting_prefer_usb_link, prefer_usb_link ? 1U : 0U);
    }

    // ---- iPhone USB link (AirPlay over the Personal Hotspot cable) ----------

    bool offer_data_guard(uxplay::usb_link_state const &link)
    {
        // never cut a deliberate tether: only when another network has internet
        return link.link_up && link.other_internet && (!link.data_guard_applied || link.data_leak());
    }

    bool offer_enable_adapter(uxplay::usb_link_state const &link, bool prefer_usb_link)
    {
        return prefer_usb_link && link.link_adapter_disabled;
    }

    std::wstring usb_link_line(uxplay::usb_link_state const &link, bool prefer_usb_link)
    {
        if (!prefer_usb_link)
        {
            // "Wi-Fi only": whatever is plugged in, the cable only charges
            return ui_text_string(link.iphone_attached || link.link_up ? uxplay::ui_text::usb_charging_only_status
                                                                       : uxplay::ui_text::usb_not_connected);
        }
        if (link.link_adapter_disabled)
        {
            return ui_text_string(uxplay::ui_text::usb_adapter_disabled);
        }
        if (link.link_up)
        {
            return std::format(L"{} {}", uxplay::ui_string(uxplay::ui_text::usb_ready_at),
                               uxplay::utf8_to_utf16(link.local_address));
        }
        if (link.iphone_attached)
        {
            return ui_text_string(uxplay::ui_text::usb_turn_on_hotspot);
        }
        return ui_text_string(uxplay::ui_text::usb_not_connected);
    }

    void refresh_usb_link_line()
    {
        bridge_state &state = bridge();
        uxplay::media_engine *const ready = state.engine.load(std::memory_order_acquire);
        uxplay::usb_link_monitor *const link = state.usb_link.load(std::memory_order_acquire);
        if (ready == nullptr || link == nullptr)
        {
            return;
        }
        uxplay::usb_link_state const current = link->current();
        bool const prefer = state.prefer_usb_link.load();
        ready->set_usb_link(usb_link_line(current, prefer), offer_data_guard(current),
                            offer_enable_adapter(current, prefer));
    }

    std::string describe_usb_link(uxplay::usb_link_state const &link)
    {
        if (link.link_up)
        {
            return std::format("up, PC address {}; PC internet through the iPhone (cellular data): IPv4 {}, IPv6 {}; "
                               "other internet {}; data guard {}",
                               link.local_address, link.ipv4_internet_via_link ? "YES" : "no",
                               link.ipv6_internet_via_link ? "YES" : "no", link.other_internet ? "yes" : "no",
                               link.data_guard_applied ? "applied" : "not applied");
        }
        if (link.link_adapter_disabled)
        {
            return "iPhone attached but its network adapter (Apple Mobile Device Ethernet) is disabled in Windows "
                   "(AirPlay uses Wi-Fi)";
        }
        if (link.link_missing)
        {
            return "iPhone attached but no network link: Personal Hotspot is off (AirPlay uses Wi-Fi)";
        }
        if (link.iphone_attached)
        {
            return "iPhone attached, waiting for its network link";
        }
        return "no iPhone attached (AirPlay uses Wi-Fi)";
    }

    void on_usb_link_change(uxplay::usb_link_state const &next)
    {
        bridge_state &state = bridge();
        uxplay::usb_link_state const previous = state.last_usb_link.value_or(uxplay::usb_link_state{});
        state.last_usb_link = next;
        bool const offer = offer_data_guard(next);
        uxplay::log::info("USB link: {}", describe_usb_link(next));
        uxplay::media_engine *const ready = state.engine.load(std::memory_order_acquire);
        if (ready == nullptr)
        {
            return;
        }
        bool const prefer = state.prefer_usb_link.load();
        ready->set_usb_link(usb_link_line(next, prefer), offer, offer_enable_adapter(next, prefer));
        if (offer && (!previous.link_up || !offer_data_guard(previous)))
        {
            ready->notify(ui_text_string(uxplay::ui_text::data_guard_offer_title),
                          ui_text_string(uxplay::ui_text::data_guard_offer_text), uxplay::tray_command::apply_data_guard);
        }
        else if (offer_enable_adapter(next, prefer) && !previous.link_adapter_disabled)
        {
            ready->notify(ui_text_string(uxplay::ui_text::adapter_disabled_title),
                          ui_text_string(uxplay::ui_text::adapter_disabled_text),
                          uxplay::tray_command::enable_link_adapter);
        }
        else if (prefer && next.link_up && !previous.link_up)
        {
            // a session that is running stays on its link until the phone reconnects
            ready->notify(ui_text_string(uxplay::ui_text::usb_ready_title),
                          ui_text_string(ready->showing_video() ? uxplay::ui_text::usb_ready_text_in_session
                                                                : uxplay::ui_text::usb_ready_text));
        }
        else if (prefer && next.link_missing && !previous.link_missing && !state.hotspot_hint_shown)
        {
            // once per run: people who only charge are not reminded at every plug-in
            state.hotspot_hint_shown = true;
            ready->notify(ui_text_string(uxplay::ui_text::iphone_attached_title),
                          ui_text_string(uxplay::ui_text::turn_on_hotspot_text));
        }
    }

    void on_admin_result(uxplay::usb_link_action action, bool done, std::string const &detail)
    {
        uxplay::media_engine *const ready = bridge().engine.load(std::memory_order_acquire);
        if (ready == nullptr)
        {
            return;
        }
        bool const declined = detail.contains("declined");
        if (action == uxplay::usb_link_action::enable_adapter)
        {
            if (done)
            {
                ready->notify(ui_text_string(uxplay::ui_text::adapter_enabled_title),
                              ui_text_string(uxplay::ui_text::turn_on_hotspot_text));
            }
            else
            {
                ready->notify(ui_text_string(uxplay::ui_text::adapter_still_disabled_title),
                              ui_text_string(declined ? uxplay::ui_text::admin_declined
                                                      : uxplay::ui_text::enable_failed));
            }
            refresh_usb_link_line();
            return;
        }
        if (done)
        {
            ready->notify(ui_text_string(uxplay::ui_text::data_guard_on_title),
                          ui_text_string(uxplay::ui_text::data_guard_on_text));
        }
        else
        {
            ready->notify(ui_text_string(uxplay::ui_text::data_guard_off_title),
                          ui_text_string(declined ? uxplay::ui_text::admin_declined : uxplay::ui_text::apply_failed));
        }
    }

    // The user picked "USB first" or "Wi-Fi only" in the tray.  Only UxPlay's
    // own choice of link changes: the USB device, the iPhone and charging are
    // left alone.  uxplay.cpp moves the mDNS advertisement and ends a session
    // that is on the other link.
    void apply_link_preference(bool prefer_usb_link)
    {
        bridge_state &state = bridge();
        bool const before = state.prefer_usb_link.exchange(prefer_usb_link);
        uxplay::media_engine *const ready = state.engine.load(std::memory_order_acquire);
        if (ready != nullptr)
        {
            ready->set_link_preference(prefer_usb_link);
        }
        if (prefer_usb_link)
        {
            // AirPlay over the cable needs the adapter: offer the fix right away
            uxplay::usb_link_monitor *const link = state.usb_link.load(std::memory_order_acquire);
            if (link != nullptr && link->current().link_adapter_disabled)
            {
                link->enable_link_adapter(ready != nullptr ? ready->window_handle() : nullptr);
            }
        }
        if (before == prefer_usb_link)
        {
            return;
        }
        save_prefer_usb_link(prefer_usb_link);
        uxplay::log::info("AirPlay link preference: {}", prefer_usb_link
                                                             ? "USB first (Wi-Fi when no USB link)"
                                                             : "Wi-Fi only (the USB cable is not used for AirPlay)");
        refresh_usb_link_line();
        emit(NATIVE_MEDIA_EVENT_LINK_PREFERENCE_CHANGED);
    }

    // The user picked a mode in the menu (UI thread).  The engine applies its
    // part at once; the host recomputes the stream size it advertises and then
    // calls native_media_performance_mode_applied().
    void apply_mode(uxplay::performance_mode mode)
    {
        bridge_state &state = bridge();
        int const before = state.mode.exchange(static_cast<int>(mode));
        if (uxplay::media_engine *const ready = state.engine.load(std::memory_order_acquire); ready != nullptr)
        {
            ready->set_performance_mode(mode);
        }
        if (before == static_cast<int>(mode))
        {
            return;
        }
        uxplay::settings::store_number(setting_performance_mode, static_cast<std::uint32_t>(mode));
        emit(NATIVE_MEDIA_EVENT_PERFORMANCE_MODE_CHANGED);
    }

    // The user picked "no cropping" or "crop black bars automatically" (UI
    // thread).  A part framed by hand is the window's and the engine's affair
    // and is not stored: it belongs to the stream it was drawn on.
    void apply_crop_mode(uxplay::crop_mode mode)
    {
        if (uxplay::media_engine *const ready = bridge().engine.load(std::memory_order_acquire); ready != nullptr)
        {
            ready->set_crop_mode(mode);
        }
        uxplay::settings::store_number(setting_crop_mode, static_cast<std::uint32_t>(mode));
    }

    // "Start with Windows" (UI thread)
    void toggle_autostart()
    {
        uxplay::media_engine *const ready = bridge().engine.load(std::memory_order_acquire);
        auto const report_failure = [ready](uxplay::failure const &cause)
        {
            uxplay::log::warning("start with Windows: {}", cause.describe());
            if (ready != nullptr)
            {
                ready->notify(ui_text_string(uxplay::ui_text::start_with_windows),
                              ui_text_string(uxplay::ui_text::autostart_failed));
            }
        };
        auto registered = uxplay::settings::autostart_command();
        if (!registered)
        {
            // the present state is unknown: nothing is changed
            report_failure(registered.error());
            return;
        }
        bool const enable = !registered->has_value();
        if (auto changed = uxplay::settings::set_autostart(enable); !changed)
        {
            report_failure(changed.error());
            return;
        }
        uxplay::log::info("start with Windows: {}", enable ? "on" : "off");
        if (ready != nullptr)
        {
            ready->set_autostart(enable);
        }
    }

    void show_about()
    {
        bridge_state &state = bridge();
        uxplay::media_engine *const ready = state.engine.load(std::memory_order_acquire);
        std::wstring const text = ui_text_with(uxplay::ui_text::about_text,
                                               {state.product_version, state.core_version, state.project_url});
        HWND const owner = ready != nullptr && ready->showing_video() ? static_cast<HWND>(ready->window_handle())
                                                                      : nullptr;
        MessageBoxW(owner, text.c_str(), L"" UXPLAY_PRODUCT_NAME,
                    MB_OK | MB_ICONINFORMATION | (owner == nullptr ? MB_SETFOREGROUND | MB_TOPMOST : 0U));
    }

    uxplay::audio_codec audio_codec_for(unsigned char compression_type)
    {
        switch (compression_type)
        {
        case 2:
            return uxplay::audio_codec::alac;
        case 4:
            return uxplay::audio_codec::aac_lc;
        case 8:
            return uxplay::audio_codec::aac_eld;
        default:
            return uxplay::audio_codec::pcm_s16le;
        }
    }

    void create_engine(uxplay::engine_settings engine_settings)
    {
        bridge_state &state = bridge();
        std::int64_t const started = uxplay::clock::now_ns();
        uxplay::engine_callbacks callbacks;
        callbacks.on_window_closed = [](media_source)
        {
            emit(NATIVE_MEDIA_EVENT_WINDOW_CLOSED);
            // once ever: closing the window does not end the program
            if (!uxplay::settings::stored_number(setting_close_hint_shown))
            {
                uxplay::settings::store_number(setting_close_hint_shown, 1);
                if (uxplay::media_engine *const ready = bridge().engine.load(); ready != nullptr)
                {
                    ready->notify(ui_text_string(uxplay::ui_text::still_running_title),
                                  ui_text_string(uxplay::ui_text::still_running_text));
                }
            }
        };
        callbacks.on_second_launch = []
        {
            bridge_state &current = bridge();
            if (uxplay::media_engine *const ready = current.engine.load(); ready != nullptr)
            {
                ready->notify(ui_text_string(uxplay::ui_text::already_running_title),
                              ui_text_with(uxplay::ui_text::select_on_iphone, {current.receiver_name}));
            }
        };
        callbacks.on_session_ending = []
        {
            // Windows ends the process when this returns: let the host say
            // goodbye on the network first (bounded)
            bridge_state &current = bridge();
            if (current.network_stopped_event == nullptr)
            {
                return;
            }
            ResetEvent(current.network_stopped_event);
            emit(NATIVE_MEDIA_EVENT_SESSION_ENDING);
            if (WaitForSingleObject(current.network_stopped_event, session_end_wait_ms) != WAIT_OBJECT_0)
            {
                uxplay::log::warning("shutdown: the network services did not stop within {} ms", session_end_wait_ms);
            }
        };
        callbacks.on_display_changed = []
        {
            bridge_state &current = bridge();
            if (auto display = uxplay::gpu_device::primary_display_mode(); display)
            {
                std::scoped_lock lock{current.display_mutex};
                current.display = std::move(*display);
            }
            else
            {
                uxplay::log::warning("display query after a display change: {}", display.error().describe());
            }
            emit(NATIVE_MEDIA_EVENT_DISPLAY_CHANGED);
        };
        callbacks.on_device_lost = []
        {
            emit(NATIVE_MEDIA_EVENT_DEVICE_LOST);
        };
        callbacks.on_usb_device_change = []
        {
            if (uxplay::usb_link_monitor *const link = bridge().usb_link.load(); link != nullptr)
            {
                link->refresh();
            }
        };
        callbacks.on_tray_command = [](uxplay::tray_command command)
        {
            bridge_state &current = bridge();
            switch (command)
            {
            case uxplay::tray_command::apply_data_guard:
                if (uxplay::usb_link_monitor *const link = current.usb_link.load(); link != nullptr)
                {
                    uxplay::media_engine *const ready = current.engine.load();
                    link->apply_data_guard(ready != nullptr ? ready->window_handle() : nullptr);
                }
                break;
            case uxplay::tray_command::enable_link_adapter:
                if (uxplay::usb_link_monitor *const link = current.usb_link.load(); link != nullptr)
                {
                    uxplay::media_engine *const ready = current.engine.load();
                    link->enable_link_adapter(ready != nullptr ? ready->window_handle() : nullptr);
                }
                break;
            case uxplay::tray_command::prefer_usb_link:
                apply_link_preference(true);
                break;
            case uxplay::tray_command::wifi_only:
                apply_link_preference(false);
                break;
            case uxplay::tray_command::open_log:
                if (!current.log_path.empty())
                {
                    // ShellExecute reports success with a value above 32
                    auto const opened = reinterpret_cast<std::intptr_t>(
                        ShellExecuteW(nullptr, L"open", current.log_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
                    if (opened <= 32)
                    {
                        uxplay::log::warning("open log file: ShellExecute failed with code {} for {}", opened,
                                             uxplay::utf16_to_utf8(current.log_path));
                    }
                }
                break;
            case uxplay::tray_command::exit:
                emit(NATIVE_MEDIA_EVENT_EXIT_REQUESTED);
                break;
            case uxplay::tray_command::mode_quality:
                apply_mode(uxplay::performance_mode::quality);
                break;
            case uxplay::tray_command::mode_balanced:
                apply_mode(uxplay::performance_mode::balanced);
                break;
            case uxplay::tray_command::mode_performance:
                apply_mode(uxplay::performance_mode::performance);
                break;
            case uxplay::tray_command::crop_off:
                apply_crop_mode(uxplay::crop_mode::off);
                break;
            case uxplay::tray_command::crop_automatic:
                apply_crop_mode(uxplay::crop_mode::automatic);
                break;
            case uxplay::tray_command::toggle_autostart:
                toggle_autostart();
                break;
            case uxplay::tray_command::about:
                show_about();
                break;
            case uxplay::tray_command::toggle_fullscreen:
            case uxplay::tray_command::toggle_always_on_top:
            case uxplay::tray_command::toggle_mute:
            case uxplay::tray_command::crop_select:
                break; // handled by the window / the engine
            }
        };
        auto created = uxplay::media_engine::create(std::move(engine_settings), std::move(callbacks));
        bool failed = false;
        if (!created)
        {
            // without the engine there is no window and no tray icon: the host
            // tells the user and exits (NATIVE_MEDIA_EVENT_START_FAILED)
            state.failure_text = created.error().describe();
            uxplay::log::error("media engine start: {}", state.failure_text);
            failed = true;
        }
        else
        {
            state.engine_owner = std::move(*created);
            state.engine.store(state.engine_owner.get(), std::memory_order_release);
            state.engine_owner->set_log_available(!state.log_path.empty());
            if (auto registered = uxplay::settings::autostart_command(); !registered)
            {
                uxplay::log::warning("start with Windows: {}", registered.error().describe());
            }
            else if (registered->has_value())
            {
                state.engine_owner->set_autostart(true);
                // keep the registration pointing at this copy of the program
                if (auto refreshed = uxplay::settings::set_autostart(true); !refreshed)
                {
                    uxplay::log::warning("start with Windows: {}", refreshed.error().describe());
                }
            }
            // first run: the program has no window until a phone connects
            if (!uxplay::settings::stored_number(setting_ready_hint_shown))
            {
                uxplay::settings::store_number(setting_ready_hint_shown, 1);
                state.engine_owner->notify(ui_text_string(uxplay::ui_text::ready_title),
                                           ui_text_with(uxplay::ui_text::select_on_iphone, {state.receiver_name}));
            }

            // AirPlay over the USB cable: status, hints and the cellular-data guard
            uxplay::usb_link_events link_events;
            link_events.on_change = [](uxplay::usb_link_state const &next)
            {
                on_usb_link_change(next);
            };
            link_events.on_admin_result = [](uxplay::usb_link_action action, bool done, std::string const &detail)
            {
                on_admin_result(action, done, detail);
            };
            state.engine_owner->set_link_preference(state.prefer_usb_link.load());
            if (auto link = uxplay::usb_link_monitor::create(std::move(link_events)); !link)
            {
                // AirPlay over the cable still works; only its status/guard are missing
                uxplay::log::warning("USB link monitor unavailable: {}", link.error().describe());
            }
            else
            {
                state.usb_link_owner = std::move(*link);
                state.usb_link.store(state.usb_link_owner.get(), std::memory_order_release);
            }
            uxplay::log::info("media engine ready in {:.0f} ms (initialized in parallel with the AirPlay server)",
                              static_cast<double>(uxplay::clock::now_ns() - started) / 1e6);
        }
        {
            std::scoped_lock lock{state.ready_mutex};
            state.start_finished.store(true, std::memory_order_release);
        }
        state.ready_wake.notify_all();
        if (failed)
        {
            emit(NATIVE_MEDIA_EVENT_START_FAILED);
        }
    }
}

extern "C" int native_media_start(const native_media_settings *settings)
{
    bridge_state &state = bridge();
    std::scoped_lock lock{state.lifecycle_mutex};
    if (state.start_thread.joinable())
    {
        return 0;
    }
    uxplay::log::install_sink(settings->log_callback, static_cast<uxplay::log::level>(settings->log_level));
    for (std::string const &warning : state.early_warnings)
    {
        uxplay::log::warning("{}", warning);
    }
    state.early_warnings.clear();
    // before the engine thread creates the window, tray menu and control bar
    apply_ui_language(settings->ui_language);
    state.event_callback = settings->event_callback;
    state.event_user_data = settings->event_user_data;
    state.log_path = settings->log_path != nullptr ? uxplay::utf8_to_utf16(settings->log_path) : std::wstring{};
    // the settings the program stored under its earlier name, once
    if (auto adopted = uxplay::settings::adopt_legacy_settings(); !adopted)
    {
        uxplay::log::warning("settings of the earlier program name: {}", adopted.error().describe());
    }
    else if (!adopted->empty())
    {
        uxplay::log::info("{}", *adopted);
    }
    // synchronous: uxplay.cpp reads these before the mDNS advertisement starts
    state.prefer_usb_link.store(load_prefer_usb_link());
    int const requested_mode = settings->performance_mode;
    uxplay::performance_mode const mode =
        requested_mode >= NATIVE_MEDIA_MODE_QUALITY && requested_mode <= NATIVE_MEDIA_MODE_PERFORMANCE
            ? mode_from_number(requested_mode)
            : mode_from_number(static_cast<int>(uxplay::settings::stored_number(setting_performance_mode)
                                                    .value_or(NATIVE_MEDIA_MODE_BALANCED)));
    state.mode.store(static_cast<int>(mode));
    auto const text_or_empty = [](const char *text)
    {
        return text != nullptr ? uxplay::utf8_to_utf16(text) : std::wstring{};
    };
    state.receiver_name = text_or_empty(settings->window_title);
    state.product_version = text_or_empty(settings->product_version);
    state.core_version = text_or_empty(settings->core_version);
    state.project_url = text_or_empty(settings->project_url);
    if (state.network_stopped_event == nullptr)
    {
        state.network_stopped_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (state.network_stopped_event == nullptr)
        {
            uxplay::log::warning("shutdown wait unavailable: {}",
                                 uxplay::make_win32_failure("CreateEventW", GetLastError()).describe());
        }
    }

    uxplay::system_tuning::process_tuning_report const tuning = uxplay::system_tuning::apply_process_tuning();
    // process priority and power throttling are set per mirroring session
    // (system_tuning::set_session_active), not for the idle program
    uxplay::log::info("host tuning: timer resolution kept {}, per-monitor DPI {}",
                      tuning.timer_resolution_ignored_opt_out, tuning.dpi_awareness_set);

    // cheap and synchronous: needed now for the AirPlay display advertisement
    if (auto display = uxplay::gpu_device::primary_display_mode(); display)
    {
        std::scoped_lock display_lock{state.display_mutex};
        state.display = std::move(*display);
    }
    else
    {
        uxplay::log::warning("display query: {}", display.error().describe());
    }

    uxplay::engine_settings engine_settings;
    engine_settings.window_title = uxplay::utf8_to_utf16(settings->window_title != nullptr ? settings->window_title
                                                                                           : UXPLAY_PRODUCT_NAME);
    engine_settings.start_fullscreen = settings->start_fullscreen;
    engine_settings.timestamp_sync = settings->timestamp_sync;
    engine_settings.keep_window_on_stop = settings->keep_window_on_stop;
    engine_settings.tray_icon = settings->tray_icon;
    engine_settings.mode = mode;
    int const requested_crop = settings->crop_mode;
    bool const automatic_crop =
        requested_crop == 0 || requested_crop == 1
            ? requested_crop == 1
            : uxplay::settings::stored_number(setting_crop_mode).value_or(0) == 1;
    engine_settings.crop = automatic_crop ? uxplay::crop_mode::automatic : uxplay::crop_mode::off;
    engine_settings.gpu = settings->gpu_preference == 1   ? uxplay::gpu_preference::integrated
                          : settings->gpu_preference == 2 ? uxplay::gpu_preference::discrete
                          : settings->gpu_preference == 3 ? uxplay::gpu_preference::software
                                                          : uxplay::gpu_preference::automatic;
    state.start_finished.store(false);
    state.start_requested.store(true, std::memory_order_release);
    state.start_thread = std::thread{[engine_settings = std::move(engine_settings)]() mutable
    {
        create_engine(std::move(engine_settings));
    }};
    return 0;
}

extern "C" void native_media_stop(void)
{
    bridge_state &state = bridge();
    std::scoped_lock lock{state.lifecycle_mutex};
    if (state.start_thread.joinable())
    {
        state.start_thread.join();
    }
    // the window thread first: its menu commands use the link monitor and
    // the engine, which go next
    if (state.engine_owner)
    {
        state.engine_owner->stop_window_callbacks();
    }
    state.usb_link.store(nullptr);
    state.usb_link_owner.reset();
    state.engine.store(nullptr);
    state.engine_owner.reset();
}

extern "C" bool native_media_query_display(native_media_display_info *info)
{
    bridge_state &state = bridge();
    if (info == nullptr)
    {
        return false;
    }
    {
        std::scoped_lock lock{state.display_mutex};
        if (!state.display || state.display->width <= 0 || state.display->height <= 0)
        {
            return false;
        }
        info->width = state.display->width;
        info->height = state.display->height;
        info->refresh_hz = static_cast<int>(std::lround(state.display->refresh_hz()));
    }
    uxplay::media_engine *const ready = engine(display_query_wait);
    if (ready == nullptr && !state.start_finished.load(std::memory_order_acquire))
    {
        // the codec offer below is decided without the GPU's answer
        uxplay::log::warning("display query: the engine was not ready within {} s; assuming no hardware decoder",
                             display_query_wait.count());
    }
    info->hevc_hardware = (ready != nullptr && ready->hevc_hardware()) ? 1 : 0;
    info->h264_hardware = (ready != nullptr && ready->h264_hardware()) ? 1 : 0;
    return true;
}

extern "C" const char *native_media_failure_text(void)
{
    bridge_state &state = bridge();
    return state.start_finished.load(std::memory_order_acquire) ? state.failure_text.c_str() : "";
}

extern "C" int native_media_performance_mode(void)
{
    return bridge().mode.load();
}

extern "C" void native_media_performance_mode_applied(bool applies_next_connection)
{
    bridge_state &state = bridge();
    uxplay::media_engine *const ready = state.engine.load(std::memory_order_acquire);
    if (ready == nullptr)
    {
        return;
    }
    uxplay::performance_mode const mode = mode_from_number(state.mode.load());
    std::wstring text = ui_text_string(mode_description(mode));
    if (applies_next_connection)
    {
        text += L'\n';
        text += uxplay::ui_string(uxplay::ui_text::mode_next_connection);
    }
    ready->notify(ui_text_string(mode_label(mode)), std::move(text));
}

extern "C" int native_media_activate_running_instance(const char *receiver_name)
{
    if (receiver_name == nullptr)
    {
        return 0;
    }
    struct search
    {
        UINT message = 0;
        WPARAM tag = 0;
        int notified = 0;
    };
    search found;
    found.message = RegisterWindowMessageW(uxplay::second_launch_message_name);
    found.tag = uxplay::second_launch_tag(uxplay::utf8_to_utf16(receiver_name));
    if (found.message == 0)
    {
        uxplay::log::warning("single instance: {}",
                             uxplay::make_win32_failure("RegisterWindowMessageW", GetLastError()).describe());
        return 0;
    }
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL
        {
            auto *const current = reinterpret_cast<search *>(parameter);
            wchar_t class_name[64]{};
            // sent with a time limit, and counted only when the window
            // answers for this receiver name: a hung instance, or one with
            // another name, is not "notified"
            DWORD_PTR answer = 0;
            if (GetClassNameW(window, class_name, 64) > 0 &&
                std::wstring_view{class_name} == uxplay::video_window_class_name &&
                SendMessageTimeoutW(window, current->message, current->tag, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000,
                                    &answer) != 0 &&
                answer == 1)
            {
                ++current->notified;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found.notified;
}

extern "C" void native_media_network_stopped(void)
{
    if (HANDLE const stopped = bridge().network_stopped_event; stopped != nullptr)
    {
        SetEvent(stopped);
    }
}

extern "C" void native_media_show_start_failure(const char *reason, const char *log_path)
{
    if (!ui_language_applied.load())
    {
        apply_ui_language(0); // a failure before native_media_start: still the user's language
    }
    std::wstring const text =
        ui_text_with(uxplay::ui_text::start_failed_text,
                     {uxplay::utf8_to_utf16(reason != nullptr ? reason : ""),
                      uxplay::utf8_to_utf16(log_path != nullptr ? log_path : "")});
    std::wstring const title =
        std::format(L"" UXPLAY_PRODUCT_NAME L" · {}", uxplay::ui_string(uxplay::ui_text::start_failed_title));
    MessageBoxW(nullptr, text.c_str(), title.c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
}

extern "C" void native_media_install_crash_report(const char *directory)
{
    if (directory == nullptr)
    {
        return;
    }
    // called first thing in main, before the log sink exists: the outcome is
    // kept and logged by native_media_start
    bridge_state &state = bridge();
    std::wstring const folder = uxplay::utf8_to_utf16(directory);
    if (std::optional<std::wstring> const previous = uxplay::crash_report::take_previous_record(folder); previous)
    {
        state.early_warnings.push_back(
            std::format("a previous run ended with a crash; its record is {} (its minidump, when one was "
                        "written, is {} in the same folder)",
                        uxplay::utf16_to_utf8(*previous),
                        uxplay::utf16_to_utf8(uxplay::crash_report::kept_dump_file_name)));
    }
    if (auto installed = uxplay::crash_report::install(folder); !installed)
    {
        state.early_warnings.push_back(std::format("crash report unavailable: {}", installed.error().describe()));
    }
    else if (!installed->empty())
    {
        state.early_warnings.push_back(*installed);
    }
}

extern "C" void native_media_airplay_video_configure(int codec, int width, int height)
{
    if (uxplay::media_engine *const ready = engine(); ready != nullptr)
    {
        ready->video_configure(media_source::airplay, codec == 1 ? uxplay::video_codec::h265 : uxplay::video_codec::h264,
                               width, height);
    }
}

extern "C" void native_media_airplay_video_frame(const unsigned char *data, int length, uint64_t local_time_ns,
                                                 uint64_t remote_time_ns)
{
    bridge_state &state = bridge();
    uxplay::media_engine *const ready = engine();
    if (ready == nullptr || data == nullptr || length <= 4)
    {
        return;
    }
    // raop_rtp_mirror marks units whose decryption failed with data[0] = 1
    if (data[0] != 0)
    {
        if (state.invalid_video_units.fetch_add(1) < 5)
        {
            uxplay::log::warning("AirPlay video unit failed decryption, skipped");
        }
        return;
    }
    std::int64_t const media_time = airplay_media_time(local_time_ns, remote_time_ns);
    ready->video_access_unit(media_source::airplay, std::span<std::uint8_t const>{data, static_cast<std::size_t>(length)},
                             media_time, uxplay::clock::now_ns());
}

extern "C" void native_media_airplay_video_pause(void)
{
    if (uxplay::media_engine *const ready = engine(); ready != nullptr)
    {
        ready->video_pause(media_source::airplay);
    }
}

extern "C" void native_media_airplay_video_resume(void)
{
    if (uxplay::media_engine *const ready = engine(); ready != nullptr)
    {
        ready->video_resume(media_source::airplay);
    }
}

extern "C" void native_media_airplay_video_stop(void)
{
    bridge_state &state = bridge();
    if (uxplay::media_engine *const ready = state.engine.load(std::memory_order_acquire); ready != nullptr)
    {
        ready->video_stop(media_source::airplay);
    }
    std::scoped_lock lock{state.mapping_mutex};
    state.remote_to_local_offset.reset();
}

extern "C" void native_media_airplay_audio_configure(unsigned char compression_type, bool timestamp_sync,
                                                     double audio_offset_ms)
{
    if (uxplay::media_engine *const ready = engine(); ready != nullptr)
    {
        ready->audio_configure(media_source::airplay, audio_codec_for(compression_type), 44100, 2, timestamp_sync,
                               audio_offset_ms);
    }
}

extern "C" void native_media_airplay_audio_packet(const unsigned char *data, int length, uint64_t local_time_ns,
                                                  uint64_t remote_time_ns)
{
    uxplay::media_engine *const ready = engine();
    if (ready == nullptr || data == nullptr || length <= 0)
    {
        return;
    }
    std::int64_t const now = uxplay::clock::now_ns();
    std::int64_t const media_time = (local_time_ns == 0 && remote_time_ns == 0)
                                        ? now
                                        : airplay_media_time(local_time_ns, remote_time_ns);
    ready->audio_packet(media_source::airplay, std::span<std::uint8_t const>{data, static_cast<std::size_t>(length)},
                        media_time, now);
}

extern "C" void native_media_airplay_audio_stop(void)
{
    if (uxplay::media_engine *const ready = bridge().engine.load(std::memory_order_acquire); ready != nullptr)
    {
        ready->audio_stop(media_source::airplay);
    }
}

extern "C" void native_media_set_volume(double linear_gain)
{
    if (uxplay::media_engine *const ready = engine(); ready != nullptr)
    {
        ready->set_volume(media_source::airplay, linear_gain);
    }
}

extern "C" int native_media_set_airplay_local_address(const char *local_address)
{
    // by the adapter that owns the address: a PC on the iPhone hotspot over
    // Wi-Fi also has 172.20.10.x, but that is Wi-Fi, not the cable
    bool const over_usb = local_address != nullptr && uxplay::is_usb_link_address(local_address);
    if (uxplay::media_engine *const ready = engine(); ready != nullptr)
    {
        ready->set_airplay_over_usb(over_usb);
    }
    return over_usb ? 1 : 0;
}

extern "C" bool native_media_is_usb_link_address(const char *local_address)
{
    return local_address != nullptr && uxplay::is_usb_link_address(local_address);
}

extern "C" bool native_media_prefer_usb_link(void)
{
    return bridge().prefer_usb_link.load();
}

extern "C" bool native_media_usb_link_up(void)
{
    uxplay::usb_link_monitor *const link = bridge().usb_link.load(std::memory_order_acquire);
    return link != nullptr && link->current().link_up;
}

extern "C" void native_media_link_preference_applied(bool session_ended)
{
    uxplay::media_engine *const ready = engine();
    if (ready == nullptr)
    {
        return;
    }
    bridge_state &state = bridge();
    bool const prefer = state.prefer_usb_link.load();
    uxplay::ui_text const title = prefer ? uxplay::ui_text::switched_to_usb_first : uxplay::ui_text::switched_to_wifi_only;
    std::wstring text = session_ended ? ui_text_with(uxplay::ui_text::choose_receiver_again, {state.receiver_name})
                                      : ui_text_string(prefer ? uxplay::ui_text::usb_when_hotspot_on
                                                              : uxplay::ui_text::usb_charging_only);
    ready->notify(ui_text_string(title), std::move(text));
}

extern "C" void native_media_notify(const char *title, const char *text)
{
    if (uxplay::media_engine *const ready = engine(); ready != nullptr && title != nullptr && text != nullptr)
    {
        ready->notify(uxplay::utf8_to_utf16(title), uxplay::utf8_to_utf16(text));
    }
}

extern "C" void native_media_notify_pin(const char *pin)
{
    if (uxplay::media_engine *const ready = engine(); ready != nullptr && pin != nullptr)
    {
        ready->notify(ui_text_string(uxplay::ui_text::pairing_pin_title), uxplay::utf8_to_utf16(pin));
    }
}
