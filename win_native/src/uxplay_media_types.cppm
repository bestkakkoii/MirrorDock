// Media source identities shared by the presenter, audio output and engine.
// Lower enumerator value = higher priority: USB (QuickTime capture over the
// cable) always wins over Wi-Fi AirPlay when both deliver media.
export module uxplay_media_types;

import std;

export namespace uxplay
{
    enum class media_source : int
    {
        usb = 0,
        airplay = 1
    };

    inline constexpr std::size_t media_source_count = 2;

    [[nodiscard]] constexpr std::size_t source_index(media_source source)
    {
        return static_cast<std::size_t>(source);
    }

    [[nodiscard]] constexpr std::string_view media_source_name(media_source source)
    {
        return source == media_source::usb ? std::string_view{"USB"} : std::string_view{"Wi-Fi AirPlay"};
    }

    inline constexpr std::array<media_source, media_source_count> sources_by_priority{media_source::usb,
                                                                                       media_source::airplay};
}
