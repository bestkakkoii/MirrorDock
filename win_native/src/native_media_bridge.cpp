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
        std::atomic<uxplay::media_engine *> engine{nullptr};
        std::atomic<uxplay::usb_mirror *> usb{nullptr};
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
                        ready->set_usb_enabled(enable);
                    }
                }
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
                uxplay::log::error("USB mirroring unavailable: {}", usb.error().describe());
                state.engine_owner->set_usb_enabled(false);
            }
            else
            {
                state.usb_owner = std::move(*usb);
                state.usb_owner->set_enabled(usb_mirroring);
                state.engine_owner->set_usb_enabled(usb_mirroring);
                state.usb.store(state.usb_owner.get(), std::memory_order_release);
                if (usb_mirroring)
                {
                    uxplay::log::info("USB mirroring enabled: plug in the iPhone (unlocked) to mirror over the cable");
                }
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

extern "C" void native_media_set_airplay_transport(int over_usb)
{
    if (uxplay::media_engine *const ready = engine(); ready != nullptr)
    {
        ready->set_airplay_over_usb(over_usb != 0);
    }
}

extern "C" void native_media_notify(const char *title, const char *text)
{
    if (uxplay::media_engine *const ready = engine(); ready != nullptr && title != nullptr && text != nullptr)
    {
        ready->notify(uxplay::utf8_to_utf16(title), uxplay::utf8_to_utf16(text));
    }
}
