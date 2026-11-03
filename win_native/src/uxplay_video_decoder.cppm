// SPDX-License-Identifier: MIT
// H.264 / H.265 decoding with FFmpeg's D3D11VA hwaccel on the display GPU.
//
// Low-latency configuration: AV_CODEC_FLAG_LOW_DELAY and no frame threading,
// so every access unit produces its picture immediately (mirroring streams
// carry no B-frame reordering).  Decoder textures are created with
// D3D11_BIND_SHADER_RESOURCE so the presenter samples them in place.
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

export module uxplay_video_decoder;

import std;
import uxplay_status;
import uxplay_gpu_device;
import uxplay_video_frame;

export namespace uxplay
{
    enum class video_codec
    {
        h264,
        h265
    };

    struct timed_frame
    {
        std::unique_ptr<decoded_frame> frame;
        std::int64_t pts_ns = 0;
    };

    class video_decoder
    {
    public:
        // allow_hardware = false forces the multi-slice software decoder.
        static outcome<std::unique_ptr<video_decoder>> create(gpu_device &device, video_codec codec,
                                                              bool allow_hardware);

        ~video_decoder();
        video_decoder(video_decoder const &) = delete;
        video_decoder &operator=(video_decoder const &) = delete;

        [[nodiscard]] video_codec codec() const;
        [[nodiscard]] bool hardware() const;

        // Decodes one Annex-B access unit; produced pictures carry `pts_ns`.
        // Corrupt units (e.g. P-frames before the first IDR) yield no frames
        // and are reported through the returned count of rejected units.
        outcome<std::vector<timed_frame>> decode(std::span<std::uint8_t const> access_unit, std::int64_t pts_ns);

        [[nodiscard]] std::uint64_t rejected_units() const;

    private:
        struct state;

        explicit video_decoder(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
