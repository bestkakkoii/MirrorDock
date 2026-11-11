// SPDX-License-Identifier: MIT
// Decoded picture handed from a video decoder to the presenter.
//
// Owns one AVFrame reference.  Hardware frames reference a slice of the
// decoder's D3D11 NV12/P010 texture array (bound as a shader resource, so the
// presenter samples it directly); software frames expose CPU planes.
//
// A picture exists only in a layout the presenter can show: D3D11 NV12 or
// P010, or software yuv420p / yuvj420p / nv12 with all planes present.
// decoded_frame::create rejects anything else as a failure that names the
// format, so no consumer ever sees a missing plane.
//
// A decoder may pass a counter that is one higher for as long as the picture
// lives: that is how it knows how many of its surfaces are still held
// outside (presenter queue, screen).
//
// luma_reader gives picture analysis (automatic crop) the luma plane on the
// CPU: in place for software pictures, through a staging texture it keeps
// for hardware pictures.
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

extern "C"
{
#include <libavutil/frame.h>
}

export module uxplay_video_frame;

import std;
import uxplay_status;
import uxplay_gpu_device;
import uxplay_media_types;

export namespace uxplay
{
    enum class frame_memory
    {
        d3d11_nv12,
        cpu_yuv420p,
        cpu_nv12
    };

    enum class frame_color_range
    {
        limited,
        full
    };

    enum class frame_color_matrix
    {
        bt601,
        bt709,
        bt2020
    };

    class decoded_frame
    {
    public:
        // Takes ownership of `owned_frame` in every case (it is freed when the
        // picture cannot be used).  `live_pictures`, when given, is incremented
        // now and decremented when the picture is destroyed.
        [[nodiscard]] static outcome<std::unique_ptr<decoded_frame>> create(
            AVFrame *owned_frame, std::shared_ptr<std::atomic<int>> live_pictures = nullptr);

        ~decoded_frame();
        decoded_frame(decoded_frame const &) = delete;
        decoded_frame &operator=(decoded_frame const &) = delete;

        [[nodiscard]] frame_memory memory() const;
        [[nodiscard]] int width() const;
        [[nodiscard]] int height() const;
        [[nodiscard]] frame_color_range color_range() const;
        [[nodiscard]] frame_color_matrix color_matrix() const;

        // d3d11_nv12 only
        [[nodiscard]] ID3D11Texture2D *texture() const;
        [[nodiscard]] unsigned int array_index() const;

        // cpu_* only
        [[nodiscard]] std::uint8_t const *plane(int index) const;
        [[nodiscard]] int stride(int index) const;

    private:
        decoded_frame(AVFrame *owned_frame, frame_memory memory_kind, std::shared_ptr<std::atomic<int>> live_pictures);

        AVFrame *frame;
        frame_memory kind;
        std::shared_ptr<std::atomic<int>> live;
    };

    // CPU access to the luma plane of decoded pictures, for picture analysis
    // (automatic crop).  One reader is used by one thread.
    //
    // Software pictures are viewed in place.  A hardware picture (a slice of
    // the decoder's NV12 / P010 texture array) is copied to a staging texture
    // that the reader keeps between calls (recreated when the surface size or
    // format changes) and mapped for reading.  The device context is used
    // under gpu_device::context_mutex(), which is held for the copy and the
    // map and again for the unmap, not while `use` runs.
    class luma_reader
    {
    public:
        explicit luma_reader(gpu_device &device);
        ~luma_reader();
        luma_reader(luma_reader const &) = delete;
        luma_reader &operator=(luma_reader const &) = delete;

        // Calls `use` exactly once with a view of the picture's luma plane,
        // valid only during the call; on failure `use` is not called and the
        // failure (stage, HRESULT) is returned.  The view has the picture's
        // size (not the padded surface's); for P010 it steps over the low
        // bytes (sample_stride 2, data at the high byte).
        [[nodiscard]] status read(decoded_frame const &picture, std::function<void(luma_view const &)> const &use);

        // frees the staging texture; the next read creates it again
        void release();

    private:
        struct state;

        std::unique_ptr<state> impl;
    };
}
