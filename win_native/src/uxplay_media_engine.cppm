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
    };

    struct engine_callbacks
    {
        // the user closed the window while `source` was on screen
        std::function<void(media_source source)> on_window_closed;
        std::function<void()> on_usb_device_change;
        std::function<void(tray_command command)> on_tray_command;
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

        [[nodiscard]] source_statistics statistics(media_source source) const;

        // notification-area integration
        void set_usb_enabled(bool enabled);

        // Whether the current AirPlay client is connected over the iPhone USB
        // link (so the window shows "USB" rather than "Wi-Fi").
        void set_airplay_over_usb(bool over_usb);
        void notify(std::wstring title, std::wstring text);

    private:
        struct state;

        explicit media_engine(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
