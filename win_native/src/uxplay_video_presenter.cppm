// SPDX-License-Identifier: MIT
// Timestamp-scheduled video presentation on a flip-model swap chain.
//
// * DXGI_SWAP_EFFECT_FLIP_DISCARD, 2 buffers, frame-latency waitable object
//   with maximum latency 1: a presented frame reaches the screen at the next
//   vblank, never queued behind older frames.
// * Each source has its own queue ordered by presentation time; at every
//   presentation opportunity the newest frame that is due is shown and older
//   due frames are dropped (no backlog, constant latency).
// * Source arbitration: the highest-priority source holding pictures is
//   displayed (USB before AirPlay); releasing it falls back immediately to
//   the next source's most recent picture.
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

export module uxplay_video_presenter;

import std;
import uxplay_status;
import uxplay_gpu_device;
import uxplay_video_frame;
import uxplay_media_types;

export namespace uxplay
{
    struct presented_stream
    {
        media_source source = media_source::airplay;
        int width = 0;
        int height = 0;

        friend bool operator==(presented_stream const &, presented_stream const &) = default;
    };

    // Called on the render thread whenever the displayed stream changes
    // (source switch, resolution change, or nothing left to display).
    using display_change_listener = std::function<void(std::optional<presented_stream>)>;

    struct presenter_statistics
    {
        std::uint64_t presented = 0;
        std::uint64_t dropped = 0;
        std::int64_t mean_lateness_ns = 0;
    };

    class video_presenter
    {
    public:
        static outcome<std::unique_ptr<video_presenter>> create(gpu_device &device, void *window_handle,
                                                                std::int64_t refresh_period_ns,
                                                                display_change_listener listener);

        ~video_presenter();
        video_presenter(video_presenter const &) = delete;
        video_presenter &operator=(video_presenter const &) = delete;

        void resize(int client_width, int client_height);

        // Queues a decoded picture to be shown at present_time_ns (local clock).
        void submit(media_source source, std::unique_ptr<decoded_frame> frame, std::int64_t present_time_ns);

        // The source stopped.  keep_last_frame leaves its final picture on
        // screen (for "-nc") until another source takes over.
        void release_source(media_source source, bool keep_last_frame);

        [[nodiscard]] std::int64_t refresh_period_ns() const;

        // Update the vblank pacing period after a monitor/mode change.
        void set_refresh_period(std::int64_t refresh_period_ns);
        [[nodiscard]] presenter_statistics statistics() const;

    private:
        struct state;

        explicit video_presenter(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
