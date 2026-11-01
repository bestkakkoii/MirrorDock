module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

module uxplay_media_engine;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_clock;
import uxplay_media_types;
import uxplay_gpu_device;
import uxplay_video_window;
import uxplay_video_frame;
import uxplay_video_decoder;
import uxplay_video_presenter;
import uxplay_audio_decoder;
import uxplay_audio_output;
import uxplay_system_tuning;

namespace uxplay
{
    namespace
    {
        constexpr std::int64_t window_ns = 2 * clock::ns_per_second;
        constexpr std::int64_t minimum_delay_ns = 8 * clock::ns_per_ms;
        constexpr std::int64_t maximum_delay_ns = 220 * clock::ns_per_ms;
        constexpr std::int64_t decay_interval_ns = 100 * clock::ns_per_ms;
        constexpr std::int64_t decay_min_step_ns = clock::ns_per_ms;
        constexpr std::int64_t unsynced_audio_buffer_ns = 40 * clock::ns_per_ms;
        constexpr std::size_t max_pending_access_units = 90;
        constexpr std::int64_t statistics_interval_ns = 10 * clock::ns_per_second;

        // Sliding-window 95th percentile of observed lateness values.
        class lateness_window
        {
        public:
            void add(std::int64_t now_ns, std::int64_t lateness_ns)
            {
                samples.emplace_back(now_ns, lateness_ns);
                while (!samples.empty() && samples.front().first < now_ns - window_ns)
                {
                    samples.pop_front();
                }
            }

            [[nodiscard]] std::optional<std::int64_t> percentile_95() const
            {
                if (samples.empty())
                {
                    return std::nullopt;
                }
                std::vector<std::int64_t> values;
                values.reserve(samples.size());
                for (auto const &sample : samples)
                {
                    values.push_back(sample.second);
                }
                std::size_t const rank = (values.size() * 95) / 100;
                std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(rank), values.end());
                return values[rank];
            }

            void clear()
            {
                samples.clear();
            }

        private:
            std::deque<std::pair<std::int64_t, std::int64_t>> samples;
        };

        // present_time = media_time + delay; delay rises at once when frames
        // or audio would be late, and decays by 1 ms per 250 ms otherwise.
        class presentation_timeline
        {
        public:
            void configure_margins(std::int64_t video_margin, std::int64_t audio_floor)
            {
                std::scoped_lock lock{mutex};
                video_margin_ns = video_margin;
                audio_floor_ns = audio_floor;
            }

            void observe_video(std::int64_t now_ns, std::int64_t lateness_ns)
            {
                std::scoped_lock lock{mutex};
                video.add(now_ns, lateness_ns);
                update(now_ns);
            }

            void observe_audio(std::int64_t now_ns, std::int64_t lateness_ns)
            {
                std::scoped_lock lock{mutex};
                audio.add(now_ns, lateness_ns);
                update(now_ns);
            }

            [[nodiscard]] std::int64_t delay() const
            {
                return current_delay.load(std::memory_order_relaxed);
            }

            void reset()
            {
                std::scoped_lock lock{mutex};
                video.clear();
                audio.clear();
                current_delay.store(initial_delay_ns, std::memory_order_relaxed);
                last_change_ns = 0;
            }

        private:
            void update(std::int64_t now_ns)
            {
                // Video drives the presentation delay: a mid-stream audio
                // cold-start (its first packets look very "late") must not lurch
                // the whole timeline.  Audio stays in sync because it is
                // scheduled against this same delay and the audio output
                // re-anchors its own queue; a fixed floor keeps buffering
                // headroom for it.
                std::int64_t target = audio_floor_ns;
                if (auto const video_tail = video.percentile_95(); video_tail)
                {
                    target = std::max(target, *video_tail + video_margin_ns);
                }
                target = std::clamp(target, minimum_delay_ns, maximum_delay_ns);
                std::int64_t const delay_now = current_delay.load(std::memory_order_relaxed);
                if (target >= delay_now)
                {
                    current_delay.store(target, std::memory_order_relaxed);
                    last_change_ns = now_ns;
                }
                else if (now_ns - last_change_ns >= decay_interval_ns)
                {
                    // ease a fraction of the gap each interval, so a transient
                    // spike recovers in ~1-2 s instead of minutes
                    std::int64_t const gap = delay_now - target;
                    std::int64_t const step = std::min(gap, std::max(decay_min_step_ns, gap / 8));
                    current_delay.store(delay_now - step, std::memory_order_relaxed);
                    last_change_ns = now_ns;
                }
            }

            static constexpr std::int64_t initial_delay_ns = 50 * clock::ns_per_ms;
            std::mutex mutex;
            lateness_window video;
            lateness_window audio;
            std::int64_t video_margin_ns = 12 * clock::ns_per_ms;
            std::int64_t audio_floor_ns = 20 * clock::ns_per_ms;
            std::int64_t last_change_ns = 0;
            std::atomic<std::int64_t> current_delay{initial_delay_ns};
        };

        struct access_unit
        {
            std::vector<std::uint8_t> bytes;
            std::int64_t media_time_ns = 0;
            std::uint64_t generation = 0;
        };

        struct video_pipeline
        {
            std::mutex mutex;
            std::condition_variable wake;
            std::deque<access_unit> queue;
            std::uint64_t generation = 0;
            std::optional<video_codec> codec;
            bool paused = false;
            bool stopping = false;
            std::thread thread;
            std::uint64_t decoded = 0;
            std::uint64_t failures = 0;
            bool hardware = false;
        };

        struct audio_pipeline
        {
            std::mutex mutex;
            std::unique_ptr<audio_decoder> decoder;
            audio_codec codec = audio_codec::aac_eld;
            int input_rate = 44100;
            int input_channels = 2;
            bool timestamp_sync = true;
            std::int64_t offset_ns = 0;
            bool configured = false;
            std::uint64_t failures = 0;
        };
    }

    struct media_engine::state
    {
        engine_settings settings;
        engine_callbacks callbacks;
        std::unique_ptr<gpu_device> gpu;
        display_mode_info display_mode;
        std::unique_ptr<video_window> window;
        std::unique_ptr<video_presenter> presenter;
        std::unique_ptr<audio_output> audio;
        std::array<presentation_timeline, media_source_count> timelines;
        std::array<video_pipeline, media_source_count> video;
        std::array<audio_pipeline, media_source_count> audio_pipelines;
        std::atomic<int> displayed_source{-1};
        std::atomic<bool> airplay_over_usb{false};
        std::atomic<bool> stopping{false};
        std::thread statistics_thread;
        std::mutex statistics_mutex;
        std::condition_variable statistics_wake;

        void decode_loop(media_source source);
        void on_display_change(std::optional<presented_stream> stream);
        void statistics_loop();
        void rebuild_audio_decoders(audio_device_format format);
    };

    void media_engine::state::on_display_change(std::optional<presented_stream> stream)
    {
        if (!stream)
        {
            displayed_source.store(-1);
            window->set_status(std::format(L"{} — 等待 iPhone 連線", settings.window_title));
            if (!settings.keep_window_on_stop)
            {
                window->hide();
            }
            return;
        }
        displayed_source.store(static_cast<int>(stream->source));
        // AirPlay can arrive over Wi-Fi or over the iPhone USB link; label the
        // transport the client actually used.
        bool const over_usb = stream->source == media_source::usb || airplay_over_usb.load();
        std::wstring const short_label = over_usb ? L"USB" : L"Wi-Fi";
        std::wstring const full_label = stream->source == media_source::usb ? L"USB"
                                        : (airplay_over_usb.load() ? L"USB AirPlay" : L"Wi-Fi AirPlay");
        window->set_status(std::format(L"{} — {} 鏡像中 {}x{}", settings.window_title, short_label, stream->width,
                                       stream->height));
        window->set_title(std::format(L"{} — {} — {}x{}", settings.window_title, full_label, stream->width,
                                      stream->height));
        window->show_for_video(stream->width, stream->height);
        char const *const path = stream->source == media_source::usb ? "USB (QuickTime)"
                                 : (airplay_over_usb.load() ? "AirPlay over USB cable" : "AirPlay over Wi-Fi");
        log::info("displaying {} {}x{}", path, stream->width, stream->height);
    }

    void media_engine::state::decode_loop(media_source source)
    {
        system_tuning::mmcss_scope const mmcss{L"Playback"};
        video_pipeline &pipeline = video[source_index(source)];
        presentation_timeline &timeline = timelines[source_index(source)];
        std::unique_ptr<video_decoder> decoder;
        bool hardware_allowed = true;
        std::uint64_t consecutive_rejections = 0;

        for (;;)
        {
            access_unit unit;
            video_codec codec = video_codec::h264;
            {
                std::unique_lock lock{pipeline.mutex};
                pipeline.wake.wait(lock, [&pipeline]
                {
                    return pipeline.stopping || (!pipeline.queue.empty() && !pipeline.paused);
                });
                if (pipeline.stopping)
                {
                    return;
                }
                unit = std::move(pipeline.queue.front());
                pipeline.queue.pop_front();
                if (unit.generation != pipeline.generation || !pipeline.codec)
                {
                    continue;
                }
                codec = *pipeline.codec;
            }

            if (!decoder || decoder->codec() != codec)
            {
                decoder.reset();
                auto created = video_decoder::create(*gpu, codec, hardware_allowed);
                if (!created)
                {
                    log::error("{} video decoder: {}", media_source_name(source), created.error().describe());
                    continue;
                }
                decoder = std::move(*created);
                consecutive_rejections = 0;
                std::scoped_lock lock{pipeline.mutex};
                pipeline.hardware = decoder->hardware();
            }

            auto frames = decoder->decode(unit.bytes, unit.media_time_ns);
            if (!frames)
            {
                std::scoped_lock lock{pipeline.mutex};
                if (++pipeline.failures <= 5)
                {
                    log::warning("{} video decode: {}", media_source_name(source), frames.error().describe());
                }
                continue;
            }
            if (frames->empty())
            {
                // hardware decoders that keep rejecting a valid stream (e.g.
                // unsupported profile) are replaced by the software decoder
                if (decoder->hardware() && ++consecutive_rejections == 120)
                {
                    log::warning("{} hardware decoder produced no pictures, switching to software decoding",
                                 media_source_name(source));
                    hardware_allowed = false;
                    decoder.reset();
                }
                continue;
            }
            consecutive_rejections = 0;
            std::int64_t const ready_ns = clock::now_ns();
            for (timed_frame &decoded : *frames)
            {
                std::int64_t present_ns = ready_ns;
                if (settings.timestamp_sync)
                {
                    timeline.observe_video(ready_ns, ready_ns - decoded.pts_ns);
                    present_ns = decoded.pts_ns + timeline.delay();
                    audio->set_delay(source, timeline.delay()); // the sound of this source uses the same delay
                }
                presenter->submit(source, std::move(decoded.frame), present_ns);
            }
            std::scoped_lock lock{pipeline.mutex};
            pipeline.decoded += frames->size();
            pipeline.hardware = decoder->hardware(); // frame pool exists once pictures are produced
        }
    }

    void media_engine::state::rebuild_audio_decoders(audio_device_format format)
    {
        for (media_source const source : sources_by_priority)
        {
            audio_pipeline &pipeline = audio_pipelines[source_index(source)];
            std::scoped_lock lock{pipeline.mutex};
            if (!pipeline.configured)
            {
                continue;
            }
            auto created = audio_decoder::create(pipeline.codec, pipeline.input_rate, pipeline.input_channels,
                                                 format.sample_rate);
            if (!created)
            {
                log::error("{} audio decoder rebuild: {}", media_source_name(source), created.error().describe());
                pipeline.decoder.reset();
                continue;
            }
            pipeline.decoder = std::move(*created);
        }
    }

    void media_engine::state::statistics_loop()
    {
        std::unique_lock lock{statistics_mutex};
        while (!statistics_wake.wait_for(lock, std::chrono::nanoseconds{statistics_interval_ns}, [this]
        {
            return stopping.load();
        }))
        {
            int const shown = displayed_source.load();
            if (shown < 0)
            {
                continue;
            }
            auto const source = static_cast<media_source>(shown);
            presenter_statistics const stats = presenter->statistics();
            log::info("{}: presented {} dropped {} | sync delay {:.1f} ms | audio device latency {:.1f} ms",
                      media_source_name(source), stats.presented, stats.dropped,
                      static_cast<double>(timelines[source_index(source)].delay()) / 1e6,
                      static_cast<double>(audio->device_latency_ns()) / 1e6);
        }
    }

    media_engine::media_engine(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    media_engine::~media_engine()
    {
        impl->stopping.store(true);
        impl->statistics_wake.notify_all();
        if (impl->statistics_thread.joinable())
        {
            impl->statistics_thread.join();
        }
        for (video_pipeline &pipeline : impl->video)
        {
            {
                std::scoped_lock lock{pipeline.mutex};
                pipeline.stopping = true;
            }
            pipeline.wake.notify_all();
            if (pipeline.thread.joinable())
            {
                pipeline.thread.join();
            }
        }
        impl->audio.reset();
        impl->presenter.reset();
        impl->window.reset();
        impl->gpu.reset();
    }

    outcome<std::unique_ptr<media_engine>> media_engine::create(engine_settings settings, engine_callbacks callbacks)
    {
        auto created = std::make_unique<state>();
        created->settings = std::move(settings);
        created->callbacks = std::move(callbacks);
        state *const engine = created.get();

        auto gpu = gpu_device::create(created->settings.gpu);
        if (!gpu)
        {
            return std::unexpected(gpu.error());
        }
        created->gpu = std::move(*gpu);
        if (auto mode = gpu_device::primary_display_mode(); mode)
        {
            created->display_mode = *mode;
            log::info("display: {}x{}, composition clock {:.3f} Hz", mode->width, mode->height, mode->refresh_hz());
            for (display_target_info const &target : mode->targets)
            {
                log::info("  shown on {} ({}) at {:.3f} Hz", target.monitor_name,
                          target.internal_panel ? "built-in" : "external", target.refresh_hz);
            }
            if (mode->targets.size() > 1)
            {
                double const lowest = std::ranges::min(mode->targets, {}, &display_target_info::refresh_hz).refresh_hz;
                if (lowest + 0.5 < mode->refresh_hz())
                {
                    log::warning("duplicate display mode: the desktop is composed at {:.2f} Hz but a monitor scans "
                                 "out at {:.2f} Hz, so that monitor drops frames; showing the desktop only on the "
                                 "external monitor makes motion smoother",
                                 mode->refresh_hz(), lowest);
                }
            }
        }
        else
        {
            log::warning("display mode query: {}", mode.error().describe());
        }

        window_callbacks window_events;
        window_events.on_client_resized = [engine](int width, int height)
        {
            if (engine->presenter)
            {
                engine->presenter->resize(width, height);
            }
        };
        window_events.on_close_requested = [engine]
        {
            int const shown = engine->displayed_source.load();
            engine->window->hide();
            if (shown >= 0 && engine->callbacks.on_window_closed)
            {
                engine->callbacks.on_window_closed(static_cast<media_source>(shown));
            }
        };
        window_events.on_usb_device_change = [engine]
        {
            if (engine->callbacks.on_usb_device_change)
            {
                engine->callbacks.on_usb_device_change();
            }
        };
        window_events.on_tray_command = [engine](tray_command command)
        {
            if (engine->callbacks.on_tray_command)
            {
                engine->callbacks.on_tray_command(command);
            }
        };
        window_events.on_display_changed = [engine]
        {
            // a monitor was attached/detached or changed resolution/refresh:
            // re-pace presentation to the new composition clock (keeps motion
            // smooth without a restart)
            auto mode = gpu_device::primary_display_mode();
            if (mode && engine->presenter)
            {
                engine->presenter->set_refresh_period(mode->refresh_period_ns());
                log::info("display changed: re-paced to {}x{}, composition clock {:.3f} Hz", mode->width, mode->height,
                          mode->refresh_hz());
            }
            else
            {
                log::warning("display changed: re-query {}", mode ? "ok but presenter not ready" : "failed");
            }
        };
        auto window = video_window::create(created->settings.window_title, std::move(window_events),
                                           created->settings.start_fullscreen, created->settings.tray_icon);
        if (!window)
        {
            return std::unexpected(window.error());
        }
        created->window = std::move(*window);
        created->window->set_status(std::format(L"{} — 等待 iPhone 連線", created->settings.window_title));

        auto presenter = video_presenter::create(*created->gpu, created->window->native_handle(),
                                                 created->display_mode.refresh_period_ns(),
                                                 [engine](std::optional<presented_stream> stream)
                                                 {
                                                     engine->on_display_change(stream);
                                                 });
        if (!presenter)
        {
            return std::unexpected(presenter.error());
        }
        created->presenter = std::move(*presenter);

        auto audio = audio_output::create([engine](audio_device_format format)
        {
            engine->rebuild_audio_decoders(format);
        });
        if (!audio)
        {
            return std::unexpected(audio.error());
        }
        created->audio = std::move(*audio);

        std::int64_t const refresh = created->presenter->refresh_period_ns();
        std::int64_t const audio_latency = created->audio->device_latency_ns();
        for (presentation_timeline &timeline : created->timelines)
        {
            // video drives the delay (half a refresh of decode headroom); audio
            // only sets a small buffering floor so its cold-start cannot lurch it
            timeline.configure_margins(refresh / 2 + 4 * clock::ns_per_ms, audio_latency + 12 * clock::ns_per_ms);
        }
        for (media_source const source : sources_by_priority)
        {
            created->video[source_index(source)].thread = std::thread{[engine, source]
            {
                engine->decode_loop(source);
            }};
        }
        created->statistics_thread = std::thread{[engine]
        {
            engine->statistics_loop();
        }};
        return std::unique_ptr<media_engine>{new media_engine{std::move(created)}};
    }

    display_summary media_engine::display() const
    {
        return display_summary{impl->display_mode.width, impl->display_mode.height, impl->display_mode.refresh_hz()};
    }

    bool media_engine::hevc_hardware() const
    {
        return impl->gpu && impl->gpu->adapter_info().hevc_main_decode;
    }

    void media_engine::video_configure(media_source source, video_codec codec, int width, int height)
    {
        video_pipeline &pipeline = impl->video[source_index(source)];
        {
            std::scoped_lock lock{pipeline.mutex};
            if (pipeline.codec != codec)
            {
                ++pipeline.generation;
                pipeline.queue.clear();
            }
            pipeline.codec = codec;
            pipeline.paused = false;
        }
        pipeline.wake.notify_all();
        log::info("{} video: {} {}x{}", media_source_name(source), codec == video_codec::h264 ? "H.264" : "H.265",
                  width, height);
    }

    void media_engine::video_access_unit(media_source source, std::span<std::uint8_t const> access_unit_bytes,
                                         std::int64_t media_time_ns, std::int64_t arrival_time_ns)
    {
        (void) arrival_time_ns;
        video_pipeline &pipeline = impl->video[source_index(source)];
        {
            std::scoped_lock lock{pipeline.mutex};
            if (!pipeline.codec)
            {
                return;
            }
            if (pipeline.queue.size() >= max_pending_access_units)
            {
                // decoder cannot keep up: restart from the next key frame
                pipeline.queue.clear();
                log::warning("{} video backlog cleared", media_source_name(source));
            }
            pipeline.queue.push_back(access_unit{std::vector<std::uint8_t>(access_unit_bytes.begin(), access_unit_bytes.end()),
                                                 media_time_ns, pipeline.generation});
        }
        pipeline.wake.notify_one();
    }

    void media_engine::video_pause(media_source source)
    {
        video_pipeline &pipeline = impl->video[source_index(source)];
        std::scoped_lock lock{pipeline.mutex};
        pipeline.paused = true;
    }

    void media_engine::video_resume(media_source source)
    {
        video_pipeline &pipeline = impl->video[source_index(source)];
        {
            std::scoped_lock lock{pipeline.mutex};
            pipeline.paused = false;
        }
        pipeline.wake.notify_all();
    }

    void media_engine::video_stop(media_source source)
    {
        video_pipeline &pipeline = impl->video[source_index(source)];
        {
            std::scoped_lock lock{pipeline.mutex};
            ++pipeline.generation;
            pipeline.queue.clear();
            pipeline.codec.reset();
            pipeline.paused = false;
        }
        impl->presenter->release_source(source, impl->settings.keep_window_on_stop);
        impl->timelines[source_index(source)].reset();
        log::info("{} video stopped", media_source_name(source));
    }

    void media_engine::audio_configure(media_source source, audio_codec codec, int sample_rate, int channels,
                                       bool timestamp_sync, double audio_offset_ms)
    {
        audio_pipeline &pipeline = impl->audio_pipelines[source_index(source)];
        std::scoped_lock lock{pipeline.mutex};
        pipeline.codec = codec;
        pipeline.input_rate = sample_rate;
        pipeline.input_channels = channels;
        pipeline.timestamp_sync = timestamp_sync && impl->settings.timestamp_sync;
        pipeline.offset_ns = static_cast<std::int64_t>(audio_offset_ms * 1e6);
        pipeline.configured = true;
        auto created = audio_decoder::create(codec, sample_rate, channels, impl->audio->format().sample_rate);
        if (!created)
        {
            log::error("{} audio decoder: {}", media_source_name(source), created.error().describe());
            pipeline.decoder.reset();
            return;
        }
        pipeline.decoder = std::move(*created);
        log::info("{} audio: codec {} {} Hz {} ch, {}", media_source_name(source), static_cast<int>(codec), sample_rate,
                  channels, pipeline.timestamp_sync ? "timestamp-synchronized" : "play on arrival");
    }

    void media_engine::audio_packet(media_source source, std::span<std::uint8_t const> packet,
                                    std::int64_t media_time_ns, std::int64_t arrival_time_ns)
    {
        audio_pipeline &pipeline = impl->audio_pipelines[source_index(source)];
        std::scoped_lock lock{pipeline.mutex};
        if (!pipeline.decoder)
        {
            return;
        }
        auto decoded = pipeline.decoder->decode(packet);
        if (!decoded)
        {
            if (++pipeline.failures <= 5)
            {
                log::warning("{} audio decode: {}", media_source_name(source), decoded.error().describe());
            }
            return;
        }
        if (decoded->stereo_samples.empty())
        {
            return;
        }
        if (pipeline.timestamp_sync)
        {
            presentation_timeline &timeline = impl->timelines[source_index(source)];
            timeline.observe_audio(arrival_time_ns, arrival_time_ns - media_time_ns);
            impl->audio->set_delay(source, timeline.delay());
            impl->audio->enqueue(source, media_time_ns + pipeline.offset_ns - decoded->resampler_delay_ns,
                                 decoded->stereo_samples, pipeline.decoder->output_rate(), true);
        }
        else
        {
            impl->audio->set_delay(source, 0);
            impl->audio->enqueue(source, arrival_time_ns + unsynced_audio_buffer_ns - decoded->resampler_delay_ns,
                                 decoded->stereo_samples, pipeline.decoder->output_rate(), false);
        }
    }

    void media_engine::audio_stop(media_source source)
    {
        audio_pipeline &pipeline = impl->audio_pipelines[source_index(source)];
        {
            std::scoped_lock lock{pipeline.mutex};
            pipeline.decoder.reset();
            pipeline.configured = false;
        }
        impl->audio->release_source(source);
    }

    void media_engine::set_volume(media_source source, double linear_gain)
    {
        impl->audio->set_volume(source, static_cast<float>(linear_gain));
    }

    void media_engine::set_usb_enabled(bool enabled)
    {
        impl->window->set_usb_enabled(enabled);
    }

    void media_engine::set_airplay_over_usb(bool over_usb)
    {
        impl->airplay_over_usb.store(over_usb, std::memory_order_relaxed);
    }

    void media_engine::notify(std::wstring title, std::wstring text)
    {
        impl->window->notify(std::move(title), std::move(text));
    }

    source_statistics media_engine::statistics(media_source source) const
    {
        source_statistics result;
        presenter_statistics const presented = impl->presenter->statistics();
        result.presented = presented.presented;
        result.dropped = presented.dropped;
        result.sync_delay_ns = impl->timelines[source_index(source)].delay();
        video_pipeline &pipeline = impl->video[source_index(source)];
        std::scoped_lock lock{pipeline.mutex};
        result.decoded = pipeline.decoded;
        result.hardware_decode = pipeline.hardware;
        return result;
    }
}
