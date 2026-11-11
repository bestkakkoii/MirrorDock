// SPDX-License-Identifier: MIT
// H.264 / H.265 decoding with FFmpeg's D3D11VA hwaccel on the display GPU.
//
// Low latency: every access unit yields its picture in the same call
// (mirroring streams carry no B-frame reordering and stop sending while the
// screen is still, so nothing may wait inside the decoder for a later unit).
//
// Hardware: decoder textures are created with D3D11_BIND_SHADER_RESOURCE so
// the presenter samples them in place.  libavcodec's D3D11VA decoder needs one
// fixed texture array; it is sized for what the stream can reference (H.264:
// the SPS reference count libavcodec reports; H.265: sps_max_dec_pic_buffering
// read from the SPS and cross-checked against libavcodec's size and level;
// otherwise libavcodec's worst case of 17) plus the pictures the presenter may
// hold (gpu_device::presentation_hold).  The decoder counts the pictures that
// are alive outside and withholds a new one rather than exhaust the array.
//
// Start-up: until the first hardware picture the decoder keeps the units it
// was given.  If D3D11VA cannot be set up for the stream, fails on it, or
// yields nothing for a key picture, the decoder reopens itself as a software
// decoder and replays those units, so its owner never has to recreate it in
// the middle of a stream that has no periodic key frames.
//
// Software: frame threads whose pictures are received synchronously
// (AV_CODEC_RECEIVE_FRAME_FLAG_SYNCHRONOUS), so they add no delay.  A caller
// that already holds the next unit says so (more_input_ready): the units are
// then decoded in parallel and their pictures come with a later call.
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
        // allow_hardware = false opens the software decoder at once.
        static outcome<std::unique_ptr<video_decoder>> create(gpu_device &device, video_codec codec,
                                                              bool allow_hardware);

        ~video_decoder();
        video_decoder(video_decoder const &) = delete;
        video_decoder &operator=(video_decoder const &) = delete;

        [[nodiscard]] video_codec codec() const;

        // true while pictures come from D3D11VA (false before the first
        // hardware picture and after a fall-back to software)
        [[nodiscard]] bool hardware() const;

        // Decodes one Annex-B access unit; produced pictures carry the pts of
        // the unit they belong to.  Corrupt units (e.g. P-frames before the
        // first IDR) yield no frames and are counted in rejected_units().
        // A picture in a layout that cannot be shown is a failure naming it.
        //
        // more_input_ready: the caller already holds the next unit and will
        // pass it right after this call.  The software decoder then does not
        // wait for this unit's picture (later calls deliver it); the last call
        // of such a run, with false, returns everything still pending.
        outcome<std::vector<timed_frame>> decode(std::span<std::uint8_t const> access_unit, std::int64_t pts_ns,
                                                 bool more_input_ready = false);

        [[nodiscard]] std::uint64_t rejected_units() const;

    private:
        struct state;

        explicit video_decoder(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
