// SPDX-License-Identifier: MIT
// Timestamp-scheduled audio output through WASAPI shared mode.
//
// * IAudioClient3 minimum engine period (event driven) for the smallest
//   device buffer the driver allows; falls back to a 10 ms IAudioClient.
// * Every source keeps one continuous sample timeline (gaps are filled with
//   silence on enqueue).  At each device period the frame that will be heard
//   next is computed from IAudioClock's QPC-correlated position, so a sample
//   with presentation time T is played at local time T.
// * Small timing errors (clock drift between phone, PC clock and DAC) are
//   corrected by varying the read rate by at most 0.3 % with 4-point cubic
//   Hermite interpolation — no clicks, inaudible pitch change.  Errors above
//   30 ms resynchronize immediately (silence or skip).
// * Only the highest-priority source with queued audio is heard (USB first).
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

export module uxplay_audio_output;

import std;
import uxplay_status;
import uxplay_media_types;

export namespace uxplay
{
    struct audio_device_format
    {
        int sample_rate = 48000;
        int channels = 2;

        friend bool operator==(audio_device_format const &, audio_device_format const &) = default;
    };

    // Invoked on the audio thread after the endpoint (re)opened with a new format.
    using audio_format_listener = std::function<void(audio_device_format)>;

    // What the output actually did with a source since the last query.
    struct audio_flow
    {
        std::uint64_t played_frames = 0; // frames taken from the source's queue
        float gain = 1.0F;               // current per-source volume
        bool muted = false;              // UxPlay's own mute (all sources)
    };

    class audio_output
    {
    public:
        static outcome<std::unique_ptr<audio_output>> create(audio_format_listener listener);

        ~audio_output();
        audio_output(audio_output const &) = delete;
        audio_output &operator=(audio_output const &) = delete;

        [[nodiscard]] audio_device_format format() const;

        // Interleaved stereo float32 at `sample_rate`.  timestamped: the
        // first frame belongs to media_time_ns and plays at media time +
        // set_delay(); the queue stays continuous in media time, so delay
        // changes are absorbed by the smooth rate correction (no gaps).
        // Not timestamped: appended back to back (play on arrival), anchored
        // at media_time_ns only when the queue is empty.  Data at a sample
        // rate other than the device rate is rejected (the listener
        // announces format changes).
        void enqueue(media_source source, std::int64_t media_time_ns, std::span<float const> stereo_samples,
                     int sample_rate, bool timestamped);

        // Presentation delay applied to the media timeline of a source.
        void set_delay(media_source source, std::int64_t delay_ns);

        void release_source(media_source source);

        // per-source linear gain (AirPlay volume applies to Wi-Fi audio only)
        void set_volume(media_source source, float linear_gain);

        // local mute of the whole output, independent of the senders' volume
        void set_muted(bool muted);

        // Engine period + device stream latency of the open endpoint.
        [[nodiscard]] std::int64_t device_latency_ns() const;

        // played frames since the previous call (counter is reset) and gain
        [[nodiscard]] audio_flow take_flow(media_source source);

    private:
        struct state;

        explicit audio_output(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
