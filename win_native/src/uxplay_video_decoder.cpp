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
import uxplay_clock;
import uxplay_gpu_device;
import uxplay_video_frame;

namespace uxplay
{
    namespace
    {
        // Surfaces the decoder itself needs besides the pictures the stream
        // keeps for reference and reordering: the picture being decoded and
        // three spare ones (the margin libavcodec uses for its own pools).
        constexpr int decoder_work_surfaces = 4;
        constexpr int most_reference_pictures = 16;
        // D3D11 limit for a decoder texture array (libavcodec clamps to it too)
        constexpr int largest_texture_array = 64;
        // units kept for a software replay while hardware decoding starts
        constexpr std::size_t most_replay_units = 16;
        constexpr std::size_t most_replay_bytes = std::size_t{32} * 1024 * 1024;
        // parameter sets and the first slice header sit at the start of a unit
        constexpr std::size_t scanned_unit_bytes = 16 * 1024;
        constexpr std::size_t most_parameter_set_bytes = 512;
        constexpr int most_failed_results_per_call = 64;
        constexpr std::int64_t report_interval_ns = 10 * clock::ns_per_second;

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

        // Frame threads of the software decoder.  Pictures are read
        // synchronously, so the count adds no delay; it only bounds how many
        // units are decoded in parallel when the caller has a backlog.
        int software_frame_threads()
        {
            return static_cast<int>(std::clamp(std::thread::hardware_concurrency() / 2, 2U, 4U));
        }

        char const *codec_name(video_codec codec)
        {
            return codec == video_codec::h264 ? "H.264" : "H.265";
        }

        // What an H.265 sequence parameter set says about picture buffering.
        struct stream_buffering
        {
            int coded_width = 0;
            int coded_height = 0;
            int level = 0;
            // sps_max_dec_pic_buffering of the highest sub-layer: pictures the
            // decoder must be able to hold, the current one included
            int decoded_pictures = 0;
        };

        struct unit_summary
        {
            bool parameter_sets = false; // carries a sequence parameter set: decoding can start here
            bool key_picture = false;    // carries an IDR / IRAP slice
            std::optional<stream_buffering> buffering;
        };

        // Calls visit(nal) for each NAL unit (without its start code) of an
        // Annex-B byte stream.
        template <class visitor_type>
        void for_each_nal(std::span<std::uint8_t const> bytes, visitor_type &&visit)
        {
            std::size_t const size = bytes.size();
            std::optional<std::size_t> begin;
            std::size_t position = 0;
            while (position + 3 <= size)
            {
                if (bytes[position] == 0 && bytes[position + 1] == 0 && bytes[position + 2] == 1)
                {
                    if (begin && position > *begin)
                    {
                        visit(bytes.subspan(*begin, position - *begin));
                    }
                    position += 3;
                    begin = position;
                }
                else
                {
                    ++position;
                }
            }
            if (begin && *begin < size)
            {
                visit(bytes.subspan(*begin));
            }
        }

        // NAL payload with the emulation prevention bytes (00 00 03) removed.
        std::vector<std::uint8_t> raw_payload(std::span<std::uint8_t const> nal, std::size_t limit)
        {
            std::vector<std::uint8_t> payload;
            payload.reserve(std::min(nal.size(), limit));
            int zeros = 0;
            for (std::uint8_t const byte : nal)
            {
                if (payload.size() >= limit)
                {
                    break;
                }
                if (zeros >= 2 && byte == 3)
                {
                    zeros = 0;
                    continue;
                }
                payload.push_back(byte);
                zeros = byte == 0 ? zeros + 1 : 0;
            }
            return payload;
        }

        class bit_reader
        {
        public:
            explicit bit_reader(std::span<std::uint8_t const> payload)
                : data{payload}
            {
            }

            [[nodiscard]] bool failed() const
            {
                return overrun;
            }

            std::uint32_t bits(int count)
            {
                std::uint32_t value = 0;
                for (int index = 0; index < count; ++index)
                {
                    std::size_t const byte = position >> 3;
                    if (byte >= data.size())
                    {
                        overrun = true;
                        return 0;
                    }
                    unsigned int const shift = 7U - static_cast<unsigned int>(position & 7U);
                    value = (value << 1) | ((static_cast<std::uint32_t>(data[byte]) >> shift) & 1U);
                    ++position;
                }
                return value;
            }

            void skip(std::size_t count)
            {
                position += count;
                if (position > data.size() * 8)
                {
                    overrun = true;
                }
            }

            // ue(v)
            std::uint32_t exp_golomb()
            {
                int zeros = 0;
                while (bits(1) == 0)
                {
                    if (overrun || ++zeros > 31)
                    {
                        overrun = true;
                        return 0;
                    }
                }
                if (zeros == 0)
                {
                    return 0;
                }
                return ((std::uint32_t{1} << zeros) - 1U) + bits(zeros);
            }

        private:
            std::span<std::uint8_t const> data;
            std::size_t position = 0;
            bool overrun = false;
        };

        // H.265 SPS (ITU-T H.265 7.3.2.2) up to sps_max_dec_pic_buffering_minus1.
        // `nal` starts at the two-byte NAL unit header.
        std::optional<stream_buffering> parse_hevc_sps(std::span<std::uint8_t const> nal)
        {
            if (nal.size() < 4)
            {
                return std::nullopt;
            }
            std::vector<std::uint8_t> const payload = raw_payload(nal.subspan(2), most_parameter_set_bytes);
            bit_reader reader{payload};
            reader.skip(4); // sps_video_parameter_set_id
            std::uint32_t const max_sub_layers_minus1 = reader.bits(3);
            reader.skip(1); // sps_temporal_id_nesting_flag
            if (reader.failed() || max_sub_layers_minus1 > 6)
            {
                return std::nullopt;
            }

            // profile_tier_level(1, max_sub_layers_minus1)
            reader.skip(88); // general profile space, tier, profile, compatibility and constraint flags
            std::uint32_t const level = reader.bits(8); // general_level_idc
            std::array<bool, 8> sub_layer_profile{};
            std::array<bool, 8> sub_layer_level{};
            for (std::uint32_t layer = 0; layer < max_sub_layers_minus1; ++layer)
            {
                sub_layer_profile[layer] = reader.bits(1) != 0;
                sub_layer_level[layer] = reader.bits(1) != 0;
            }
            if (max_sub_layers_minus1 > 0)
            {
                reader.skip(std::size_t{2} * (8 - max_sub_layers_minus1)); // reserved_zero_2bits
            }
            for (std::uint32_t layer = 0; layer < max_sub_layers_minus1; ++layer)
            {
                if (sub_layer_profile[layer])
                {
                    reader.skip(88);
                }
                if (sub_layer_level[layer])
                {
                    reader.skip(8);
                }
            }

            reader.exp_golomb(); // sps_seq_parameter_set_id
            std::uint32_t const chroma_format = reader.exp_golomb();
            if (chroma_format == 3)
            {
                reader.skip(1); // separate_colour_plane_flag
            }
            std::uint32_t const width = reader.exp_golomb();  // pic_width_in_luma_samples
            std::uint32_t const height = reader.exp_golomb(); // pic_height_in_luma_samples
            if (reader.bits(1) != 0) // conformance_window_flag
            {
                for (int offset = 0; offset < 4; ++offset)
                {
                    reader.exp_golomb();
                }
            }
            reader.exp_golomb(); // bit_depth_luma_minus8
            reader.exp_golomb(); // bit_depth_chroma_minus8
            reader.exp_golomb(); // log2_max_pic_order_cnt_lsb_minus4
            bool const ordering_info_for_all = reader.bits(1) != 0; // sps_sub_layer_ordering_info_present_flag
            std::uint32_t decoded_pictures = 0;
            std::uint32_t const first_layer = ordering_info_for_all ? 0 : max_sub_layers_minus1;
            for (std::uint32_t layer = first_layer; layer <= max_sub_layers_minus1; ++layer)
            {
                decoded_pictures = reader.exp_golomb() + 1; // sps_max_dec_pic_buffering_minus1
                reader.exp_golomb();                        // sps_max_num_reorder_pics
                reader.exp_golomb();                        // sps_max_latency_increase_plus1
            }
            if (reader.failed() || width == 0 || height == 0 || width > 16384 || height > 16384 ||
                decoded_pictures == 0 || decoded_pictures > static_cast<std::uint32_t>(most_reference_pictures))
            {
                return std::nullopt;
            }
            return stream_buffering{static_cast<int>(width), static_cast<int>(height), static_cast<int>(level),
                                    static_cast<int>(decoded_pictures)};
        }

        unit_summary summarize(video_codec codec, std::span<std::uint8_t const> access_unit)
        {
            unit_summary summary;
            for_each_nal(access_unit.first(std::min(access_unit.size(), scanned_unit_bytes)),
                         [&summary, codec](std::span<std::uint8_t const> nal)
                         {
                             if (codec == video_codec::h264)
                             {
                                 int const type = nal[0] & 0x1F;
                                 summary.parameter_sets = summary.parameter_sets || type == 7;
                                 summary.key_picture = summary.key_picture || type == 5;
                                 return;
                             }
                             int const type = (nal[0] >> 1) & 0x3F;
                             bool const base_layer = nal.size() >= 2 && (nal[0] & 0x01) == 0 && (nal[1] >> 3) == 0;
                             if (type == 33 && base_layer)
                             {
                                 summary.parameter_sets = true;
                                 if (auto buffering = parse_hevc_sps(nal); buffering)
                                 {
                                     summary.buffering = buffering;
                                 }
                             }
                             else if (type >= 16 && type <= 21)
                             {
                                 summary.key_picture = true;
                             }
                         });
            return summary;
        }
    }

    struct video_decoder::state
    {
        struct replay_unit
        {
            std::vector<std::uint8_t> bytes;
            std::int64_t pts_ns = 0;
        };

        gpu_device *gpu = nullptr;
        video_codec codec = video_codec::h264;
        AVCodec const *decoder = nullptr;
        AVCodecContext *context = nullptr;
        AVBufferRef *device_reference = nullptr;
        AVPacket *packet = nullptr;
        bool hardware = false;        // the open context is set up for D3D11VA
        bool software_format = false; // ... but get_format had to settle on a software format
        std::uint64_t rejected = 0;

        // hardware start-up: units kept until the first hardware picture
        bool starting = false;
        std::vector<replay_unit> replay;
        std::size_t replay_bytes = 0;
        bool key_picture_seen = false;
        int units_since_key_picture = 0;

        // surface pool
        std::optional<stream_buffering> announced_buffering; // from the newest H.265 SPS
        std::shared_ptr<std::atomic<int>> pictures_out = std::make_shared<std::atomic<int>>(0);
        int pool_surfaces = 0;
        int pool_hold = 0;           // of those: for pictures alive outside the decoder
        bool full_size_pool = false; // a right-sized pool ran out: trust libavcodec's worst case from now on
        clock::interval_gate withheld_gate{report_interval_ns};
        clock::interval_gate problem_gate{report_interval_ns};

        state() = default;
        state(state const &) = delete;
        state &operator=(state const &) = delete;

        ~state()
        {
            close();
            av_packet_free(&packet);
        }

        status open(bool with_hardware);
        void close();
        [[nodiscard]] int decoder_surfaces(AVCodecContext const *codec_context, int suggested) const;
        bool create_pool(AVCodecContext *codec_context);
        void note_pool_exhausted();
        void keep_for_replay(unit_summary const &summary, std::span<std::uint8_t const> access_unit,
                             std::int64_t pts_ns);
        void end_start_up();
        void receive_pictures(unsigned int flags, std::int64_t fallback_pts_ns, std::vector<timed_frame> &produced,
                              std::optional<failure> &problem);
        outcome<std::vector<timed_frame>> decode_unit(std::span<std::uint8_t const> access_unit, std::int64_t pts_ns,
                                                      bool more_input_ready);
        outcome<std::vector<timed_frame>> restart_in_software(std::string_view reason);

        static AVPixelFormat choose_format(AVCodecContext *codec_context, AVPixelFormat const *formats);
    };

    void video_decoder::state::close()
    {
        avcodec_free_context(&context);
        av_buffer_unref(&device_reference);
        hardware = false;
        software_format = false;
        pool_surfaces = 0;
        pool_hold = 0;
    }

    status video_decoder::state::open(bool with_hardware)
    {
        close();
        end_start_up();
        context = avcodec_alloc_context3(decoder);
        if (context == nullptr)
        {
            return std::unexpected(make_failure("avcodec_alloc_context3", AVERROR(ENOMEM), "out of memory"));
        }
        context->opaque = this;
        context->pkt_timebase = AVRational{1, 1'000'000'000};

        if (with_hardware)
        {
            device_reference = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (device_reference == nullptr)
            {
                log::warning("D3D11VA device context: out of memory, using software decoding");
            }
            else
            {
                auto *device_context = reinterpret_cast<AVHWDeviceContext *>(device_reference->data);
                auto *d3d11_device = static_cast<AVD3D11VADeviceContext *>(device_context->hwctx);
                d3d11_device->device = gpu->device();
                d3d11_device->device->AddRef();
                d3d11_device->device_context = gpu->context();
                d3d11_device->device_context->AddRef();
                d3d11_device->lock = lock_context;
                d3d11_device->unlock = unlock_context;
                d3d11_device->lock_ctx = &gpu->context_mutex();
                int const result = av_hwdevice_ctx_init(device_reference);
                if (result < 0)
                {
                    log::warning("D3D11VA device init failed ({}), using software decoding", av_error_text(result));
                    av_buffer_unref(&device_reference);
                }
            }
            if (device_reference != nullptr)
            {
                context->hw_device_ctx = av_buffer_ref(device_reference);
                if (context->hw_device_ctx == nullptr)
                {
                    log::warning("D3D11VA device reference: out of memory, using software decoding");
                    av_buffer_unref(&device_reference);
                }
            }
        }

        if (device_reference != nullptr)
        {
            context->flags |= AV_CODEC_FLAG_LOW_DELAY;
            context->get_format = choose_format;
            context->thread_count = 1;
            hardware = true;
        }
        else
        {
            // No AV_CODEC_FLAG_LOW_DELAY here: in the pinned libavcodec its only
            // effect on these decoders is to switch frame threading off
            // (pthread.c, validate_thread_parameters).  Pictures still come
            // without delay because they are received synchronously.
            context->thread_count = software_frame_threads();
            context->thread_type = FF_THREAD_FRAME;
        }

        int const result = avcodec_open2(context, decoder, nullptr);
        if (result < 0)
        {
            failure const problem = av_failure("avcodec_open2", result);
            close();
            return std::unexpected(problem);
        }
        starting = hardware;
        if (hardware)
        {
            log::info("video decoder: {} D3D11VA hardware", codec_name(codec));
        }
        else
        {
            log::info("video decoder: {} software ({} frame threads)", codec_name(codec), context->thread_count);
        }
        return {};
    }

    int video_decoder::state::decoder_surfaces(AVCodecContext const *codec_context, int suggested) const
    {
        // libavcodec suggests room for the most pictures any stream may reference
        int const worst_case = suggested > 0 ? suggested : most_reference_pictures + 1;
        if (full_size_pool)
        {
            return worst_case;
        }
        std::optional<int> kept_pictures; // what this stream keeps besides the picture being decoded
        if (codec == video_codec::h264)
        {
            // set from the active SPS before get_format runs (max_num_ref_frames)
            if (codec_context->refs >= 0 && codec_context->refs <= most_reference_pictures)
            {
                kept_pictures = codec_context->refs + std::max(codec_context->has_b_frames, 0);
            }
        }
        else if (announced_buffering && announced_buffering->coded_width == codec_context->coded_width &&
                 announced_buffering->coded_height == codec_context->coded_height &&
                 announced_buffering->level == codec_context->level &&
                 announced_buffering->decoded_pictures > codec_context->has_b_frames)
        {
            // the SPS read here is the one libavcodec activated (same size and level)
            kept_pictures = announced_buffering->decoded_pictures - 1;
        }
        if (!kept_pictures)
        {
            return worst_case;
        }
        return std::min(std::clamp(*kept_pictures, 1, most_reference_pictures) + decoder_work_surfaces, worst_case);
    }

    bool video_decoder::state::create_pool(AVCodecContext *codec_context)
    {
        int const hold = static_cast<int>(std::min(gpu->presentation_hold(), 48U));
        // Some drivers refuse shader-readable decoder arrays: the second try
        // is decoder-only and the presenter copies each picture.
        for (bool const shader_readable : {true, false})
        {
            AVBufferRef *frames_reference = nullptr;
            int result = avcodec_get_hw_frames_parameters(codec_context, codec_context->hw_device_ctx,
                                                          AV_PIX_FMT_D3D11, &frames_reference);
            if (result < 0)
            {
                log::warning("D3D11VA frame parameters unavailable ({})", av_error_text(result));
                return false;
            }
            auto *frames = reinterpret_cast<AVHWFramesContext *>(frames_reference->data);
            auto *d3d11_frames = static_cast<AVD3D11VAFramesContext *>(frames->hwctx);
            int const for_decoder = decoder_surfaces(codec_context, frames->initial_pool_size);
            int const surfaces = std::min(for_decoder + hold, largest_texture_array);
            frames->initial_pool_size = surfaces;
            if (shader_readable)
            {
                d3d11_frames->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
            }
            result = av_hwframe_ctx_init(frames_reference);
            if (result < 0)
            {
                av_buffer_unref(&frames_reference);
                if (!shader_readable)
                {
                    log::warning("D3D11VA frame pool creation failed ({})", av_error_text(result));
                }
                continue;
            }
            av_buffer_unref(&codec_context->hw_frames_ctx);
            codec_context->hw_frames_ctx = frames_reference;
            pool_surfaces = surfaces;
            pool_hold = surfaces - for_decoder;
            log::info("D3D11VA surface pool: {} surfaces of {}x{} ({} for the decoder, {} for presentation){}",
                      surfaces, frames->width, frames->height, for_decoder, pool_hold,
                      shader_readable ? "" : "; textures are not shader-readable, the presenter copies");
            return true;
        }
        return false;
    }

    AVPixelFormat video_decoder::state::choose_format(AVCodecContext *codec_context, AVPixelFormat const *formats)
    {
        auto *const owner = static_cast<state *>(codec_context->opaque);
        AVPixelFormat software_choice = AV_PIX_FMT_NONE;
        for (AVPixelFormat const *candidate = formats; *candidate != AV_PIX_FMT_NONE; ++candidate)
        {
            if (*candidate == AV_PIX_FMT_D3D11)
            {
                if (owner != nullptr && codec_context->hw_device_ctx != nullptr && owner->create_pool(codec_context))
                {
                    return AV_PIX_FMT_D3D11;
                }
                continue;
            }
            AVPixFmtDescriptor const *descriptor = av_pix_fmt_desc_get(*candidate);
            if (software_choice == AV_PIX_FMT_NONE && descriptor != nullptr &&
                (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0)
            {
                software_choice = *candidate;
            }
        }
        // Either the pool could not be created, or libavcodec asks again after
        // the D3D11VA decoder itself failed to initialize.
        if (owner != nullptr)
        {
            owner->software_format = true;
        }
        return software_choice;
    }

    void video_decoder::state::note_pool_exhausted()
    {
        if (full_size_pool)
        {
            return;
        }
        full_size_pool = true;
        log::warning("D3D11VA surface pool ran out ({} surfaces, {} pictures alive outside the decoder); the next "
                     "pool of this decoder gets libavcodec's full size",
                     pool_surfaces, pictures_out->load(std::memory_order_relaxed));
    }

    void video_decoder::state::end_start_up()
    {
        starting = false;
        replay.clear();
        replay.shrink_to_fit();
        replay_bytes = 0;
        key_picture_seen = false;
        units_since_key_picture = 0;
    }

    void video_decoder::state::keep_for_replay(unit_summary const &summary, std::span<std::uint8_t const> access_unit,
                                               std::int64_t pts_ns)
    {
        if (summary.parameter_sets)
        {
            // decoding can start from this unit: earlier ones are not needed
            replay.clear();
            replay_bytes = 0;
            key_picture_seen = false;
            units_since_key_picture = 0;
        }
        if (summary.key_picture)
        {
            key_picture_seen = true;
            units_since_key_picture = 0;
        }
        else if (key_picture_seen)
        {
            ++units_since_key_picture;
        }
        if (replay.size() >= most_replay_units || replay_bytes + access_unit.size() > most_replay_bytes)
        {
            log::warning("video decoder: {} units without a picture since hardware decoding started; a software "
                         "replay is no longer possible",
                         replay.size());
            end_start_up();
            return;
        }
        replay.push_back(replay_unit{std::vector<std::uint8_t>(access_unit.begin(), access_unit.end()), pts_ns});
        replay_bytes += access_unit.size();
    }

    void video_decoder::state::receive_pictures(unsigned int flags, std::int64_t fallback_pts_ns,
                                                std::vector<timed_frame> &produced, std::optional<failure> &problem)
    {
        int failed_results = 0;
        for (;;)
        {
            AVFrame *frame = av_frame_alloc();
            if (frame == nullptr)
            {
                if (!problem)
                {
                    problem = make_failure("av_frame_alloc", AVERROR(ENOMEM), "out of memory");
                }
                return;
            }
            int const result = avcodec_receive_frame_flags(context, frame, flags);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            {
                av_frame_free(&frame);
                return;
            }
            if (result < 0)
            {
                av_frame_free(&frame);
                ++rejected;
                if (hardware && result == AVERROR(ENOMEM))
                {
                    note_pool_exhausted();
                }
                if (result != AVERROR_INVALIDDATA && !problem)
                {
                    problem = av_failure("avcodec_receive_frame", result);
                }
                // a failed result belongs to one unit: pictures of other units
                // may still be waiting (software frame threads)
                if (++failed_results >= most_failed_results_per_call)
                {
                    return;
                }
                continue;
            }

            std::int64_t const frame_pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp
                                                                                           : fallback_pts_ns;
            bool const from_pool = frame->format == AV_PIX_FMT_D3D11;
            if (from_pool && pool_hold > 0 && pictures_out->load(std::memory_order_relaxed) >= pool_hold)
            {
                // The consumer holds every surface set aside for it.  The
                // picture stays in the decoder as a reference; handing it out
                // as well would leave no surface for the next one.
                av_frame_free(&frame);
                if (auto const earlier = withheld_gate.pass(clock::now_ns()); earlier)
                {
                    log::warning("video decoder: {} pictures are still held for presentation, a decoded picture was "
                                 "not handed over ({} more since the last report)",
                                 pool_hold, *earlier);
                }
                continue;
            }
            auto picture = decoded_frame::create(frame, from_pool ? pictures_out : nullptr);
            if (!picture)
            {
                ++rejected;
                if (!problem)
                {
                    problem = picture.error();
                }
                continue;
            }
            produced.push_back(timed_frame{std::move(*picture), frame_pts});
        }
    }

    outcome<std::vector<timed_frame>> video_decoder::state::decode_unit(std::span<std::uint8_t const> access_unit,
                                                                        std::int64_t pts_ns, bool more_input_ready)
    {
        std::vector<timed_frame> produced;
        std::optional<failure> problem;
        // av_new_packet adds the zeroed AV_INPUT_BUFFER_PADDING_SIZE tail FFmpeg requires.
        int result = av_new_packet(packet, static_cast<int>(access_unit.size()));
        if (result < 0)
        {
            return std::unexpected(av_failure("av_new_packet", result));
        }
        std::memcpy(packet->data, access_unit.data(), access_unit.size());
        packet->pts = pts_ns;
        packet->dts = pts_ns;

        result = avcodec_send_packet(context, packet);
        if (result == AVERROR(EAGAIN))
        {
            // output of earlier units is still inside: take it, then the unit is accepted
            receive_pictures(0, pts_ns, produced, problem);
            result = avcodec_send_packet(context, packet);
        }
        av_packet_unref(packet);
        if (result < 0)
        {
            ++rejected;
            if (hardware && result == AVERROR(ENOMEM))
            {
                note_pool_exhausted();
            }
            if (result != AVERROR_INVALIDDATA && !problem)
            {
                problem = av_failure("avcodec_send_packet", result);
            }
        }

        // Synchronous: the picture of this unit is returned by this call even
        // with frame threads (which otherwise hold it until more units arrive).
        unsigned int const flags =
            more_input_ready ? 0U : static_cast<unsigned int>(AV_CODEC_RECEIVE_FRAME_FLAG_SYNCHRONOUS);
        receive_pictures(flags, pts_ns, produced, problem);

        if (problem)
        {
            if (produced.empty())
            {
                return std::unexpected(*problem);
            }
            // pictures and a failure in one call: the pictures go to the caller
            if (auto const earlier = problem_gate.pass(clock::now_ns()); earlier)
            {
                log::warning("video decode: {} ({} more since the last report); pictures decoded in the same call "
                             "are kept",
                             problem->describe(), *earlier);
            }
        }
        return produced;
    }

    outcome<std::vector<timed_frame>> video_decoder::state::restart_in_software(std::string_view reason)
    {
        log::warning("{} hardware decoding could not be used ({}); the stream is restarted with the software decoder",
                     codec_name(codec), reason);
        std::vector<replay_unit> const units = std::move(replay);
        if (status const reopened = open(false); !reopened)
        {
            return std::unexpected(reopened.error());
        }
        std::vector<timed_frame> produced;
        std::optional<failure> problem;
        for (replay_unit const &unit : units)
        {
            auto replayed = decode_unit(unit.bytes, unit.pts_ns, false);
            if (!replayed)
            {
                if (!problem)
                {
                    problem = replayed.error();
                }
                continue;
            }
            std::ranges::move(*replayed, std::back_inserter(produced));
        }
        if (problem)
        {
            if (produced.empty())
            {
                return std::unexpected(*problem);
            }
            log::warning("video decode during the software restart: {}", problem->describe());
        }
        return produced;
    }

    video_decoder::video_decoder(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    video_decoder::~video_decoder() = default;

    outcome<std::unique_ptr<video_decoder>> video_decoder::create(gpu_device &device, video_codec codec,
                                                                  bool allow_hardware)
    {
        auto created = std::make_unique<state>();
        created->gpu = &device;
        created->codec = codec;
        created->decoder = avcodec_find_decoder(codec == video_codec::h264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
        if (created->decoder == nullptr)
        {
            return std::unexpected(make_failure("avcodec_find_decoder", 0, "decoder not compiled into FFmpeg"));
        }
        created->packet = av_packet_alloc();
        if (created->packet == nullptr)
        {
            return std::unexpected(make_failure("av_packet_alloc", AVERROR(ENOMEM), "out of memory"));
        }
        if (status const opened = created->open(allow_hardware); !opened)
        {
            return std::unexpected(opened.error());
        }
        return std::unique_ptr<video_decoder>{new video_decoder{std::move(created)}};
    }

    video_codec video_decoder::codec() const
    {
        return impl->codec;
    }

    bool video_decoder::hardware() const
    {
        return impl->hardware && !impl->software_format && impl->context != nullptr &&
               impl->context->hw_frames_ctx != nullptr;
    }

    std::uint64_t video_decoder::rejected_units() const
    {
        return impl->rejected;
    }

    outcome<std::vector<timed_frame>> video_decoder::decode(std::span<std::uint8_t const> access_unit,
                                                            std::int64_t pts_ns, bool more_input_ready)
    {
        state &self = *impl;
        if (self.context == nullptr)
        {
            return std::unexpected(make_failure("video decoder", 0, "closed after a failed software restart"));
        }
        if (access_unit.empty())
        {
            return std::vector<timed_frame>{};
        }
        if (!self.hardware)
        {
            return self.decode_unit(access_unit, pts_ns, more_input_ready);
        }

        unit_summary const summary = summarize(self.codec, access_unit);
        if (summary.buffering)
        {
            self.announced_buffering = summary.buffering; // read by create_pool during this unit
        }
        bool const was_starting = self.starting;
        if (was_starting)
        {
            self.keep_for_replay(summary, access_unit, pts_ns);
        }
        auto produced = self.decode_unit(access_unit, pts_ns, false);

        if (!self.starting)
        {
            // Running (or start-up was given up).  A later stream change can
            // still leave D3D11VA unusable; the unit that carries it starts a
            // new sequence, so the software decoder can take over from it.
            if (!was_starting && self.software_format && summary.parameter_sets && summary.key_picture)
            {
                self.replay.clear();
                self.replay.push_back(
                    state::replay_unit{std::vector<std::uint8_t>(access_unit.begin(), access_unit.end()), pts_ns});
                return self.restart_in_software("D3D11VA could not be set up after a stream change");
            }
            return produced;
        }

        bool const hardware_picture = produced && std::ranges::any_of(*produced, [](timed_frame const &picture)
        {
            return picture.frame->memory() == frame_memory::d3d11_nv12;
        });
        if (hardware_picture && !self.software_format)
        {
            self.end_start_up();
            return produced;
        }
        if (self.software_format)
        {
            return self.restart_in_software("D3D11VA could not be set up for this stream");
        }
        if (!produced)
        {
            return self.restart_in_software(produced.error().describe());
        }
        if (self.key_picture_seen && self.units_since_key_picture >= std::max(self.context->has_b_frames, 0))
        {
            return self.restart_in_software("a key frame produced no picture");
        }
        return produced; // nothing decodable yet (no key picture so far)
    }
}
