// SPDX-License-Identifier: MIT
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
import uxplay_ui_text;
import uxplay_crop_detector;
import uxplay_remote_control;

namespace uxplay
{
    namespace
    {
        constexpr std::int64_t window_ns = 2 * clock::ns_per_second;
        constexpr std::int64_t minimum_delay_ns = 8 * clock::ns_per_ms;
        constexpr std::int64_t maximum_delay_ns = 220 * clock::ns_per_ms;
        constexpr std::int64_t decay_interval_ns = 100 * clock::ns_per_ms;
        constexpr std::int64_t decay_min_step_ns = clock::ns_per_ms;
        // play-on-arrival audio is scheduled this long after it arrived, and
        // never closer than what the endpoint keeps queued plus this margin
        constexpr std::int64_t unsynced_audio_buffer_ns = 40 * clock::ns_per_ms;
        constexpr std::int64_t unsynced_audio_margin_ns = 12 * clock::ns_per_ms;
        constexpr std::size_t max_pending_access_units = 90;
        constexpr std::int64_t statistics_interval_ns = 10 * clock::ns_per_second;
        // automatic crop looks at one decoded picture this often, and more
        // often while a crop is in force: content that appears in a cut-off
        // bar must be noticed quickly
        constexpr std::int64_t crop_examination_interval_ns = 200 * clock::ns_per_ms;
        constexpr std::int64_t cropped_examination_interval_ns = 100 * clock::ns_per_ms;
        // Upper limit of the delay, and of the time a decoded picture waits
        // for its presentation time: the presenter queues 13 pictures in
        // quality mode and 8 otherwise, i.e. that many frame intervals of a
        // 60 fps stream.  Waiting longer would only make it evict pictures
        // unseen: the picture would freeze while sound goes on.
        //
        // What a picture normally waits is the delay plus the lead of the
        // sender's timestamps: an iPhone stamps its pictures about 85 ms ahead
        // of the moment they arrive (measured: 75 to 95 ms), so they wait
        // about 100 ms at the resting delay.  The limit must stay above that,
        // or pictures would be shown by arrival time instead of by timestamp.
        constexpr std::int64_t standard_delay_limit_ns = 135 * clock::ns_per_ms;
        // a part framed by hand: not smaller than this (pixels of the picture),
        // and a frame around (nearly) everything shown changes nothing
        constexpr int smallest_crop_edge = 32;
        constexpr double whole_selection_fraction = 0.98;
        // remote control: one notch of the wheel scrolls this share of what is shown
        constexpr double wheel_notch_fraction = 0.07;
        constexpr double touch_full_scale = 65535.0;
        // a flick from the keyboard travels this share of what is shown, and
        // stays this far from the edges of the screen
        constexpr double swipe_travel_fraction = 0.45;
        constexpr double swipe_edge_fraction = 0.04;
        // so often in a row a control session is started again by itself
        // after the phone went away in mid-use (then the user is told instead)
        constexpr int longest_reconnect_streak = 3;

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
            void configure_margins(std::int64_t video_margin, std::int64_t audio_floor, std::int64_t delay_limit)
            {
                std::scoped_lock lock{mutex};
                video_margin_ns = video_margin;
                audio_floor_ns = audio_floor;
                delay_limit_ns = std::max({delay_limit, audio_floor, minimum_delay_ns});
                if (!observed)
                {
                    current_delay.store(start_delay(), std::memory_order_relaxed);
                }
            }

            void observe_video(std::int64_t now_ns, std::int64_t lateness_ns)
            {
                std::scoped_lock lock{mutex};
                observed = true;
                video.add(now_ns, lateness_ns);
                update(now_ns);
            }

            void observe_audio(std::int64_t now_ns, std::int64_t lateness_ns)
            {
                std::scoped_lock lock{mutex};
                observed = true;
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
                observed = false;
                current_delay.store(start_delay(), std::memory_order_relaxed);
                last_change_ns = 0;
            }

        private:
            // The delay a stream starts with (mutex held): its resting value.
            // Pictures that come late raise it at once (update); starting
            // higher only made the first pictures wait longer than the
            // presenter queue is deep, and easing down afterwards is faster
            // than the audio output can follow without a skip.
            [[nodiscard]] std::int64_t start_delay() const
            {
                return std::clamp(std::max(audio_floor_ns, video_margin_ns), minimum_delay_ns, delay_limit_ns);
            }

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
                target = std::clamp(target, minimum_delay_ns, delay_limit_ns);
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
            std::int64_t delay_limit_ns = maximum_delay_ns;
            bool observed = false; // a picture or a packet arrived since the last reset
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
            // the session ended: the decode thread frees its decoder (and with
            // it the GPU surface pool) instead of keeping it while idle
            bool release_decoder = false;
            // access units received since the last statistics line
            std::uint64_t received = 0;
            // (time a picture was decoded - its timestamp) since the last
            // statistics line: negative when pictures are ready early
            std::int64_t lateness_minimum_ns = std::numeric_limits<std::int64_t>::max();
            std::int64_t lateness_maximum_ns = std::numeric_limits<std::int64_t>::min();
            std::int64_t lateness_sum_ns = 0;
            std::uint64_t lateness_count = 0;
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
            // flow since the last statistics line
            std::uint64_t packets = 0;
            std::uint64_t empty_outputs = 0; // decoder accepted the packet but produced nothing
            std::uint64_t decoded_frames = 0;
            float peak = 0.0F; // largest |sample| decoded
            // (time a packet arrived - its timestamp) since the last
            // statistics line: negative when the sound arrives early
            std::int64_t lateness_minimum_ns = std::numeric_limits<std::int64_t>::max();
            std::int64_t lateness_maximum_ns = std::numeric_limits<std::int64_t>::min();
            std::int64_t lateness_sum_ns = 0;
            std::uint64_t lateness_count = 0;
        };

        // Automatic crop, per decode thread: the CPU copy of a picture's
        // luma plane and what the detector learned from the stream.
        struct crop_inspection
        {
            explicit crop_inspection(gpu_device &device)
                : luma{device}
            {
            }

            luma_reader luma;
            crop_detector detector;
            std::uint64_t generation = 0; // crop_generation the detector state belongs to (0: none)
            std::int64_t next_examination_ns = 0;
            bool reading = false; // the reader holds its staging copy
            std::uint64_t failures = 0;
            int disagreements = 0;   // examinations in a row at which the presenter showed something else
            int reconciliations = 0; // times in a row the presenter was told again
            int picture_width = 0;   // of the picture examined last
            int picture_height = 0;
        };

        std::string level_text(float peak)
        {
            if (peak <= 0.0F)
            {
                return "all zero";
            }
            return std::format("{:.1f} dBFS", 20.0 * std::log10(static_cast<double>(peak)));
        }
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
        std::atomic<bool> muted{false};
        std::atomic<bool> stopping{false};
        std::atomic<performance_mode> mode{performance_mode::balanced};
        // crop: which part of the picture is shown.  base_crop is the user's
        // standing choice (off or automatic); crop is what is in force (also
        // manual, until the stream changes).
        std::atomic<crop_mode> base_crop{crop_mode::off};
        std::atomic<crop_mode> crop{crop_mode::off};
        // counts the user's choices: decode threads judge afresh after each
        std::atomic<std::uint64_t> crop_generation{1};
        // a change of the shown region and the mode it belongs to are one step
        // (the user on the UI thread, the detector on a decode thread)
        std::mutex crop_mutex;
        // what the presenter shows now (written on the render thread)
        std::mutex shown_mutex;
        std::optional<presented_stream> shown_stream;
        // the stream a hand-framed part was drawn on (source and picture
        // size; guarded by crop_mutex)
        presented_stream manual_stream;
        // a video session runs for the source (video_configure .. video_stop);
        // cleared under crop_mutex, so no crop is set for a session that ended
        std::array<std::atomic<bool>, media_source_count> session_live{};
        // the audio device latency the timing margins were computed from
        std::atomic<std::int64_t> applied_audio_latency{-1};
        // one mode is applied at a time (UI thread, statistics thread)
        std::mutex mode_mutex;
        // No picture waits longer than this for its presentation time (the
        // mode's delay limit): one that is ready earlier than expected would
        // otherwise sit in the presenter's queue beyond its depth.
        std::atomic<std::int64_t> presentation_wait_limit_ns{maximum_delay_ns};
        std::thread statistics_thread;
        std::mutex statistics_mutex;
        std::condition_variable statistics_wake;

        // Remote control of the phone.  A control session with it
        // (uxplay_remote_control) runs while the user wants control and a
        // stream is shown; the control thread starts and ends it, because
        // both take a moment (longer while the phone does not answer).
        std::atomic<bool> control_wanted{false};
        std::atomic<bool> control_ready{false}; // input reaches the phone
        std::atomic<bool> control_connecting{false}; // the session is getting ready (shown in the title)
        std::mutex control_mutex;               // guards the members below
        std::condition_variable control_wake;
        std::unique_ptr<remote_control> control;
        bool control_check = false;   // something changed: the control thread looks again
        bool control_retry = false;   // a reason to try again after a failure
        bool control_gave_up = false; // the last attempt failed: wait for such a reason
        bool control_ended = false;   // the session ended by itself: its object is let go
        bool control_exit = false;
        remote_control_problem control_problem = remote_control_problem::none; // of the last failure
        bool control_hint_shown = false; // "you can control the iPhone now" was shown in this run
        int control_reconnects = 0;      // restarts in a row after the phone went away
        std::thread control_thread;

        void control_loop();
        void control_changed(bool retry);
        void control_status(remote_control_status const &reported);
        void control_failed(remote_control_problem problem, std::string_view detail);
        [[nodiscard]] std::optional<touch_point> control_point(double x, double y, bool touching);
        void control_pointer(pointer_action action, double x, double y);
        void control_wheel(double x, double y, double horizontal, double vertical);
        void control_swipe(swipe_action action, double x, double y);
        void label_stream(presented_stream const &stream);
        void relabel_shown_stream();

        void decode_loop(media_source source);
        void on_display_change(std::optional<presented_stream> stream);
        void statistics_loop();
        void rebuild_audio_decoders(audio_device_format format);
        void apply_performance_mode();
        [[nodiscard]] std::optional<picture_region> inspect_for_crop(media_source source,
                                                                     decoded_frame const &picture,
                                                                     crop_inspection &inspection);
        void apply_detected_crop(media_source source, picture_region region, std::uint64_t generation,
                                 int picture_width, int picture_height);
        void choose_crop_mode(crop_mode selected);
        void apply_crop_selection(fraction_region selection);
    };

    // Status line, window title and control-bar text for the stream that is
    // shown: the link, the size of the part shown, and whether the phone can
    // be controlled.  (Render thread, control thread and the control session's thread.)
    void media_engine::state::label_stream(presented_stream const &stream)
    {
        // AirPlay can arrive over Wi-Fi or over the iPhone USB link; label the
        // transport the client actually used.
        std::wstring const label = airplay_over_usb.load() ? L"USB" : L"Wi-Fi";
        picture_region const visible =
            stream.visible.empty() ? picture_region{0, 0, stream.width, stream.height} : stream.visible;
        bool const cropped = visible.width != stream.width || visible.height != stream.height;
        std::wstring size = std::format(L"{} × {}", visible.width, visible.height);
        auto const decorate = [&size](ui_text pattern)
        {
            std::wstring_view const inner{size};
            size = ui_format(pattern, std::span<std::wstring_view const>{&inner, 1});
        };
        if (cropped)
        {
            decorate(ui_text::cropped);
        }
        if (control_ready.load())
        {
            decorate(ui_text::controlled);
        }
        else if (control_connecting.load())
        {
            size = std::format(L"{} · {}", size, ui_string(ui_text::control_connecting));
        }
        window->set_status(std::format(L"{} · {} · {}", settings.window_title, label, size));
        window->set_title(std::format(L"{} · {} · {}", settings.window_title, label, size));
        window->set_stream_info(label, size);
    }

    // The labels again, for the stream that is on screen (control began or ended).
    void media_engine::state::relabel_shown_stream()
    {
        std::optional<presented_stream> current;
        {
            std::scoped_lock lock{shown_mutex};
            current = shown_stream;
        }
        if (current && displayed_source.load() >= 0)
        {
            label_stream(*current);
        }
    }

    // Something the control thread decides on has changed.  `retry`: there is
    // a reason to try again after a failure (the user chose it again, a stream
    // appeared, a device was plugged in).
    void media_engine::state::control_changed(bool retry)
    {
        {
            std::scoped_lock lock{control_mutex};
            control_check = true;
            control_retry = control_retry || retry;
            if (retry)
            {
                control_reconnects = 0; // a new reason to try: the streak starts over
            }
        }
        control_wake.notify_one();
    }

    // Control cannot be had: the user is told why, the log gets the detail.
    void media_engine::state::control_failed(remote_control_problem problem, std::string_view detail)
    {
        control_ready.store(false);
        control_connecting.store(false);
        window->set_remote_control(control_wanted.load(), false);
        log::warning("remote control: {} ({})", remote_control_problem_name(problem), detail);
        ui_text reason = ui_text::control_failed_text;
        switch (problem)
        {
        case remote_control_problem::no_device:
        case remote_control_problem::not_trusted:
            reason = ui_text::control_needs_cable;
            break;
        case remote_control_problem::developer_mode_off:
            reason = ui_text::control_needs_developer_mode;
            break;
        case remote_control_problem::image_mount_failed:
            reason = ui_text::control_image_failed;
            break;
        case remote_control_problem::media_in_use:
            reason = ui_text::control_camera_in_use;
            break;
        case remote_control_problem::unsupported_ios:
            reason = ui_text::control_needs_ios;
            break;
        case remote_control_problem::none:
        case remote_control_problem::tunnel_failed:
        case remote_control_problem::stream_failed:
        case remote_control_problem::connection_lost:
        case remote_control_problem::internal:
            break;
        }
        window->notify(std::wstring{ui_string(ui_text::control_failed_title)}, std::wstring{ui_string(reason)});
        relabel_shown_stream();
    }

    // What the control session says about itself (on its own thread).  Must
    // not end the session: the control thread does.
    void media_engine::state::control_status(remote_control_status const &reported)
    {
        switch (reported.state)
        {
        case remote_control_state::starting:
            log::debug("remote control: {}", reported.detail);
            return;
        case remote_control_state::preparing:
            // takes up to a minute: the user is told what the wait is for
            log::info("remote control: {}", reported.detail);
            window->notify(std::wstring{ui_string(ui_text::control_preparing_title)},
                           std::wstring{ui_string(ui_text::control_preparing_text)});
            return;
        case remote_control_state::ready:
        {
            control_ready.store(true);
            control_connecting.store(false);
            window->set_remote_control(control_wanted.load(), true);
            log::info("remote control: ready ({})", reported.detail);
            bool first_time = false;
            {
                std::scoped_lock lock{control_mutex};
                first_time = !std::exchange(control_hint_shown, true);
                control_problem = remote_control_problem::none;
            }
            if (first_time)
            {
                window->notify(std::wstring{ui_string(ui_text::control_ready_title)},
                               std::wstring{ui_string(ui_text::control_ready_text)});
            }
            relabel_shown_stream();
            return;
        }
        case remote_control_state::failed:
            break;
        }
        // the session is over (not because this program ended it: then it is silent)
        remote_control_problem const problem = reported.problem;
        bool again = false;
        {
            std::scoped_lock lock{control_mutex};
            control_ended = true;
            control_check = true;
            control_problem = problem;
            // The phone went away in mid-use (the cable has a loose contact,
            // the phone restarted its services): a new session is started at
            // once.  Not for ever; with the cable pulled the new session ends
            // with "no iPhone on a cable", which the user is told.
            if (problem == remote_control_problem::connection_lost && control_reconnects < longest_reconnect_streak)
            {
                ++control_reconnects;
                control_retry = true;
                again = true;
            }
            else
            {
                control_gave_up = true;
            }
        }
        control_wake.notify_one();
        if (!again)
        {
            control_failed(problem, reported.detail);
            return;
        }
        control_ready.store(false);
        control_connecting.store(true);
        window->set_remote_control(control_wanted.load(), false);
        log::info("remote control: the connection to the phone was lost ({}); connecting again", reported.detail);
        relabel_shown_stream();
    }

    // Starts a control session when control is wanted and a stream is shown,
    // ends it when either is no longer so, and lets go of one that ended by
    // itself.
    void media_engine::state::control_loop()
    {
        std::unique_lock lock{control_mutex};
        for (;;)
        {
            control_wake.wait(lock, [this] { return control_exit || control_check; });
            if (control_exit)
            {
                break;
            }
            control_check = false;
            bool const retry = std::exchange(control_retry, false);
            if (std::exchange(control_ended, false) && control)
            {
                std::unique_ptr<remote_control> ended = std::move(control);
                lock.unlock();
                ended.reset();
                lock.lock();
            }
            if (!control_wanted.load() || displayed_source.load() < 0)
            {
                control_gave_up = false;
                if (control)
                {
                    std::unique_ptr<remote_control> ending = std::move(control);
                    lock.unlock();
                    ending.reset(); // waits until the session has ended
                    log::info("remote control: ended");
                    lock.lock();
                }
                bool const was_connecting = control_connecting.exchange(false);
                if (control_ready.exchange(false) || was_connecting)
                {
                    lock.unlock();
                    window->set_remote_control(control_wanted.load(), false);
                    relabel_shown_stream();
                    lock.lock();
                }
                continue;
            }
            if (control || (control_gave_up && !retry))
            {
                continue;
            }
            control_gave_up = false;
            lock.unlock();
            control_connecting.store(true);
            relabel_shown_stream();
            log::info("remote control: connecting to the phone");
            auto started = remote_control::start(
                [this](remote_control_status const &reported) { control_status(reported); }, settings.control_cache);
            if (!started)
            {
                std::string const cause = started.error().describe();
                control_failed(remote_control_problem::internal, cause);
                lock.lock();
                control_gave_up = true;
                control_problem = remote_control_problem::internal;
                continue;
            }
            lock.lock();
            control = std::move(*started);
            control_check = true; // what is wanted may have changed meanwhile
        }
        std::unique_ptr<remote_control> ending = std::move(control);
        lock.unlock();
        ending.reset();
    }

    // A position on the picture as it is shown (fractions) as a position on
    // the phone's screen: the shown part may be a crop of the picture.
    std::optional<touch_point> media_engine::state::control_point(double x, double y, bool touching)
    {
        std::optional<presented_stream> current;
        {
            std::scoped_lock lock{shown_mutex};
            current = shown_stream;
        }
        if (!current || current->width <= 0 || current->height <= 0)
        {
            return std::nullopt;
        }
        picture_region const visible =
            current->visible.empty() ? picture_region{0, 0, current->width, current->height} : current->visible;
        auto const scaled = [](double position, int extent)
        {
            return static_cast<std::uint16_t>(
                std::clamp(std::lround(position / extent * touch_full_scale), 0L, 65535L));
        };
        touch_point point;
        point.identity = 0;
        point.x = scaled(visible.left + x * visible.width, current->width);
        point.y = scaled(visible.top + y * visible.height, current->height);
        point.touching = touching;
        return point;
    }

    void media_engine::state::control_pointer(pointer_action action, double x, double y)
    {
        std::optional<touch_point> const point = control_point(x, y, action != pointer_action::release);
        if (!point)
        {
            return;
        }
        std::scoped_lock lock{control_mutex};
        if (control)
        {
            control->touch(std::span<touch_point const>{&*point, 1});
        }
    }

    // The wheel: what is under the pointer is dragged along, like under a
    // finger (the wheel turned away from the user shows what is further up).
    void media_engine::state::control_wheel(double x, double y, double horizontal, double vertical)
    {
        std::optional<touch_point> const point = control_point(x, y, true);
        std::optional<presented_stream> current;
        {
            std::scoped_lock lock{shown_mutex};
            current = shown_stream;
        }
        if (!point || !current)
        {
            return;
        }
        picture_region const visible =
            current->visible.empty() ? picture_region{0, 0, current->width, current->height} : current->visible;
        auto const distance = [](double notches, int shown, int whole)
        {
            return static_cast<int>(std::lround(notches * wheel_notch_fraction * shown / whole * touch_full_scale));
        };
        int const delta_x = distance(-horizontal, visible.width, current->width);
        int const delta_y = distance(vertical, visible.height, current->height);
        std::scoped_lock lock{control_mutex};
        if (control)
        {
            control->scroll(point->x, point->y, delta_x, delta_y);
        }
    }

    // A flick from the keyboard: the finger travels through (x, y), half of
    // the way before it and half after.  "Previous to the left" keeps its
    // start when that is at the left edge: from there it is the phone's
    // "back" gesture.
    void media_engine::state::control_swipe(swipe_action action, double x, double y)
    {
        double direction_x = 0.0;
        double direction_y = 0.0;
        switch (action)
        {
        case swipe_action::next_below:
            direction_y = -1.0;
            break;
        case swipe_action::previous_above:
            direction_y = 1.0;
            break;
        case swipe_action::next_right:
            direction_x = -1.0;
            break;
        case swipe_action::previous_left:
            direction_x = 1.0;
            break;
        }
        double const low = swipe_edge_fraction;
        double const high = 1.0 - swipe_edge_fraction;
        bool const from_left_edge = action == swipe_action::previous_left && x <= swipe_edge_fraction;
        double start_x = from_left_edge ? 0.0 : std::clamp(x - direction_x * swipe_travel_fraction / 2, low, high);
        double const start_y = std::clamp(y - direction_y * swipe_travel_fraction / 2, low, high);
        double const end_x = std::clamp(start_x + direction_x * swipe_travel_fraction, from_left_edge ? 0.0 : low, high);
        double const end_y = std::clamp(start_y + direction_y * swipe_travel_fraction, low, high);
        std::optional<touch_point> const from = control_point(start_x, start_y, true);
        std::optional<touch_point> const to = control_point(end_x, end_y, true);
        if (!from || !to)
        {
            return;
        }
        std::scoped_lock lock{control_mutex};
        if (control)
        {
            control->swipe(from->x, from->y, static_cast<int>(to->x) - static_cast<int>(from->x),
                           static_cast<int>(to->y) - static_cast<int>(from->y));
        }
    }

    // Timing margins per mode (the presenter applies its own part: frame
    // dropping, queue depth and scaling).  The delay is
    // max(audio floor, video p95 lateness + video margin).
    void media_engine::state::apply_performance_mode()
    {
        std::scoped_lock mode_lock{mode_mutex};
        performance_mode const selected = mode.load();
        std::int64_t const refresh = presenter->refresh_period_ns();
        std::int64_t const audio_latency = audio->device_latency_ns();
        applied_audio_latency.store(audio_latency);
        std::int64_t video_margin = refresh / 2 + 4 * clock::ns_per_ms;
        std::int64_t audio_floor = audio_latency + 12 * clock::ns_per_ms;
        std::int64_t delay_limit = standard_delay_limit_ns;
        switch (selected)
        {
        case performance_mode::quality:
            // a full refresh of headroom: the delay almost never has to move
            video_margin = refresh + 4 * clock::ns_per_ms;
            audio_floor = std::max(audio_floor, 3 * refresh);
            delay_limit = maximum_delay_ns;
            break;
        case performance_mode::performance:
            video_margin = refresh / 2 + 2 * clock::ns_per_ms;
            audio_floor = audio_latency + 8 * clock::ns_per_ms;
            break;
        case performance_mode::balanced:
            break;
        }
        presentation_wait_limit_ns.store(delay_limit);
        for (presentation_timeline &timeline : timelines)
        {
            timeline.configure_margins(video_margin, audio_floor, delay_limit);
        }
        presenter->set_performance_mode(selected);
        window->set_performance_mode(selected);
    }

    void media_engine::state::on_display_change(std::optional<presented_stream> stream)
    {
        {
            std::scoped_lock lock{shown_mutex};
            shown_stream = stream;
        }
        if (!stream)
        {
            displayed_source.store(-1);
            control_changed(false); // nothing to control: the control session ends
            system_tuning::set_session_active(false);
            window->set_status(
                std::format(L"{} · {}", settings.window_title, ui_string(ui_text::waiting_for_connection)));
            window->set_stream_info({}, {});
            if (!settings.keep_window_on_stop)
            {
                window->hide();
            }
            return;
        }
        if (!session_live[source_index(stream->source)].load())
        {
            // A picture kept on screen after its session ended ("-nc") was
            // redrawn (the presenter lets go of a crop at the end): the
            // window is neither resized nor shown again for it.
            return;
        }
        bool const became_shown = displayed_source.exchange(static_cast<int>(stream->source)) < 0;
        system_tuning::set_session_active(true);
        bool const over_usb = airplay_over_usb.load();
        // the window follows the part of the picture that is shown
        picture_region const visible =
            stream->visible.empty() ? picture_region{0, 0, stream->width, stream->height} : stream->visible;
        bool const cropped = visible.width != stream->width || visible.height != stream->height;
        label_stream(*stream);
        window->show_for_video(visible.width, visible.height);
        // Remote control: a stream to control appeared (worth another try
        // after a failure), or its picture changed - the phone may have been
        // turned, and only the control session can ask it which way.
        {
            std::scoped_lock lock{control_mutex};
            if (control)
            {
                control->picture_changed();
            }
        }
        control_changed(became_shown);
        char const *const path = over_usb ? "AirPlay over the USB cable" : "AirPlay over Wi-Fi";
        if (cropped)
        {
            log::info("displaying {} {}x{}, showing {}x{} at ({}, {})", path, stream->width, stream->height,
                      visible.width, visible.height, visible.left, visible.top);
        }
        else
        {
            log::info("displaying {} {}x{}", path, stream->width, stream->height);
            // A part framed by hand belongs to the stream it was drawn on.
            // When another stream (source or picture size) is shown whole, the
            // presenter has dropped it and the standing choice is in force
            // again.  (No presenter call from its own listener.)
            if (crop.load() == crop_mode::manual)
            {
                std::scoped_lock lock{crop_mutex};
                if (crop.load() == crop_mode::manual &&
                    (manual_stream.source != stream->source || manual_stream.width != stream->width ||
                     manual_stream.height != stream->height))
                {
                    crop_mode const standing = base_crop.load();
                    crop.store(standing);
                    ++crop_generation;
                    window->set_crop_mode(standing);
                }
            }
        }
    }

    // Automatic crop (decode thread): looks at a decoded picture every
    // crop_examination_interval_ns (more often while a crop is in force, so
    // that content appearing in a cut-off bar is noticed quickly) and returns
    // the region to show when that has to change.  The caller applies it
    // under the pipeline lock, after it made sure the session still runs.
    std::optional<picture_region> media_engine::state::inspect_for_crop(media_source source,
                                                                        decoded_frame const &picture,
                                                                        crop_inspection &inspection)
    {
        if (crop.load() != crop_mode::automatic)
        {
            if (std::exchange(inspection.reading, false))
            {
                inspection.luma.release(); // the staging copy is not kept while unused
            }
            return std::nullopt;
        }
        std::uint64_t const generation = crop_generation.load();
        if (generation != inspection.generation)
        {
            // switched on, a new session, or the user chose again: judge afresh
            inspection.generation = generation;
            inspection.detector.reset();
            inspection.next_examination_ns = 0;
            inspection.disagreements = 0;
            inspection.reconciliations = 0;
        }
        std::int64_t const now_ns = clock::now_ns();
        // every picture while the detector tolerates content in a bar: the
        // next one may settle it
        if (now_ns < inspection.next_examination_ns && !inspection.detector.grace_deadline())
        {
            return std::nullopt;
        }
        inspection.reading = true;
        inspection.picture_width = picture.width();
        inspection.picture_height = picture.height();
        std::optional<picture_region> decision;
        status const read = inspection.luma.read(picture, [&inspection, &decision, now_ns](luma_view const &view)
        {
            decision = inspection.detector.examine(view, now_ns);
        });
        inspection.next_examination_ns = now_ns + (inspection.detector.region().empty()
                                                       ? crop_examination_interval_ns
                                                       : cropped_examination_interval_ns);
        if (!read)
        {
            // first failure, then a count now and then: the picture is simply not cropped
            if (++inspection.failures == 1 || inspection.failures % 300 == 0)
            {
                log::warning("automatic crop: the picture could not be read ({} so far): {}", inspection.failures,
                             read.error().describe());
            }
            return std::nullopt;
        }
        if (decision)
        {
            inspection.disagreements = 0;
            inspection.reconciliations = 0;
            return decision;
        }
        // No change decided.  What the presenter shows must still be what the
        // detector believes: the presenter drops a region by itself when a
        // picture of another size passes, however briefly.
        std::optional<presented_stream> current;
        {
            std::scoped_lock lock{shown_mutex};
            current = shown_stream;
        }
        if (!current || current->source != source || current->width != picture.width() ||
            current->height != picture.height())
        {
            inspection.disagreements = 0;
            return std::nullopt;
        }
        picture_region const whole{0, 0, picture.width(), picture.height()};
        picture_region const believed = inspection.detector.region().empty() ? whole : inspection.detector.region();
        picture_region const actual = current->visible.empty() ? whole : current->visible;
        if (believed == actual)
        {
            inspection.disagreements = 0;
            inspection.reconciliations = 0;
            return std::nullopt;
        }
        // twice in a row (the presenter needs one picture to follow a change),
        // and not for ever when the two cannot agree
        if (++inspection.disagreements < 2 || inspection.reconciliations >= 3)
        {
            return std::nullopt;
        }
        inspection.disagreements = 0;
        ++inspection.reconciliations;
        return inspection.detector.region();
    }

    // Shows the region the detector decided (decode thread, pipeline lock
    // held by the caller: the session cannot end underneath).
    void media_engine::state::apply_detected_crop(media_source source, picture_region region, std::uint64_t generation,
                                                  int picture_width, int picture_height)
    {
        {
            std::scoped_lock lock{crop_mutex};
            if (crop.load() != crop_mode::automatic || crop_generation.load() != generation)
            {
                return; // the user chose something else meanwhile
            }
            presenter->set_crop(source, region);
        }
        if (region.empty())
        {
            log::info("automatic crop: showing the whole {}x{} picture", picture_width, picture_height);
        }
        else
        {
            log::info("automatic crop: showing {}x{} at ({}, {}) of the {}x{} picture", region.width, region.height,
                      region.left, region.top, picture_width, picture_height);
        }
    }

    // The user chose a crop mode (off or automatic): the whole picture is
    // shown at once; automatic then detects from there.
    void media_engine::state::choose_crop_mode(crop_mode selected)
    {
        {
            std::scoped_lock lock{crop_mutex};
            crop.store(selected);
            ++crop_generation;
            for (media_source const source : sources_by_priority)
            {
                presenter->set_crop(source, {});
            }
        }
        window->set_crop_mode(selected);
    }

    // The user framed a part of the picture as it is shown now (UI thread):
    // `selection` is in fractions of the visible part.
    void media_engine::state::apply_crop_selection(fraction_region selection)
    {
        if (!presenter)
        {
            return; // the engine is still starting
        }
        std::optional<presented_stream> current;
        {
            std::scoped_lock lock{shown_mutex};
            current = shown_stream;
        }
        if (!current)
        {
            return;
        }
        picture_region const visible =
            current->visible.empty() ? picture_region{0, 0, current->width, current->height} : current->visible;
        double const left = std::clamp(selection.left, 0.0, 1.0);
        double const top = std::clamp(selection.top, 0.0, 1.0);
        double const right = std::clamp(selection.right, left, 1.0);
        double const bottom = std::clamp(selection.bottom, top, 1.0);
        if (right - left >= whole_selection_fraction && bottom - top >= whole_selection_fraction)
        {
            return; // (nearly) everything that is shown: nothing to cut
        }
        // even coordinates and sizes: 4:2:0 chroma
        picture_region region;
        region.left = (visible.left + static_cast<int>(std::lround(left * visible.width))) & ~1;
        region.top = (visible.top + static_cast<int>(std::lround(top * visible.height))) & ~1;
        region.width = (visible.left + static_cast<int>(std::lround(right * visible.width)) - region.left) & ~1;
        region.height = (visible.top + static_cast<int>(std::lround(bottom * visible.height)) - region.top) & ~1;
        if (region.width < smallest_crop_edge || region.height < smallest_crop_edge)
        {
            log::info("crop: the framed part ({}x{} pixels of the picture) is too small and was ignored", region.width,
                      region.height);
            return;
        }
        {
            std::scoped_lock lock{crop_mutex};
            if (!session_live[source_index(current->source)].load())
            {
                return; // the session ended while the frame was drawn
            }
            crop.store(crop_mode::manual);
            ++crop_generation;
            manual_stream = presented_stream{current->source, current->width, current->height, {}};
            presenter->set_crop(current->source, region);
        }
        window->set_crop_mode(crop_mode::manual);
        log::info("crop: showing {}x{} at ({}, {}) of the {}x{} picture (framed by hand)", region.width, region.height,
                  region.left, region.top, current->width, current->height);
    }

    void media_engine::state::decode_loop(media_source source)
    {
        system_tuning::mmcss_scope const mmcss{L"Playback"};
        video_pipeline &pipeline = video[source_index(source)];
        presentation_timeline &timeline = timelines[source_index(source)];
        std::unique_ptr<video_decoder> decoder;
        std::uint64_t decoder_generation = 0;
        crop_inspection inspection{*gpu};
        // the last unit was passed with more_input_ready: the decoder may
        // still hold its picture, which only the next unit brings out
        bool flush_needed = false;

        for (;;)
        {
            access_unit unit;
            video_codec codec = video_codec::h264;
            bool release_now = false;
            bool more_input_ready = false;
            bool crop_grace_ended = false;
            // the crop detector tolerates content in a bar until then, and
            // must be told when no picture came by that time
            std::optional<std::int64_t> const crop_deadline =
                crop.load() == crop_mode::automatic ? inspection.detector.grace_deadline() : std::nullopt;
            {
                std::unique_lock lock{pipeline.mutex};
                auto const work_waits = [&pipeline, &flush_needed]
                {
                    // a pause holds the stream back, but not the one unit
                    // that brings out pictures the decoder still holds
                    return pipeline.stopping || pipeline.release_decoder ||
                           (!pipeline.queue.empty() && (!pipeline.paused || flush_needed));
                };
                if (crop_deadline)
                {
                    std::int64_t const remaining_ns = std::max<std::int64_t>(*crop_deadline - clock::now_ns(), 0);
                    crop_grace_ended = !pipeline.wake.wait_for(lock, std::chrono::nanoseconds{remaining_ns}, work_waits);
                }
                else
                {
                    pipeline.wake.wait(lock, work_waits);
                }
                if (pipeline.stopping)
                {
                    return;
                }
                if (crop_grace_ended)
                {
                    // The stream went still with content in a bar of the crop:
                    // the whole picture is shown (applied under this lock, so
                    // not for a session that ended).
                    if (std::optional<picture_region> const whole = inspection.detector.expire(clock::now_ns());
                        whole && pipeline.codec && pipeline.generation == decoder_generation)
                    {
                        apply_detected_crop(source, *whole, inspection.generation, inspection.picture_width,
                                            inspection.picture_height);
                    }
                    continue;
                }
                if (pipeline.release_decoder)
                {
                    pipeline.release_decoder = false;
                    release_now = true;
                }
                else
                {
                    unit = std::move(pipeline.queue.front());
                    pipeline.queue.pop_front();
                    if (unit.generation != pipeline.generation || !pipeline.codec)
                    {
                        continue;
                    }
                    codec = *pipeline.codec;
                    // the next unit of this stream is waiting already: a
                    // software decoder may then work on both at once
                    more_input_ready = !pipeline.queue.empty() && !pipeline.paused &&
                                       pipeline.queue.front().generation == unit.generation;
                }
            }
            if (release_now)
            {
                // the session ended: the decoder and its GPU surface pool are
                // not kept while idle, and neither is what was learned about
                // the picture
                if (decoder)
                {
                    // FFmpeg's hardware context uses the device context
                    std::scoped_lock gpu_lock{gpu->context_mutex()};
                    decoder.reset();
                }
                flush_needed = false;
                inspection.luma.release();
                inspection.reading = false;
                inspection.generation = 0;
                continue;
            }
            if (unit.generation != decoder_generation)
            {
                // a new session (or codec): decode failures are reported
                // again, and the crop is detected afresh (the presenter shows
                // the whole picture of a new stream)
                decoder_generation = unit.generation;
                inspection.generation = 0;
                std::scoped_lock lock{pipeline.mutex};
                pipeline.failures = 0;
            }

            if (!decoder || decoder->codec() != codec)
            {
                decoder.reset();
                flush_needed = false;
                // the decoder falls back to software decoding by itself when
                // the GPU cannot decode the stream
                auto created = video_decoder::create(*gpu, codec, true);
                if (!created)
                {
                    // every unit would fail the same way: say it a few times
                    std::scoped_lock lock{pipeline.mutex};
                    if (++pipeline.failures <= 5)
                    {
                        log::error("{} video decoder: {}", media_source_name(source), created.error().describe());
                    }
                    continue;
                }
                decoder = std::move(*created);
                std::scoped_lock lock{pipeline.mutex};
                pipeline.hardware = decoder->hardware();
            }

            auto frames = decoder->decode(unit.bytes, unit.media_time_ns, more_input_ready);
            flush_needed = more_input_ready;
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
                continue;
            }
            decoded_frame const &newest = *frames->back().frame;
            int const picture_width = newest.width();
            int const picture_height = newest.height();
            std::optional<picture_region> const crop_decision = inspect_for_crop(source, newest, inspection);
            // taken after the inspection: the delay controller sees its cost
            std::int64_t const ready_ns = clock::now_ns();

            // The pictures are handed over under the pipeline lock, which
            // video_stop takes to end the session: a picture decoded while
            // the session ended is never shown afterwards, and neither is a
            // crop decided from it.
            std::scoped_lock lock{pipeline.mutex};
            if (unit.generation != pipeline.generation)
            {
                continue;
            }
            if (crop_decision)
            {
                apply_detected_crop(source, *crop_decision, inspection.generation, picture_width, picture_height);
            }
            for (timed_frame &decoded : *frames)
            {
                std::int64_t present_ns = ready_ns;
                if (settings.timestamp_sync)
                {
                    // (the sound of this source reads the same delay when its
                    // packets are queued: audio_packet)
                    std::int64_t const lateness_ns = ready_ns - decoded.pts_ns;
                    timeline.observe_video(ready_ns, lateness_ns);
                    pipeline.lateness_minimum_ns = std::min(pipeline.lateness_minimum_ns, lateness_ns);
                    pipeline.lateness_maximum_ns = std::max(pipeline.lateness_maximum_ns, lateness_ns);
                    pipeline.lateness_sum_ns += lateness_ns;
                    ++pipeline.lateness_count;
                    // bounded from now: a picture never waits longer than the
                    // presenter's queue is deep
                    present_ns =
                        std::min(decoded.pts_ns + timeline.delay(), ready_ns + presentation_wait_limit_ns.load());
                }
                presenter->submit(source, std::move(decoded.frame), present_ns);
            }
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
        std::uint64_t presented_before = 0;
        std::uint64_t late_before = 0;
        std::uint64_t overflow_before = 0;
        bool device_loss_reported = false;
        while (!statistics_wake.wait_for(lock, std::chrono::nanoseconds{statistics_interval_ns}, [this]
        {
            return stopping.load();
        }))
        {
            presenter_statistics const stats = presenter->statistics();
            if (stats.device_lost && !device_loss_reported)
            {
                device_loss_reported = true;
                log::error("the Direct3D device was lost: no picture can be shown until the program restarts");
                if (callbacks.on_device_lost)
                {
                    callbacks.on_device_lost();
                }
            }
            // the default audio endpoint changed to one with another latency
            // (the format listener does not fire when the format is the
            // same): the audio buffering floor follows it
            if (std::int64_t const latency = audio->device_latency_ns(); latency != applied_audio_latency.load())
            {
                apply_performance_mode();
                log::info("audio device latency is {:.1f} ms now: timing margins updated",
                          static_cast<double>(latency) / 1e6);
            }
            // taken in every interval, so that the figures of a session that
            // ended do not show up in the first line of the next one
            presenter_interval interval = presenter->take_interval();
            int const shown = displayed_source.load();
            if (shown < 0)
            {
                presented_before = stats.presented;
                late_before = stats.dropped_late;
                overflow_before = stats.dropped_overflow;
                continue;
            }
            auto const source = static_cast<media_source>(shown);
            if (interval.stream_width <= 0 || interval.stream_height <= 0)
            {
                // no picture was presented in this interval (a still screen)
                std::scoped_lock shown_lock{shown_mutex};
                if (shown_stream)
                {
                    interval.stream_width = shown_stream->width;
                    interval.stream_height = shown_stream->height;
                }
            }
            std::uint64_t video_received = 0;
            std::uint64_t video_decoded = 0;
            std::uint64_t video_failures = 0;
            bool video_hardware = false;
            bool video_session = false;
            std::int64_t lateness_minimum_ns = 0;
            std::int64_t lateness_maximum_ns = 0;
            std::int64_t lateness_mean_ns = 0;
            {
                video_pipeline &flow = video[source_index(source)];
                std::scoped_lock pipeline_lock{flow.mutex};
                video_received = std::exchange(flow.received, 0);
                video_decoded = flow.decoded;
                video_failures = flow.failures;
                video_hardware = flow.hardware;
                // running: configured, not stopped, and not paused (by the
                // phone, or by the host when it keeps a last picture)
                video_session = flow.codec.has_value() && !flow.paused;
                if (flow.lateness_count > 0)
                {
                    lateness_minimum_ns = flow.lateness_minimum_ns;
                    lateness_maximum_ns = flow.lateness_maximum_ns;
                    lateness_mean_ns = flow.lateness_sum_ns / static_cast<std::int64_t>(flow.lateness_count);
                }
                flow.lateness_minimum_ns = std::numeric_limits<std::int64_t>::max();
                flow.lateness_maximum_ns = std::numeric_limits<std::int64_t>::min();
                flow.lateness_sum_ns = 0;
                flow.lateness_count = 0;
            }
            // The display stays awake as long as the stream runs: the phone
            // sends no pictures while its screen does not change, so the
            // number presented says nothing.  A last picture kept after the
            // stream ended or paused ("-nc", connection lost, phone locked)
            // does not keep it awake.
            window->set_stream_active(video_session);
            log::info("{} video {}x{}: {} units received, {} decoded in total ({}), {} decode failures, decoded "
                      "min {:.1f} mean {:.1f} max {:.1f} ms after its timestamp | presented {} (+{}), dropped late "
                      "+{} overflow +{} | lateness p50 {:.1f} p95 {:.1f} ms, queue peak {}, wake overshoot {:.1f} ms "
                      "| mode {}",
                      media_source_name(source), interval.stream_width, interval.stream_height, video_received,
                      video_decoded, video_hardware ? "hardware" : "software", video_failures,
                      static_cast<double>(lateness_minimum_ns) / 1e6, static_cast<double>(lateness_mean_ns) / 1e6,
                      static_cast<double>(lateness_maximum_ns) / 1e6,
                      stats.presented, stats.presented - presented_before, stats.dropped_late - late_before,
                      stats.dropped_overflow - overflow_before, static_cast<double>(interval.lateness_p50_ns) / 1e6,
                      static_cast<double>(interval.lateness_p95_ns) / 1e6, interval.peak_queue_depth,
                      static_cast<double>(interval.worst_wake_overshoot_ns) / 1e6,
                      performance_mode_name(mode.load()));
            presented_before = stats.presented;
            late_before = stats.dropped_late;
            overflow_before = stats.dropped_overflow;
            std::uint64_t packets = 0;
            std::uint64_t empty_outputs = 0;
            std::uint64_t failures = 0;
            std::uint64_t decoded_frames = 0;
            float peak = 0.0F;
            std::int64_t audio_lateness_minimum_ns = 0;
            std::int64_t audio_lateness_maximum_ns = 0;
            std::int64_t audio_lateness_mean_ns = 0;
            {
                audio_pipeline &flow = audio_pipelines[source_index(source)];
                std::scoped_lock pipeline_lock{flow.mutex};
                packets = std::exchange(flow.packets, 0);
                empty_outputs = std::exchange(flow.empty_outputs, 0);
                decoded_frames = std::exchange(flow.decoded_frames, 0);
                peak = std::exchange(flow.peak, 0.0F);
                failures = flow.failures;
                if (flow.lateness_count > 0)
                {
                    audio_lateness_minimum_ns = flow.lateness_minimum_ns;
                    audio_lateness_maximum_ns = flow.lateness_maximum_ns;
                    audio_lateness_mean_ns = flow.lateness_sum_ns / static_cast<std::int64_t>(flow.lateness_count);
                }
                flow.lateness_minimum_ns = std::numeric_limits<std::int64_t>::max();
                flow.lateness_maximum_ns = std::numeric_limits<std::int64_t>::min();
                flow.lateness_sum_ns = 0;
                flow.lateness_count = 0;
            }
            audio_flow const played = audio->take_flow(source);
            // audio: where a silent session loses its sound (no packets, decoder
            // output empty or all zero, nothing played, or volume 0)
            log::info("{}: presented {} dropped {} | sync delay {:.1f} ms | audio {} packets ({} empty, {} failed "
                      "total), arrived min {:.1f} mean {:.1f} max {:.1f} ms after their timestamps, decoded {} frames "
                      "peak {}, played {} frames, volume x{:.2f}{} | audio device latency {:.1f} ms",
                      media_source_name(source), stats.presented, stats.dropped,
                      static_cast<double>(timelines[source_index(source)].delay()) / 1e6, packets, empty_outputs,
                      failures, static_cast<double>(audio_lateness_minimum_ns) / 1e6,
                      static_cast<double>(audio_lateness_mean_ns) / 1e6,
                      static_cast<double>(audio_lateness_maximum_ns) / 1e6, decoded_frames, level_text(peak),
                      played.played_frames, played.gain,
                      played.muted ? " (muted here)" : "",
                      static_cast<double>(audio->device_latency_ns()) / 1e6);
        }
    }

    media_engine::media_engine(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    media_engine::~media_engine()
    {
        // the window thread lives until the window is destroyed below: from
        // here on it calls nothing of the engine any more
        if (impl->window)
        {
            impl->window->detach_callbacks();
        }
        // the control session ends with its thread (its reports use the window)
        {
            std::scoped_lock lock{impl->control_mutex};
            impl->control_exit = true;
        }
        impl->control_wake.notify_all();
        if (impl->control_thread.joinable())
        {
            impl->control_thread.join();
        }
        {
            // under the mutex the statistics thread waits with: no lost wake-up
            std::scoped_lock lock{impl->statistics_mutex};
            impl->stopping.store(true);
        }
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
            // a phone that was missing may have been plugged in
            bool phone_was_missing = false;
            {
                std::scoped_lock lock{engine->control_mutex};
                phone_was_missing = engine->control_gave_up &&
                                    (engine->control_problem == remote_control_problem::no_device ||
                                     engine->control_problem == remote_control_problem::not_trusted);
            }
            if (phone_was_missing)
            {
                engine->control_changed(true);
            }
            if (engine->callbacks.on_usb_device_change)
            {
                engine->callbacks.on_usb_device_change();
            }
        };
        window_events.on_tray_command = [engine](tray_command command)
        {
            if (command == tray_command::toggle_mute)
            {
                // an engine concern (it owns the audio output)
                if (!engine->audio)
                {
                    return; // the tray menu works before the audio output exists
                }
                bool const now_muted = !engine->muted.load();
                engine->muted.store(now_muted);
                engine->audio->set_muted(now_muted);
                engine->window->set_muted(now_muted);
                log::info("audio output {} here (the phone's volume is not touched)", now_muted ? "muted" : "unmuted");
                return;
            }
            if (engine->callbacks.on_tray_command)
            {
                engine->callbacks.on_tray_command(command);
            }
        };
        window_events.on_crop_selected = [engine](fraction_region selection)
        {
            engine->apply_crop_selection(selection);
        };
        window_events.on_pointer = [engine](pointer_action action, double x, double y)
        {
            engine->control_pointer(action, x, y);
        };
        window_events.on_wheel = [engine](double x, double y, double horizontal, double vertical)
        {
            engine->control_wheel(x, y, horizontal, vertical);
        };
        window_events.on_keys = [engine](std::span<std::uint8_t const> usages)
        {
            std::scoped_lock lock{engine->control_mutex};
            if (engine->control)
            {
                engine->control->keys(usages);
            }
        };
        window_events.on_swipe = [engine](swipe_action action, double x, double y)
        {
            engine->control_swipe(action, x, y);
        };
        window_events.on_home_button = [engine]
        {
            std::scoped_lock lock{engine->control_mutex};
            if (engine->control)
            {
                engine->control->press(phone_button::home);
            }
        };
        window_events.on_second_launch = [engine]
        {
            if (engine->callbacks.on_second_launch)
            {
                engine->callbacks.on_second_launch();
            }
        };
        window_events.on_session_ending = [engine]
        {
            if (engine->callbacks.on_session_ending)
            {
                engine->callbacks.on_session_ending();
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
                if (engine->audio)
                {
                    engine->apply_performance_mode(); // the margins follow the refresh period
                }
            }
            else
            {
                log::warning("display changed: re-query {}", mode ? "ok but presenter not ready" : "failed");
            }
            if (engine->callbacks.on_display_changed)
            {
                engine->callbacks.on_display_changed();
            }
        };
        auto window = video_window::create(created->settings.window_title, std::move(window_events),
                                           created->settings.start_fullscreen, created->settings.tray_icon);
        if (!window)
        {
            return std::unexpected(window.error());
        }
        created->window = std::move(*window);
        created->window->set_status(std::format(L"{} · {}", created->settings.window_title,
                                                 ui_string(ui_text::waiting_for_connection)));

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

        // video drives the delay (decode headroom); audio only sets a small
        // buffering floor so its cold-start cannot lurch it
        created->mode.store(created->settings.mode);
        created->apply_performance_mode();
        crop_mode const standing_crop =
            created->settings.crop == crop_mode::automatic ? crop_mode::automatic : crop_mode::off;
        created->base_crop.store(standing_crop);
        created->crop.store(standing_crop);
        created->window->set_crop_mode(standing_crop);
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
        created->control_wanted.store(created->settings.remote_control);
        created->window->set_remote_control(created->settings.remote_control, false);
        created->control_thread = std::thread{[engine]
        {
            engine->control_loop();
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

    bool media_engine::h264_hardware() const
    {
        return impl->gpu && impl->gpu->adapter_info().h264_decode;
    }

    gpu_adapter_info media_engine::adapter() const
    {
        return impl->gpu ? impl->gpu->adapter_info() : gpu_adapter_info{};
    }

    void media_engine::set_performance_mode(performance_mode mode)
    {
        impl->mode.store(mode);
        impl->apply_performance_mode();
        log::info("mode: {}", performance_mode_name(mode));
    }

    void media_engine::set_crop_mode(crop_mode mode)
    {
        crop_mode const standing = mode == crop_mode::automatic ? crop_mode::automatic : crop_mode::off;
        impl->base_crop.store(standing);
        impl->choose_crop_mode(standing);
        log::info("crop: {}", crop_mode_name(standing));
    }

    void media_engine::set_remote_control(bool wanted)
    {
        impl->control_wanted.store(wanted);
        impl->window->set_remote_control(wanted, wanted && impl->control_ready.load());
        log::info("remote control: {}", wanted ? "wanted" : "not wanted");
        impl->control_changed(true);
    }

    bool media_engine::showing_video() const
    {
        return impl->displayed_source.load() >= 0;
    }

    void media_engine::set_autostart(bool enabled)
    {
        impl->window->set_autostart(enabled);
    }

    void media_engine::set_log_available(bool available)
    {
        impl->window->set_log_available(available);
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
        impl->session_live[source_index(source)].store(true);
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
            ++pipeline.received;
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
        {
            // no crop is set for this session from here on (the UI thread and
            // the decode thread check under the same lock), and a part framed
            // by hand ends with the session
            std::scoped_lock lock{impl->crop_mutex};
            impl->session_live[source_index(source)].store(false);
            if (impl->crop.load() == crop_mode::manual)
            {
                crop_mode const standing = impl->base_crop.load();
                impl->crop.store(standing);
                ++impl->crop_generation;
                impl->window->set_crop_mode(standing);
            }
        }
        // the presenter lets go of this source's pictures first; then the
        // decode thread frees the decoder and with it the GPU surface pool
        impl->presenter->release_source(source, impl->settings.keep_window_on_stop);
        {
            std::scoped_lock lock{pipeline.mutex};
            pipeline.release_decoder = true;
        }
        pipeline.wake.notify_all();
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
        ++pipeline.packets;
        {
            std::int64_t const lateness_ns = arrival_time_ns - media_time_ns;
            pipeline.lateness_minimum_ns = std::min(pipeline.lateness_minimum_ns, lateness_ns);
            pipeline.lateness_maximum_ns = std::max(pipeline.lateness_maximum_ns, lateness_ns);
            pipeline.lateness_sum_ns += lateness_ns;
            ++pipeline.lateness_count;
        }
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
            ++pipeline.empty_outputs;
            return;
        }
        pipeline.decoded_frames += decoded->stereo_samples.size() / 2;
        for (float const sample : decoded->stereo_samples)
        {
            pipeline.peak = std::max(pipeline.peak, std::abs(sample));
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
            // an endpoint that keeps more queued than the fixed lead would
            // find every chunk late
            std::int64_t const lead_ns =
                std::max(unsynced_audio_buffer_ns, impl->audio->device_latency_ns() + unsynced_audio_margin_ns);
            impl->audio->enqueue(source, arrival_time_ns + lead_ns - decoded->resampler_delay_ns,
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

    void media_engine::set_usb_link(std::wstring text, bool offer_data_guard, bool offer_enable_adapter)
    {
        impl->window->set_usb_link(std::move(text), offer_data_guard, offer_enable_adapter);
    }

    void media_engine::set_link_preference(bool prefer_usb_link)
    {
        impl->window->set_link_preference(prefer_usb_link);
    }

    void *media_engine::window_handle() const
    {
        return impl->window->native_handle();
    }

    void media_engine::stop_window_callbacks()
    {
        impl->window->detach_callbacks();
    }

    void media_engine::set_airplay_over_usb(bool over_usb)
    {
        impl->airplay_over_usb.store(over_usb, std::memory_order_relaxed);
    }

    void media_engine::notify(std::wstring title, std::wstring text, std::optional<tray_command> on_click)
    {
        impl->window->notify(std::move(title), std::move(text), on_click);
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
