// SPDX-License-Identifier: MIT
// AirPlay audio decoding to interleaved stereo float32 at the audio device
// rate.
//
// AirPlay mirroring sends AAC-ELD 44.1 kHz (480-sample frames), audio-only
// streaming sends ALAC 44.1 kHz (AAC-LC and LPCM are also accepted).
// Sample-rate conversion uses libswresample's windowed-sinc resampler; the
// resampler delay is reported so presentation times stay exact.
export module uxplay_audio_decoder;

import std;
import uxplay_status;

export namespace uxplay
{
    enum class audio_codec
    {
        pcm_s16le,
        alac,
        aac_lc,
        aac_eld
    };

    struct decoded_audio
    {
        std::vector<float> stereo_samples;
        // output frame 0 corresponds to (packet time - resampler_delay_ns)
        std::int64_t resampler_delay_ns = 0;
    };

    class audio_decoder
    {
    public:
        static outcome<std::unique_ptr<audio_decoder>> create(audio_codec codec, int input_rate, int input_channels,
                                                              int output_rate);

        ~audio_decoder();
        audio_decoder(audio_decoder const &) = delete;
        audio_decoder &operator=(audio_decoder const &) = delete;

        [[nodiscard]] audio_codec codec() const;
        [[nodiscard]] int output_rate() const;

        outcome<decoded_audio> decode(std::span<std::uint8_t const> packet);

    private:
        struct state;

        explicit audio_decoder(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
