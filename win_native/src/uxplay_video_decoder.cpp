// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}

module uxplay_video_decoder;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_gpu_device;
import uxplay_video_frame;

namespace uxplay
{
    namespace
    {
        // Pictures the presenter may hold on top of the decoder's DPB:
        // one displayed + up to four queued per source.
        constexpr int presenter_held_frames = 6;

        std::string av_error_text(int error_code)
        {
            char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
            av_strerror(error_code, buffer, sizeof(buffer));
            return buffer;
        }

        failure av_failure(std::string_view stage, int error_code)
        {
            return make_failure(stage, error_code, av_error_text(error_code));
        }

        void lock_context(void *context)
        {
            static_cast<std::recursive_mutex *>(context)->lock();
        }

        void unlock_context(void *context)
        {
            static_cast<std::recursive_mutex *>(context)->unlock();
        }

        AVPixelFormat choose_format(AVCodecContext *context, AVPixelFormat const *formats)
        {
            AVPixelFormat software_choice = AV_PIX_FMT_NONE;
            for (AVPixelFormat const *candidate = formats; *candidate != AV_PIX_FMT_NONE; ++candidate)
            {
                if (*candidate == AV_PIX_FMT_D3D11 && context->hw_device_ctx != nullptr)
                {
                    AVBufferRef *frames_reference = nullptr;
                    int result = avcodec_get_hw_frames_parameters(context, context->hw_device_ctx, AV_PIX_FMT_D3D11,
                                                                  &frames_reference);
                    if (result < 0)
                    {
                        log::warning("D3D11VA frame parameters unavailable ({}), using software decoding",
                                     av_error_text(result));
                        continue;
                    }
                    auto *frames = reinterpret_cast<AVHWFramesContext *>(frames_reference->data);
                    auto *d3d11_frames = static_cast<AVD3D11VAFramesContext *>(frames->hwctx);
                    d3d11_frames->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
                    frames->initial_pool_size += presenter_held_frames;
                    result = av_hwframe_ctx_init(frames_reference);
                    if (result < 0)
                    {
                        // Some drivers refuse shader-resource decoder arrays:
                        // retry decoder-only; the presenter then copies.
                        av_buffer_unref(&frames_reference);
                        result = avcodec_get_hw_frames_parameters(context, context->hw_device_ctx, AV_PIX_FMT_D3D11,
                                                                  &frames_reference);
                        if (result >= 0)
                        {
                            frames = reinterpret_cast<AVHWFramesContext *>(frames_reference->data);
                            frames->initial_pool_size += presenter_held_frames;
                            result = av_hwframe_ctx_init(frames_reference);
                        }
                        if (result < 0)
                        {
                            av_buffer_unref(&frames_reference);
                            log::warning("D3D11VA frame pool creation failed ({}), using software decoding",
                                         av_error_text(result));
                            continue;
                        }
                        log::info("D3D11VA decoder textures are not shader-readable: presenter will copy");
                    }
                    av_buffer_unref(&context->hw_frames_ctx);
                    context->hw_frames_ctx = frames_reference;
                    return AV_PIX_FMT_D3D11;
                }
                AVPixFmtDescriptor const *descriptor = av_pix_fmt_desc_get(*candidate);
                if (software_choice == AV_PIX_FMT_NONE && descriptor != nullptr &&
                    (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0)
                {
                    software_choice = *candidate;
                }
            }
            return software_choice;
        }
    }

    struct video_decoder::state
    {
        video_codec codec = video_codec::h264;
        bool hardware = false;
        AVCodecContext *context = nullptr;
        AVBufferRef *device_reference = nullptr;
        AVPacket *packet = nullptr;
        std::uint64_t rejected = 0;

        ~state()
        {
            av_packet_free(&packet);
            avcodec_free_context(&context);
            av_buffer_unref(&device_reference);
        }
    };

    video_decoder::video_decoder(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    video_decoder::~video_decoder() = default;

    outcome<std::unique_ptr<video_decoder>> video_decoder::create(gpu_device &device, video_codec codec,
                                                                  bool allow_hardware)
    {
        auto created = std::make_unique<state>();
        created->codec = codec;
        AVCodec const *decoder = avcodec_find_decoder(codec == video_codec::h264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
        if (decoder == nullptr)
        {
            return std::unexpected(make_failure("avcodec_find_decoder", 0, "decoder not compiled into FFmpeg"));
        }
        created->context = avcodec_alloc_context3(decoder);
        created->packet = av_packet_alloc();
        if (created->context == nullptr || created->packet == nullptr)
        {
            return std::unexpected(make_failure("avcodec_alloc_context3", AVERROR(ENOMEM), "out of memory"));
        }
        AVCodecContext *context = created->context;
        context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        context->pkt_timebase = AVRational{1, 1'000'000'000};

        if (allow_hardware)
        {
            created->device_reference = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (created->device_reference != nullptr)
            {
                auto *device_context = reinterpret_cast<AVHWDeviceContext *>(created->device_reference->data);
                auto *d3d11_device = static_cast<AVD3D11VADeviceContext *>(device_context->hwctx);
                d3d11_device->device = device.device();
                d3d11_device->device->AddRef();
                d3d11_device->device_context = device.context();
                d3d11_device->device_context->AddRef();
                d3d11_device->lock = lock_context;
                d3d11_device->unlock = unlock_context;
                d3d11_device->lock_ctx = &device.context_mutex();
                int const result = av_hwdevice_ctx_init(created->device_reference);
                if (result < 0)
                {
                    log::warning("D3D11VA device init failed: {}", av_error_text(result));
                    av_buffer_unref(&created->device_reference);
                }
            }
        }

        if (created->device_reference != nullptr)
        {
            context->hw_device_ctx = av_buffer_ref(created->device_reference);
            context->get_format = choose_format;
            context->thread_count = 1;
            created->hardware = true;
        }
        else
        {
            // Software fallback: slice threads only (frame threads add latency).
            context->thread_count = 0;
            context->thread_type = FF_THREAD_SLICE;
        }

        int const result = avcodec_open2(context, decoder, nullptr);
        if (result < 0)
        {
            return std::unexpected(av_failure("avcodec_open2", result));
        }
        log::info("video decoder: {} {}", codec == video_codec::h264 ? "H.264" : "H.265",
                  created->hardware ? "D3D11VA hardware" : "software");
        return std::unique_ptr<video_decoder>{new video_decoder{std::move(created)}};
    }

    video_codec video_decoder::codec() const
    {
        return impl->codec;
    }

    bool video_decoder::hardware() const
    {
        return impl->hardware && impl->context->hw_frames_ctx != nullptr;
    }

    std::uint64_t video_decoder::rejected_units() const
    {
        return impl->rejected;
    }

    outcome<std::vector<timed_frame>> video_decoder::decode(std::span<std::uint8_t const> access_unit,
                                                            std::int64_t pts_ns)
    {
        std::vector<timed_frame> produced;
        AVPacket *packet = impl->packet;
        // av_new_packet adds the zeroed AV_INPUT_BUFFER_PADDING_SIZE tail FFmpeg requires.
        int result = av_new_packet(packet, static_cast<int>(access_unit.size()));
        if (result < 0)
        {
            return std::unexpected(av_failure("av_new_packet", result));
        }
        std::memcpy(packet->data, access_unit.data(), access_unit.size());
        packet->pts = pts_ns;
        packet->dts = pts_ns;

        result = avcodec_send_packet(impl->context, packet);
        av_packet_unref(packet);
        if (result < 0 && result != AVERROR(EAGAIN))
        {
            ++impl->rejected;
            if (result == AVERROR_INVALIDDATA)
            {
                return produced;
            }
            return std::unexpected(av_failure("avcodec_send_packet", result));
        }

        for (;;)
        {
            AVFrame *frame = av_frame_alloc();
            if (frame == nullptr)
            {
                return std::unexpected(make_failure("av_frame_alloc", AVERROR(ENOMEM), "out of memory"));
            }
            result = avcodec_receive_frame(impl->context, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            {
                av_frame_free(&frame);
                break;
            }
            if (result < 0)
            {
                av_frame_free(&frame);
                ++impl->rejected;
                return std::unexpected(av_failure("avcodec_receive_frame", result));
            }
            std::int64_t const frame_pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp
                                                                                           : pts_ns;
            produced.push_back(timed_frame{std::make_unique<decoded_frame>(frame), frame_pts});
        }
        return produced;
    }
}
