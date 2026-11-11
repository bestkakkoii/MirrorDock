// SPDX-License-Identifier: MIT
// Native media engine: owns the GPU device, video window, presenter and
// audio output, and runs one decode pipeline per media source.
//
// Synchronization model ("one timeline per source"):
//   present_time = media_time + delay(source)
// media_time is the sender's timestamp mapped onto the local clock (NTP for
// AirPlay, minimum-transport-delay offset for USB); audio and video of a
// source share the same mapping and the same delay, so lip sync is exact by
// construction.  The delay adapts to the smallest value that still lets the
// late tail (95th percentile) of video frames be decoded and audio be queued
// in time: minimal latency without stutter.
export module uxplay_media_engine;

import std;
import uxplay_status;
import uxplay_media_types;
import uxplay_video_decoder;
import uxplay_audio_decoder;
import uxplay_gpu_device;
import uxplay_video_window;

export namespace uxplay
{
    struct engine_settings
    {
        std::wstring window_title;
        bool start_fullscreen = false;
        bool timestamp_sync = true;
        bool keep_window_on_stop = false;
        gpu_preference gpu = gpu_preference::automatic;
        bool tray_icon = false;
        performance_mode mode = performance_mode::balanced;
        crop_mode crop = crop_mode::off; // off or automatic
        // Remote control of the phone (uxplay_remote_control): the user's
        // standing choice, and the directory in which the Developer Disk
        // Image is kept once it was fetched for a phone that lacked it
        // (empty: it is fetched each time one does).
        bool remote_control = false;
        std::filesystem::path control_cache;
    };

    struct engine_callbacks
    {
        // the user closed the window while `source` was on screen
        std::function<void(media_source source)> on_window_closed;
        std::function<void()> on_usb_device_change;
        std::function<void(tray_command command)> on_tray_command;
        // the program was started again while this instance runs (UI thread)
        std::function<void()> on_second_launch;
        // Windows shuts down or the user signs out (UI thread; may block briefly)
        std::function<void()> on_session_ending;
        // the monitor configuration changed; presentation is already re-paced
        std::function<void()> on_display_changed;
        // the Direct3D device was lost (driver update or crash): no picture can
        // be shown until the program restarts (statistics thread, once)
        std::function<void()> on_device_lost;
    };

    struct display_summary
    {
        int width = 0;
        int height = 0;
        double refresh_hz = 60.0;
    };

    struct source_statistics
    {
        std::uint64_t decoded = 0;
        std::uint64_t presented = 0; // all sources
        std::uint64_t dropped = 0;   // all sources
        std::int64_t sync_delay_ns = 0;
        bool hardware_decode = false;
    };

    class media_engine
    {
    public:
        static outcome<std::unique_ptr<media_engine>> create(engine_settings settings, engine_callbacks callbacks);

        ~media_engine();
        media_engine(media_engine const &) = delete;
        media_engine &operator=(media_engine const &) = delete;

        [[nodiscard]] display_summary display() const;

        // Whether the selected GPU decodes HEVC Main in hardware (so H.265 is
        // worth offering to the client; otherwise H.264 gives a better result).
        [[nodiscard]] bool hevc_hardware() const;
        // Whether the selected GPU decodes H.264 in hardware (without it every
        // stream is decoded on the CPU, which a lighter stream helps).
        [[nodiscard]] bool h264_hardware() const;

        void video_configure(media_source source, video_codec codec, int width, int height);
        // Annex-B access unit; media_time_ns is on the local clock.
        void video_access_unit(media_source source, std::span<std::uint8_t const> access_unit,
                               std::int64_t media_time_ns, std::int64_t arrival_time_ns);
        void video_pause(media_source source);
        void video_resume(media_source source);
        void video_stop(media_source source);

        void audio_configure(media_source source, audio_codec codec, int sample_rate, int channels,
                             bool timestamp_sync, double audio_offset_ms);
        void audio_packet(media_source source, std::span<std::uint8_t const> packet, std::int64_t media_time_ns,
                          std::int64_t arrival_time_ns);
        void audio_stop(media_source source);

        void set_volume(media_source source, double linear_gain);

        // quality / balanced / performance: timing margins, frame dropping
        // and scaling follow at once (the stream size is the host's part)
        void set_performance_mode(performance_mode mode);
        // off: the whole picture; automatic: black bars around the content are
        // cut off (uxplay_crop_detector).  The third mode, a part framed by
        // hand, is entered from the window and lasts until the stream changes
        // or one of these two is chosen again.
        void set_crop_mode(crop_mode mode);
        // Mouse and keyboard on the picture operate the phone: a control
        // session with it runs while this is wanted and a stream is shown.
        // Why it cannot is told to the user in a notification and to the log.
        void set_remote_control(bool wanted);
        // a stream is on screen
        [[nodiscard]] bool showing_video() const;

        [[nodiscard]] source_statistics statistics(media_source source) const;

        // notification-area integration
        void set_usb_link(std::wstring text, bool offer_data_guard, bool offer_enable_adapter);
        void set_link_preference(bool prefer_usb_link);
        // menu state owned by the host: "start with Windows", a log file to open
        void set_autostart(bool enabled);
        void set_log_available(bool available);

        // Whether the current AirPlay client is connected over the iPhone USB
        // link (so the window shows "USB" rather than "Wi-Fi").
        void set_airplay_over_usb(bool over_usb);
        void notify(std::wstring title, std::wstring text, std::optional<tray_command> on_click = std::nullopt);

        // HWND of the video window (owner for system prompts)
        [[nodiscard]] void *window_handle() const;

        // From now on the window thread calls nothing any more (menu commands,
        // keys, resizing): the host calls this before it destroys objects its
        // own callbacks use.  Returns when a callback that was running ended.
        void stop_window_callbacks();

    private:
        struct state;

        explicit media_engine(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
