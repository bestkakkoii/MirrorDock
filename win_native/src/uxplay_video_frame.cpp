// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

extern "C"
{
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

module uxplay_video_frame;

import std;
import uxplay_status;
import uxplay_gpu_device;
import uxplay_media_types;

namespace uxplay
{
    namespace
    {
        std::string format_name(int format)
        {
            char const *const name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(format));
            return name != nullptr ? std::string{name} : std::format("pixel format {}", format);
        }

        failure unusable(AVFrame const *frame, std::string_view reason)
        {
            return make_failure("decoded picture", frame->format,
                                std::format("{} ({}, {}x{}); supported: D3D11 nv12/p010, yuv420p, yuvj420p, nv12",
                                            reason, format_name(frame->format), frame->width, frame->height));
        }

        // The layout of a picture, or why the presenter could not show it.
        outcome<frame_memory> classify(AVFrame const *frame)
        {
            if (frame->width <= 0 || frame->height <= 0)
            {
                return std::unexpected(unusable(frame, "picture without a size"));
            }
            int plane_count = 0;
            frame_memory kind = frame_memory::cpu_yuv420p;
            switch (frame->format)
            {
            case AV_PIX_FMT_D3D11:
            {
                if (frame->data[0] == nullptr || frame->hw_frames_ctx == nullptr)
                {
                    return std::unexpected(unusable(frame, "hardware picture without a texture"));
                }
                auto const *frames = reinterpret_cast<AVHWFramesContext const *>(frame->hw_frames_ctx->data);
                if (frames->sw_format != AV_PIX_FMT_NV12 && frames->sw_format != AV_PIX_FMT_P010)
                {
                    return std::unexpected(unusable(
                        frame, std::format("unsupported hardware surface format {}", format_name(frames->sw_format))));
                }
                return frame_memory::d3d11_nv12;
            }
            case AV_PIX_FMT_NV12:
                kind = frame_memory::cpu_nv12;
                plane_count = 2;
                break;
            case AV_PIX_FMT_YUV420P:
            case AV_PIX_FMT_YUVJ420P:
                kind = frame_memory::cpu_yuv420p;
                plane_count = 3;
                break;
            default:
                return std::unexpected(unusable(frame, "unsupported pixel format"));
            }
            for (int plane = 0; plane < plane_count; ++plane)
            {
                if (frame->data[plane] == nullptr || frame->linesize[plane] <= 0)
                {
                    return std::unexpected(unusable(frame, std::format("picture without plane {}", plane)));
                }
            }
            return kind;
        }
    }

    outcome<std::unique_ptr<decoded_frame>> decoded_frame::create(AVFrame *owned_frame,
                                                                 std::shared_ptr<std::atomic<int>> live_pictures)
    {
        if (owned_frame == nullptr)
        {
            return std::unexpected(make_failure("decoded picture", 0, "no frame"));
        }
        outcome<frame_memory> const kind = classify(owned_frame);
        if (!kind)
        {
            av_frame_free(&owned_frame);
            return std::unexpected(kind.error());
        }
        return std::unique_ptr<decoded_frame>{new decoded_frame{owned_frame, *kind, std::move(live_pictures)}};
    }

    decoded_frame::decoded_frame(AVFrame *owned_frame, frame_memory memory_kind,
                                 std::shared_ptr<std::atomic<int>> live_pictures)
        : frame{owned_frame},
          kind{memory_kind},
          live{std::move(live_pictures)}
    {
        if (live)
        {
            live->fetch_add(1, std::memory_order_relaxed);
        }
    }

    decoded_frame::~decoded_frame()
    {
        av_frame_free(&frame);
        if (live)
        {
            live->fetch_sub(1, std::memory_order_relaxed);
        }
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

    struct luma_reader::state
    {
        gpu_device *gpu = nullptr;
        // CPU-readable copy target for one decoder surface
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        UINT staging_width = 0;
        UINT staging_height = 0;
        DXGI_FORMAT staging_format = DXGI_FORMAT_UNKNOWN;

        status ensure_staging(D3D11_TEXTURE2D_DESC const &surface)
        {
            if (staging && staging_width == surface.Width && staging_height == surface.Height &&
                staging_format == surface.Format)
            {
                return {};
            }
            staging.Reset();
            D3D11_TEXTURE2D_DESC description{};
            description.Width = surface.Width;
            description.Height = surface.Height;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = surface.Format;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_STAGING;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            HRESULT const result = gpu->device()->CreateTexture2D(&description, nullptr, &staging);
            if (FAILED(result))
            {
                staging.Reset();
                return std::unexpected(make_hresult_failure("CreateTexture2D (luma staging)", result));
            }
            staging_width = surface.Width;
            staging_height = surface.Height;
            staging_format = surface.Format;
            return {};
        }
    };

    luma_reader::luma_reader(gpu_device &device)
        : impl{std::make_unique<state>()}
    {
        impl->gpu = &device;
    }

    luma_reader::~luma_reader() = default;

    void luma_reader::release()
    {
        impl->staging.Reset();
        impl->staging_width = 0;
        impl->staging_height = 0;
        impl->staging_format = DXGI_FORMAT_UNKNOWN;
    }

    status luma_reader::read(decoded_frame const &picture, std::function<void(luma_view const &)> const &use)
    {
        if (!use)
        {
            return std::unexpected(make_failure("luma_reader::read", 0, "no consumer for the luma plane"));
        }
        luma_view view;
        view.width = picture.width();
        view.height = picture.height();
        if (picture.memory() != frame_memory::d3d11_nv12)
        {
            // software picture: plane 0 in place (8-bit luma in every accepted layout)
            view.data = picture.plane(0);
            view.row_stride = picture.stride(0);
            view.sample_stride = 1;
            use(view);
            return {};
        }

        ID3D11Texture2D *const surface = picture.texture();
        D3D11_TEXTURE2D_DESC description{};
        surface->GetDesc(&description);
        bool const ten_bit = description.Format == DXGI_FORMAT_P010;
        if (!ten_bit && description.Format != DXGI_FORMAT_NV12)
        {
            return std::unexpected(make_failure("luma_reader::read", static_cast<std::int64_t>(description.Format),
                                                "decoder surface is neither NV12 nor P010"));
        }
        if (status const ready = impl->ensure_staging(description); !ready)
        {
            return std::unexpected(ready.error());
        }

        // The device context is used under its mutex: once for copy + map,
        // once for unmap.  `use` runs in between without it, so the render
        // thread is not held up while the picture is analysed.
        struct mapped_plane
        {
            gpu_device *gpu;
            ID3D11Texture2D *texture;
            D3D11_MAPPED_SUBRESOURCE memory{};
            bool mapped = false;

            ~mapped_plane()
            {
                if (mapped)
                {
                    std::scoped_lock gpu_lock{gpu->context_mutex()};
                    gpu->context()->Unmap(texture, 0);
                }
            }
        };
        mapped_plane plane{impl->gpu, impl->staging.Get()};
        {
            std::scoped_lock gpu_lock{impl->gpu->context_mutex()};
            ID3D11DeviceContext *const context = impl->gpu->context();
            context->CopySubresourceRegion(plane.texture, 0, 0, 0, 0, surface,
                                           D3D11CalcSubresource(0, picture.array_index(), description.MipLevels),
                                           nullptr);
            // waits until the copy has been carried out
            HRESULT const result = context->Map(plane.texture, 0, D3D11_MAP_READ, 0, &plane.memory);
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure("ID3D11DeviceContext::Map (luma staging)", result));
            }
            plane.mapped = true;
        }
        if (plane.memory.pData == nullptr)
        {
            return std::unexpected(make_failure("ID3D11DeviceContext::Map (luma staging)", 0, "no memory mapped"));
        }
        // the luma plane comes first; P010 keeps its 10 bits at the top of
        // little-endian 16-bit samples, so the high byte is the second one
        view.data = static_cast<std::uint8_t const *>(plane.memory.pData) + (ten_bit ? 1 : 0);
        view.row_stride = static_cast<std::ptrdiff_t>(plane.memory.RowPitch);
        view.sample_stride = ten_bit ? 2 : 1;
        use(view);
        return {};
    }
}
