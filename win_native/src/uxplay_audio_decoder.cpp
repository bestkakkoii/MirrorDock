module;

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

module uxplay_audio_decoder;

import std;
import uxplay_status;
import uxplay_log;

namespace uxplay
{
    namespace
    {
        // AudioSpecificConfig / magic cookies used by AirPlay (see UxPlay's
        // renderers/audio_renderer.c caps strings)
        constexpr std::array<std::uint8_t, 4> aac_eld_config{0xf8, 0xe8, 0x50, 0x00};
        constexpr std::array<std::uint8_t, 2> aac_lc_config{0x12, 0x10};
        constexpr std::array<std::uint8_t, 36> alac_cookie{
            0x00, 0x00, 0x00, 0x24, 0x61, 0x6c, 0x61, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x60, 0x00, 0x10,
            0x28, 0x0a, 0x0e, 0x02, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xac, 0x44};

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
    }

    struct audio_decoder::state
    {
        audio_codec codec = audio_codec::aac_eld;
        int input_rate = 44100;
        int input_channels = 2;
        int output_rate = 48000;
        AVCodecContext *context = nullptr;
        AVPacket *packet = nullptr;
        AVFrame *frame = nullptr;
        SwrContext *resampler = nullptr;
        AVSampleFormat resampler_input_format = AV_SAMPLE_FMT_NONE;
        int resampler_input_rate = 0;
        int resampler_input_channels = 0;

        ~state()
        {
            swr_free(&resampler);
            av_frame_free(&frame);
            av_packet_free(&packet);
            avcodec_free_context(&context);
        }

        status ensure_resampler(AVSampleFormat format, int rate, int channels);
        status convert(std::uint8_t const *const *planes, int frames, decoded_audio &output);
    };

    status audio_decoder::state::ensure_resampler(AVSampleFormat format, int rate, int channels)
    {
        if (resampler != nullptr && format == resampler_input_format && rate == resampler_input_rate &&
            channels == resampler_input_channels)
        {
            return {};
        }
        swr_free(&resampler);
        AVChannelLayout input_layout{};
        av_channel_layout_default(&input_layout, channels);
        AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
        int result = swr_alloc_set_opts2(&resampler, &output_layout, AV_SAMPLE_FMT_FLT, output_rate, &input_layout,
                                         format, rate, 0, nullptr);
        av_channel_layout_uninit(&input_layout);
        if (result < 0)
        {
            return std::unexpected(av_failure("swr_alloc_set_opts2", result));
        }
        // a longer sinc filter: transparent 44.1 -> 48 kHz conversion
        av_opt_set_int(resampler, "filter_size", 48, 0);
        av_opt_set_double(resampler, "cutoff", 0.97, 0);
        result = swr_init(resampler);
        if (result < 0)
        {
            return std::unexpected(av_failure("swr_init", result));
        }
        resampler_input_format = format;
        resampler_input_rate = rate;
        resampler_input_channels = channels;
        return {};
    }

    status audio_decoder::state::convert(std::uint8_t const *const *planes, int frames, decoded_audio &output)
    {
        std::int64_t const delay_ns = swr_get_delay(resampler, 1'000'000'000);
        int const capacity = swr_get_out_samples(resampler, frames);
        if (capacity < 0)
        {
            return std::unexpected(av_failure("swr_get_out_samples", capacity));
        }
        std::size_t const start = output.stereo_samples.size();
        if (start == 0)
        {
            output.resampler_delay_ns = delay_ns;
        }
        output.stereo_samples.resize(start + static_cast<std::size_t>(capacity) * 2);
        auto *destination = reinterpret_cast<std::uint8_t *>(output.stereo_samples.data() + start);
        int const converted = swr_convert(resampler, &destination, capacity, planes, frames);
        if (converted < 0)
        {
            output.stereo_samples.resize(start);
            return std::unexpected(av_failure("swr_convert", converted));
        }
        output.stereo_samples.resize(start + static_cast<std::size_t>(converted) * 2);
        return {};
    }

    audio_decoder::audio_decoder(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    audio_decoder::~audio_decoder() = default;

    outcome<std::unique_ptr<audio_decoder>> audio_decoder::create(audio_codec codec, int input_rate,
                                                                  int input_channels, int output_rate)
    {
        auto created = std::make_unique<state>();
        created->codec = codec;
        created->input_rate = input_rate;
        created->input_channels = input_channels;
        created->output_rate = output_rate;
        created->frame = av_frame_alloc();
        created->packet = av_packet_alloc();
        if (created->frame == nullptr || created->packet == nullptr)
        {
            return std::unexpected(make_failure("av_frame_alloc", AVERROR(ENOMEM), "out of memory"));
        }

        if (codec != audio_codec::pcm_s16le)
        {
            AVCodecID const codec_id = codec == audio_codec::alac ? AV_CODEC_ID_ALAC : AV_CODEC_ID_AAC;
            AVCodec const *decoder = avcodec_find_decoder(codec_id);
            if (decoder == nullptr)
            {
                return std::unexpected(make_failure("avcodec_find_decoder (audio)", 0, "decoder not compiled into FFmpeg"));
            }
            created->context = avcodec_alloc_context3(decoder);
            if (created->context == nullptr)
            {
                return std::unexpected(make_failure("avcodec_alloc_context3 (audio)", AVERROR(ENOMEM), "out of memory"));
            }
            std::span<std::uint8_t const> extradata;
            switch (codec)
            {
            case audio_codec::aac_eld:
                extradata = aac_eld_config;
                break;
            case audio_codec::aac_lc:
                extradata = aac_lc_config;
                break;
            case audio_codec::alac:
                extradata = alac_cookie;
                break;
            case audio_codec::pcm_s16le:
                break;
            }
            created->context->extradata =
                static_cast<std::uint8_t *>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            std::memcpy(created->context->extradata, extradata.data(), extradata.size());
            created->context->extradata_size = static_cast<int>(extradata.size());
            created->context->sample_rate = input_rate;
            av_channel_layout_default(&created->context->ch_layout, input_channels);
            created->context->flags |= AV_CODEC_FLAG_LOW_DELAY;
            int const result = avcodec_open2(created->context, decoder, nullptr);
            if (result < 0)
            {
                return std::unexpected(av_failure("avcodec_open2 (audio)", result));
            }
        }
        else
        {
            status const ready = created->ensure_resampler(AV_SAMPLE_FMT_S16, input_rate, input_channels);
            if (!ready)
            {
                return std::unexpected(ready.error());
            }
        }
        return std::unique_ptr<audio_decoder>{new audio_decoder{std::move(created)}};
    }

    audio_codec audio_decoder::codec() const
    {
        return impl->codec;
    }

    int audio_decoder::output_rate() const
    {
        return impl->output_rate;
    }

    outcome<decoded_audio> audio_decoder::decode(std::span<std::uint8_t const> packet_data)
    {
        decoded_audio output;
        if (impl->codec == audio_codec::pcm_s16le)
        {
            int const frames = static_cast<int>(packet_data.size() / (2 * static_cast<std::size_t>(impl->input_channels)));
            std::uint8_t const *planes[1] = {packet_data.data()};
            status const converted = impl->convert(planes, frames, output);
            if (!converted)
            {
                return std::unexpected(converted.error());
            }
            return output;
        }

        AVPacket *packet = impl->packet;
        int result = av_new_packet(packet, static_cast<int>(packet_data.size()));
        if (result < 0)
        {
            return std::unexpected(av_failure("av_new_packet (audio)", result));
        }
        std::memcpy(packet->data, packet_data.data(), packet_data.size());
        result = avcodec_send_packet(impl->context, packet);
        av_packet_unref(packet);
        if (result < 0 && result != AVERROR(EAGAIN))
        {
            return std::unexpected(av_failure("avcodec_send_packet (audio)", result));
        }
        for (;;)
        {
            result = avcodec_receive_frame(impl->context, impl->frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            {
                break;
            }
            if (result < 0)
            {
                return std::unexpected(av_failure("avcodec_receive_frame (audio)", result));
            }
            AVFrame *frame = impl->frame;
            status ready = impl->ensure_resampler(static_cast<AVSampleFormat>(frame->format), frame->sample_rate,
                                                  frame->ch_layout.nb_channels);
            if (ready)
            {
                ready = impl->convert(const_cast<std::uint8_t const *const *>(frame->extended_data), frame->nb_samples,
                                      output);
            }
            av_frame_unref(frame);
            if (!ready)
            {
                return std::unexpected(ready.error());
            }
        }
        return output;
    }
}
