// Decoded picture handed from a video decoder to the presenter.
//
// Owns one AVFrame reference.  Hardware frames reference a slice of the
// decoder's D3D11 NV12 texture array (bound as a shader resource, so the
// presenter samples it directly); software frames expose CPU planes.
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
        // Takes ownership of `owned_frame` (freed with av_frame_free).
        explicit decoded_frame(AVFrame *owned_frame);
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
        AVFrame *frame;
        frame_memory kind;
    };
}
