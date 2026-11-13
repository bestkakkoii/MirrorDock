// SPDX-License-Identifier: MIT
// Timestamp-scheduled video presentation on a flip-model swap chain.
//
// * DXGI_SWAP_EFFECT_FLIP_DISCARD, 2 buffers, frame-latency waitable object
//   with maximum latency 1: a presented frame reaches the screen at the next
//   vblank, never queued behind older frames.
// * Each source has its own queue ordered by presentation time.  The render
//   thread sleeps on a high-resolution timer until the head of the queue is
//   due (half a refresh period before its presentation time) and is woken
//   early only when the head changes.
// * What is shown when several pictures are due depends on the mode
//   (set_performance_mode):
//     quality     - in order; a picture gives way to its successor only when
//                   it is more than 2 refresh periods late; queue of 13
//     balanced    - in order; more than 1 refresh period late; queue of 8
//     performance - the newest due picture, older due ones are dropped;
//                   queue of 8; bilinear scaling only
//   A picture that is late but has no due successor is still shown: the
//   newest picture always reaches the screen.  A picture beyond the queue
//   limit evicts the oldest one.
// * The queue lock is never held while drawing or presenting: submit(),
//   resize() and statistics() wait for the queue only, not for the GPU.
// * Source arbitration: the highest-priority source holding pictures is
//   displayed; releasing it falls back immediately to the next source's most
//   recent picture.  When the last picture is released the render thread
//   frees everything that belongs to a stream (views on decoder textures,
//   copy/upload textures, the swap chain); the next picture recreates them.
// * Crop (set_crop): only a rectangle of a source's pictures is shown.  The
//   letterbox is computed from the rectangle's aspect ratio, so a window of
//   that ratio is filled edge to edge, and every texture fetch of every filter
//   is clamped to the rectangle (to the picture when nothing is cut off), so
//   neither cut-off bars nor decoder surface padding bleed into the edge
//   pixels.  A crop belongs to one picture size: it is dropped when a picture
//   of another size is shown and when the source is released.  The listener
//   learns the visible rectangle with every change (presented_stream::visible).
// * A lost D3D11 device is reported once (presenter_statistics::device_lost)
//   and stops all GPU work; other render failures are logged once and then
//   as a count at most every 10 s.
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
        // the part of the picture that is shown; {0, 0, width, height} when
        // nothing is cut off (never empty)
        picture_region visible{};

        friend bool operator==(presented_stream const &, presented_stream const &) = default;
    };

    // Called on the render thread whenever the displayed stream changes
    // (source switch, resolution change, another visible part, or nothing left
    // to display).
    using display_change_listener = std::function<void(std::optional<presented_stream>)>;

    // Cumulative since the presenter was created.
    struct presenter_statistics
    {
        std::uint64_t presented = 0;
        std::uint64_t dropped = 0;          // dropped_late + dropped_overflow
        std::int64_t mean_lateness_ns = 0;  // moving average of (selection time - presentation time)
        std::uint64_t dropped_late = 0;     // gave way to a newer due picture
        std::uint64_t dropped_overflow = 0; // evicted by the queue limit
        bool device_lost = false;           // the D3D11 device was removed or reset: nothing is drawn any more
    };

    // What happened since the previous take_interval() call.
    struct presenter_interval
    {
        // (selection time - presentation time) of the pictures chosen for
        // display; negative = chosen ahead of its presentation time
        std::int64_t lateness_p50_ns = 0;
        std::int64_t lateness_p95_ns = 0;
        std::size_t peak_queue_depth = 0;
        // how long after a timed deadline the render thread woke at worst
        std::int64_t worst_wake_overshoot_ns = 0;
        // size of the last picture presented (0 x 0: none)
        int stream_width = 0;
        int stream_height = 0;
    };

    // The picture on screen as it is shown: its visible part in the picture's
    // own pixels (not the window's), whatever the size of the window.
    struct captured_picture
    {
        int width = 0;
        int height = 0;
        // 4 bytes a pixel (blue, green, red, unused), rows from the top, no padding
        std::vector<std::uint8_t> pixels;
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
        // screen (for "-nc") until another source takes over.  Without it,
        // once nothing is left to display, the render thread also frees the
        // per-stream GPU objects (see above), so no presenter object refers to
        // decoder textures afterwards.
        void release_source(media_source source, bool keep_last_frame);

        // Shows only `region` of the pictures of `source` (empty: the whole
        // picture).  Any thread; waits for the queue lock only, never for a
        // Present.  The region is clamped to the picture and its edges are
        // moved inwards to even positions; if less than 16 x 16 remains the
        // whole picture is shown and one warning is logged.  It is checked
        // against the picture on screen, or, when the source has none yet,
        // against its first picture.  The picture on screen is redrawn with
        // it at once, and the listener is told the new visible part.
        void set_crop(media_source source, picture_region region);

        [[nodiscard]] std::int64_t refresh_period_ns() const;

        // Update the vblank pacing period after a monitor/mode change.
        void set_refresh_period(std::int64_t refresh_period_ns);

        // Drop policy, queue limit and scaling filter (see above); any thread,
        // any time, default balanced.  Also publishes the number of pictures
        // the presenter may hold to the device (gpu_device::presentation_hold),
        // which sizes the decoder surface pool of the next stream.
        void set_performance_mode(performance_mode mode);

        // A copy of the picture on screen (a screenshot).  The render thread
        // makes it when it next draws, which it is asked to do at once: the
        // call waits for that, some milliseconds, and no longer than
        // `patience`.  A failure when nothing is shown, when the graphics
        // device is lost or when the copy cannot be made.  Not from the
        // display change listener; one call at a time.
        [[nodiscard]] outcome<captured_picture> capture(std::chrono::milliseconds patience);

        [[nodiscard]] presenter_statistics statistics() const;

        // The figures gathered since the previous call; resets them.  Meant
        // for one periodic caller (the engine's statistics loop).
        [[nodiscard]] presenter_interval take_interval();

    private:
        struct state;

        explicit video_presenter(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
