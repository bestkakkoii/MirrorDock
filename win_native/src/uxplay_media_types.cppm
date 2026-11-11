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

    // What the receiver optimizes for (user setting; numeric values are stored
    // in the settings and passed through the C API):
    // quality     - the sharpest picture: full monitor resolution, high-quality
    //               scaling, a little more buffering so no frame is skipped
    // balanced    - adapts to the monitor, the GPU and the network (default)
    // performance - the lowest delay and load: a lighter stream, minimal
    //               buffering, late frames are skipped
    enum class performance_mode : int
    {
        quality = 0,
        balanced = 1,
        performance = 2
    };

    [[nodiscard]] constexpr std::string_view performance_mode_name(performance_mode mode)
    {
        switch (mode)
        {
        case performance_mode::quality:
            return "quality";
        case performance_mode::performance:
            return "performance";
        case performance_mode::balanced:
            break;
        }
        return "balanced";
    }

    // Which part of the picture the window shows (user setting; numeric
    // values are stored in the settings):
    // off       - the whole picture, as the phone sends it
    // automatic - black bars around the content are cut off (a film or a game
    //             that does not fill the phone's screen), see uxplay_crop_detector
    // manual    - the part the user framed with the mouse (not stored: it
    //             belongs to the stream it was drawn on)
    enum class crop_mode : int
    {
        off = 0,
        automatic = 1,
        manual = 2
    };

    [[nodiscard]] constexpr std::string_view crop_mode_name(crop_mode mode)
    {
        switch (mode)
        {
        case crop_mode::automatic:
            return "automatic";
        case crop_mode::manual:
            return "manual";
        case crop_mode::off:
            break;
        }
        return "off";
    }

    // A rectangle inside a decoded picture, in its pixels.  An empty region
    // (no width or no height) stands for the whole picture.
    struct picture_region
    {
        int left = 0;
        int top = 0;
        int width = 0;
        int height = 0;

        [[nodiscard]] constexpr bool empty() const
        {
            return width <= 0 || height <= 0;
        }

        friend constexpr bool operator==(picture_region const &, picture_region const &) = default;
    };

    // A rectangle as fractions (0..1) of some reference rectangle, e.g. the
    // part of the visible picture the user framed.
    struct fraction_region
    {
        double left = 0.0;
        double top = 0.0;
        double right = 1.0;
        double bottom = 1.0;
    };

    // Read-only view of a picture's luma plane on the CPU: the 8 most
    // significant bits of the sample at column x, row y are
    // data[y * row_stride + x * sample_stride].  Valid only inside the call
    // that hands it out.
    struct luma_view
    {
        std::uint8_t const *data = nullptr;
        int width = 0;
        int height = 0;
        std::ptrdiff_t row_stride = 0;
        std::ptrdiff_t sample_stride = 1;
    };
}
