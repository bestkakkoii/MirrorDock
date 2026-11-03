// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

extern "C"
{
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

module uxplay_video_frame;

import std;

namespace uxplay
{
    decoded_frame::decoded_frame(AVFrame *owned_frame)
        : frame{owned_frame},
          kind{frame_memory::cpu_yuv420p}
    {
        switch (frame->format)
        {
        case AV_PIX_FMT_D3D11:
            kind = frame_memory::d3d11_nv12;
            break;
        case AV_PIX_FMT_NV12:
            kind = frame_memory::cpu_nv12;
            break;
        default:
            kind = frame_memory::cpu_yuv420p;
            break;
        }
    }

    decoded_frame::~decoded_frame()
    {
        av_frame_free(&frame);
    }

    frame_memory decoded_frame::memory() const
    {
        return kind;
    }

    int decoded_frame::width() const
    {
        return frame->width;
    }

    int decoded_frame::height() const
    {
        return frame->height;
    }

    frame_color_range decoded_frame::color_range() const
    {
        // Apple mirroring streams signal full range (colorimetry 1:3:7:1);
        // AVCOL_FMT_YUVJ* formats imply full range too.
        if (frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P)
        {
            return frame_color_range::full;
        }
        return frame_color_range::limited;
    }

    frame_color_matrix decoded_frame::color_matrix() const
    {
        switch (frame->colorspace)
        {
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_FCC:
            return frame_color_matrix::bt601;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            return frame_color_matrix::bt2020;
        default:
            // unspecified on Apple streams means BT.709 (HD content)
            return frame_color_matrix::bt709;
        }
    }

    ID3D11Texture2D *decoded_frame::texture() const
    {
        return reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);
    }

    unsigned int decoded_frame::array_index() const
    {
        return static_cast<unsigned int>(reinterpret_cast<std::intptr_t>(frame->data[1]));
    }

    std::uint8_t const *decoded_frame::plane(int index) const
    {
        return frame->data[index];
    }

    int decoded_frame::stride(int index) const
    {
        return frame->linesize[index];
    }
}
