// SPDX-License-Identifier: MIT
// Media source identities shared by the presenter, audio output and engine.
// AirPlay is the only source (over Wi-Fi or the iPhone USB link); the
// per-source arrays and the priority list keep the arbitration code general.
export module uxplay_media_types;

import std;

export namespace uxplay
{
    enum class media_source : int
    {
        airplay = 0
    };

    inline constexpr std::size_t media_source_count = 1;

    [[nodiscard]] constexpr std::size_t source_index(media_source source)
    {
        return static_cast<std::size_t>(source);
    }

    [[nodiscard]] constexpr std::string_view media_source_name(media_source source)
    {
        return source == media_source::airplay ? std::string_view{"AirPlay"} : std::string_view{"unknown"};
    }

    inline constexpr std::array<media_source, media_source_count> sources_by_priority{media_source::airplay};
}
