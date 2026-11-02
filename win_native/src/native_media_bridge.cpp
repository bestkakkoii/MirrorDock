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

import std;
import uxplay_status;
import uxplay_log;
import uxplay_clock;
import uxplay_media_types;
import uxplay_media_engine;
import uxplay_usb_mirror;
import uxplay_usb_link;
import uxplay_video_decoder;
import uxplay_audio_decoder;
import uxplay_system_tuning;
import uxplay_gpu_device;
import uxplay_video_window;

namespace
{
    using uxplay::media_source;

    constexpr auto engine_start_wait = std::chrono::seconds{3};

    struct bridge_state
    {
        std::mutex lifecycle_mutex;
        std::thread start_thread;
        std::mutex ready_mutex;
        std::condition_variable ready_wake;
        std::atomic<bool> start_requested{false};
        std::atomic<bool> start_finished{false};
        std::unique_ptr<uxplay::media_engine> engine_owner;
        std::unique_ptr<uxplay::usb_mirror> usb_owner;
        std::unique_ptr<uxplay::usb_link_monitor> usb_link_owner;
        std::atomic<uxplay::media_engine *> engine{nullptr};
        std::atomic<uxplay::usb_mirror *> usb{nullptr};
        std::atomic<uxplay::usb_link_monitor *> usb_link{nullptr};
        // USB link monitor thread only
        std::optional<uxplay::usb_link_state> last_usb_link;
        bool hotspot_hint_shown = false;
        // AirPlay link preference (persisted in HKCU)
        std::atomic<bool> prefer_usb_link{true};
        std::optional<uxplay::display_mode_info> display;
        native_media_event_callback event_callback = nullptr;
        void *event_user_data = nullptr;
        std::wstring log_path;

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

    uxplay::media_engine *engine()
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
        state.ready_wake.wait_for(lock, engine_start_wait, [&state]
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

    // ---- AirPlay link preference (USB first / Wi-Fi only) --------------------

    constexpr wchar_t settings_key[] = L"Software\\UxPlay";
    constexpr wchar_t prefer_usb_link_value[] = L"prefer_usb_link";

    bool load_prefer_usb_link()
    {
        DWORD value = 1;
        DWORD size = sizeof(value);
        LSTATUS const result = RegGetValueW(HKEY_CURRENT_USER, settings_key, prefer_usb_link_value, RRF_RT_REG_DWORD,
                                            nullptr, &value, &size);
        if (result == ERROR_FILE_NOT_FOUND)
        {
            return true; // default: USB first
        }
        if (result != ERROR_SUCCESS)
        {
            uxplay::log::warning("settings: {} (using USB first)",
                                 uxplay::make_win32_failure("RegGetValueW(prefer_usb_link)",
                                                            static_cast<unsigned long>(result))
                                     .describe());
            return true;
        }
        return value != 0;
    }

    void save_prefer_usb_link(bool prefer_usb_link)
    {
        DWORD const value = prefer_usb_link ? 1 : 0;
        LSTATUS const result = RegSetKeyValueW(HKEY_CURRENT_USER, settings_key, prefer_usb_link_value, REG_DWORD,
                                               &value, sizeof(value));
        if (result != ERROR_SUCCESS)
        {
            uxplay::log::warning("settings: link preference not saved: {}",
                                 uxplay::make_win32_failure("RegSetKeyValueW(prefer_usb_link)",
                                                            static_cast<unsigned long>(result))
                                     .describe());
        }
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
            if (link.link_up)
            {
                return L"USB：線路已連線，但目前只用 Wi-Fi 投影";
            }
            if (link.iphone_attached)
            {
                return L"USB：iPhone 已接上（只用 Wi-Fi 投影，USB 線只充電）";
            }
            return L"USB：未接上 iPhone（只用 Wi-Fi 投影）";
        }
        if (link.link_adapter_disabled)
        {
            return L"USB：Apple USB 網路卡已被停用，USB 投影無法使用";
        }
        if (link.link_up)
        {
            return std::format(L"USB：已連線（{}）— 投影走傳輸線", uxplay::utf8_to_utf16(link.local_address));
        }
        if (link.iphone_attached)
        {
            return L"USB：iPhone 已接上，開啟「個人熱點」即走 USB";
        }
        return L"USB：未接上 iPhone（目前走 Wi-Fi）";
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
        if (!next.iphone_attached)
        {
            state.hotspot_hint_shown = false;
        }
        uxplay::media_engine *const ready = state.engine.load(std::memory_order_acquire);
        if (ready == nullptr)
        {
            return;
        }
        bool const prefer = state.prefer_usb_link.load();
        ready->set_usb_link(usb_link_line(next, prefer), offer, offer_enable_adapter(next, prefer));
        if (offer && (!previous.link_up || !offer_data_guard(previous)))
        {
            ready->notify(L"保護手機流量",
                          L"電腦正經由 iPhone 的行動數據上網。點這則通知並在系統視窗按「是」，電腦上網就固定走 "
                          L"Wi-Fi（只需一次；投影本身不耗流量）。",
                          uxplay::tray_command::apply_data_guard);
        }
        else if (offer_enable_adapter(next, prefer) && !previous.link_adapter_disabled)
        {
            ready->notify(L"USB 投影的網路卡被停用了",
                          L"點這則通知並在系統視窗按「是」即可重新啟用。之後想改走 Wi-Fi，請用系統匣的「只用 "
                          L"Wi-Fi」，不必停用網路卡（停用可能讓 USB 充電中斷）。",
                          uxplay::tray_command::enable_link_adapter);
        }
        else if (prefer && next.link_up && !previous.link_up)
        {
            ready->notify(L"USB 已連線",
                          L"在 iPhone「螢幕鏡像輸出」選 UxPlay，畫面就走 USB 傳輸線（不耗行動數據）。");
        }
        else if (prefer && next.link_missing && !previous.link_missing && !state.hotspot_hint_shown)
        {
            state.hotspot_hint_shown = true;
            ready->notify(L"iPhone 已接上 USB",
                          L"要走 USB 投影：在 iPhone 開「設定 › 個人熱點 › 允許其他人加入」。投影本身不耗行動數據。");
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
                ready->notify(L"已重新啟用 Apple USB 網路卡",
                              L"iPhone 開著個人熱點時，USB 投影就能使用。");
            }
            else if (declined)
            {
                ready->notify(L"網路卡仍是停用狀態",
                              L"系統視窗被按了「否」。可從通知區域選單的「重新啟用 Apple USB 網路卡」再試一次。");
            }
            else
            {
                ready->notify(L"網路卡仍是停用狀態", L"重新啟用沒有完成（原因見記錄檔）。可從通知區域選單再試一次。");
            }
            refresh_usb_link_line();
            return;
        }
        if (done)
        {
            ready->notify(L"已保護手機流量", L"電腦上網固定走 Wi-Fi，iPhone 傳輸線只用來投影（設定已保存）。");
        }
        else if (declined)
        {
            ready->notify(L"未保護手機流量", L"系統視窗被按了「否」。可從通知區域選單的「保護手機流量」再試一次。");
        }
        else
        {
            ready->notify(L"未保護手機流量", L"設定沒有完成（原因見記錄檔）。可從通知區域選單再試一次。");
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

    void create_engine(uxplay::engine_settings engine_settings, bool usb_mirroring)
    {
        bridge_state &state = bridge();
        std::int64_t const started = uxplay::clock::now_ns();
        uxplay::engine_callbacks callbacks;
        callbacks.on_window_closed = [](media_source source)
        {
            if (source == media_source::usb)
            {
                if (uxplay::usb_mirror *const usb = bridge().usb.load(); usb != nullptr)
                {
                    uxplay::log::info("window closed: stopping USB mirroring (replug the iPhone to start again)");
                    usb->stop_current_session();
                }
                return;
            }
            emit(NATIVE_MEDIA_EVENT_WINDOW_CLOSED);
        };
        callbacks.on_usb_device_change = []
        {
            if (uxplay::usb_link_monitor *const link = bridge().usb_link.load(); link != nullptr)
            {
                link->refresh();
            }
            if (uxplay::usb_mirror *const usb = bridge().usb.load(); usb != nullptr)
            {
                usb->rescan();
            }
        };
        callbacks.on_tray_command = [](uxplay::tray_command command)
        {
            bridge_state &current = bridge();
            switch (command)
            {
            case uxplay::tray_command::toggle_usb:
                if (uxplay::usb_mirror *const usb = current.usb.load(); usb != nullptr)
                {
                    bool const enable = !usb->enabled();
                    usb->set_enabled(enable);
                    if (uxplay::media_engine *const ready = current.engine.load(); ready != nullptr)
                    {
                        ready->set_legacy_usb(true, enable);
                    }
                }
                break;
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
                    ShellExecuteW(nullptr, L"open", current.log_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                break;
            case uxplay::tray_command::exit:
                emit(NATIVE_MEDIA_EVENT_EXIT_REQUESTED);
                break;
            case uxplay::tray_command::toggle_fullscreen:
                break;
            }
        };
        auto created = uxplay::media_engine::create(std::move(engine_settings), std::move(callbacks));
        if (!created)
        {
            uxplay::log::error("media engine start: {}", created.error().describe());
        }
        else
        {
            state.engine_owner = std::move(*created);
            state.engine.store(state.engine_owner.get(), std::memory_order_release);
            // the USB watcher always exists so the tray menu can switch it on later
            uxplay::display_summary const display = state.engine_owner->display();
            uxplay::usb_mirror_events usb_events;
            usb_events.on_started = []
            {
                emit(NATIVE_MEDIA_EVENT_USB_STARTED);
            };
            usb_events.on_stopped = []
            {
                emit(NATIVE_MEDIA_EVENT_USB_STOPPED);
            };
            auto usb = uxplay::usb_mirror::create(*state.engine_owner, std::move(usb_events), display.width,
                                                  display.height);
            if (!usb)
            {
                uxplay::log::error("legacy QuickTime USB capture unavailable: {}", usb.error().describe());
                state.engine_owner->set_legacy_usb(false, false);
            }
            else
            {
                state.usb_owner = std::move(*usb);
                state.usb_owner->set_enabled(usb_mirroring);
                // the legacy toggle is offered only when asked for with -usb
                state.engine_owner->set_legacy_usb(usb_mirroring, usb_mirroring);
                state.usb.store(state.usb_owner.get(), std::memory_order_release);
                if (usb_mirroring)
                {
                    uxplay::log::info("legacy QuickTime USB capture enabled (-usb): streams only on iOS 15 and "
                                      "earlier; on iOS 16+ use AirPlay over the USB cable (Personal Hotspot)");
                }
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
    state.event_callback = settings->event_callback;
    state.event_user_data = settings->event_user_data;
    state.log_path = settings->log_path != nullptr ? uxplay::utf8_to_utf16(settings->log_path) : std::wstring{};
    // synchronous: uxplay.cpp reads it before the mDNS advertisement starts
    state.prefer_usb_link.store(load_prefer_usb_link());

    uxplay::system_tuning::process_tuning_report const tuning = uxplay::system_tuning::apply_process_tuning();
    uxplay::log::info("host tuning: power throttling off {}, timer resolution kept {}, priority above normal {}, "
                      "per-monitor DPI {}",
                      tuning.power_throttling_disabled, tuning.timer_resolution_ignored_opt_out, tuning.priority_raised,
                      tuning.dpi_awareness_set);

    // cheap and synchronous: needed now for the AirPlay display advertisement
    if (auto display = uxplay::gpu_device::primary_display_mode(); display)
    {
        state.display = std::move(*display);
    }
    else
    {
        uxplay::log::warning("display query: {}", display.error().describe());
    }

    uxplay::engine_settings engine_settings;
    engine_settings.window_title = uxplay::utf8_to_utf16(settings->window_title != nullptr ? settings->window_title
                                                                                           : "UxPlay");
    engine_settings.start_fullscreen = settings->start_fullscreen;
    engine_settings.timestamp_sync = settings->timestamp_sync;
    engine_settings.keep_window_on_stop = settings->keep_window_on_stop;
    engine_settings.tray_icon = settings->tray_icon;
    engine_settings.gpu = settings->gpu_preference == 1   ? uxplay::gpu_preference::integrated
                          : settings->gpu_preference == 2 ? uxplay::gpu_preference::discrete
                          : settings->gpu_preference == 3 ? uxplay::gpu_preference::software
                                                          : uxplay::gpu_preference::automatic;
    bool const usb_mirroring = settings->usb_mirroring;
    state.start_finished.store(false);
    state.start_requested.store(true, std::memory_order_release);
    state.start_thread = std::thread{[engine_settings = std::move(engine_settings), usb_mirroring]() mutable
    {
        create_engine(std::move(engine_settings), usb_mirroring);
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
    state.usb_link.store(nullptr);
    state.usb_link_owner.reset();
    state.usb.store(nullptr);
    state.usb_owner.reset();
    state.engine.store(nullptr);
    state.engine_owner.reset();
}

extern "C" bool native_media_query_display(native_media_display_info *info)
{
    bridge_state &state = bridge();
    if (!state.display || info == nullptr || state.display->width <= 0 || state.display->height <= 0)
    {
        return false;
    }
    info->width = state.display->width;
    info->height = state.display->height;
    info->refresh_hz = static_cast<int>(std::lround(state.display->refresh_hz()));
    uxplay::media_engine *const ready = engine();
    info->hevc_hardware = (ready != nullptr && ready->hevc_hardware()) ? 1 : 0;
    return true;
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
    bool const prefer = bridge().prefer_usb_link.load();
    std::wstring const title = prefer ? L"已改為 USB 優先" : L"已改為只用 Wi-Fi";
    std::wstring text = prefer ? L"iPhone 開著個人熱點並接上傳輸線時，投影走 USB。"
                               : L"投影只走 Wi-Fi；USB 線可以繼續接著充電，不用停用任何裝置。";
    if (session_ended)
    {
        text += L"目前這次投影已結束，請在 iPhone「螢幕鏡像輸出」重新選 UxPlay。";
    }
    ready->notify(title, std::move(text));
}

extern "C" void native_media_notify(const char *title, const char *text)
{
    if (uxplay::media_engine *const ready = engine(); ready != nullptr && title != nullptr && text != nullptr)
    {
        ready->notify(uxplay::utf8_to_utf16(title), uxplay::utf8_to_utf16(text));
    }
}
