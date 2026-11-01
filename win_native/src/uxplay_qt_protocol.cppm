// QuickTime screen-capture protocol spoken by iOS devices over the hidden
// USB configuration (the protocol macOS QuickTime / CoreMediaIO uses).
//
// Wire format, packet structure and the session handshake follow the
// reverse-engineered documentation of quicktime_video_hack (Daniel Paulus,
// MIT): https://github.com/danielpaulus/quicktime_video_hack
// This is an independent C++ implementation; HPD1/HPA1 dictionaries mirror
// the values a real macOS host sends (captured fixtures in tests/).
//
// The session is transport independent: it consumes complete packets and
// emits replies through a send callback, so it is unit-tested from fixtures.
export module uxplay_qt_protocol;

import std;
import uxplay_status;

export namespace uxplay::qt
{
    [[nodiscard]] constexpr std::uint32_t fourcc(char const (&text)[5])
    {
        return (static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) << 24) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 16) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 8) |
               static_cast<std::uint32_t>(static_cast<unsigned char>(text[3]));
    }

    struct cm_time
    {
        std::int64_t value = 0;
        std::int32_t scale = 0;
        std::uint32_t flags = 0;
        std::int64_t epoch = 0;

        [[nodiscard]] bool valid() const;
        [[nodiscard]] std::int64_t to_ns() const;
    };

    struct audio_stream_description
    {
        double sample_rate = 48000.0;
        std::uint32_t format_id = 0;
        std::uint32_t format_flags = 0;
        std::uint32_t bytes_per_packet = 0;
        std::uint32_t frames_per_packet = 0;
        std::uint32_t bytes_per_frame = 0;
        std::uint32_t channels = 0;
        std::uint32_t bits_per_channel = 0;
    };

    struct video_description
    {
        std::uint32_t codec = 0; // 'avc1' or 'hvc1'
        int width = 0;
        int height = 0;
        int nal_length_size = 4;
        std::vector<std::uint8_t> parameter_sets; // Annex-B SPS/PPS (+VPS)
    };

    struct sample_buffer
    {
        std::optional<cm_time> presentation_time;
        std::vector<std::uint8_t> data;
        int sample_count = 0;
        std::optional<video_description> video_format;
        std::optional<audio_stream_description> audio_format;
    };

    // Parses a CMSampleBuffer ("sbuf" atom including its length prefix).
    [[nodiscard]] outcome<sample_buffer> parse_sample_buffer(std::span<std::uint8_t const> bytes);

    // Parses a CMFormatDescription ("fdsc" atom including its length prefix).
    struct format_description
    {
        std::optional<video_description> video;
        std::optional<audio_stream_description> audio;
    };
    [[nodiscard]] outcome<format_description> parse_format_description(std::span<std::uint8_t const> bytes);

    // Length-prefixed NAL units (AVCC/HVCC) to Annex-B start codes.
    [[nodiscard]] outcome<std::vector<std::uint8_t>> length_prefixed_to_annex_b(std::span<std::uint8_t const> data,
                                                                                int length_size);

    // Splits the bulk-transfer byte stream into complete packets.
    class packet_reassembler
    {
    public:
        // Calls `sink` with every complete packet (length prefix included).
        status append(std::span<std::uint8_t const> data,
                      std::function<void(std::span<std::uint8_t const>)> const &sink);

    private:
        std::vector<std::uint8_t> pending;
    };

    struct session_handlers
    {
        std::function<void(std::vector<std::uint8_t> packet)> send;
        std::function<void(video_description const &format)> on_video_format;
        std::function<void(sample_buffer &&sample, std::int64_t arrival_ns)> on_video_sample;
        std::function<void(audio_stream_description const &format)> on_audio_format;
        std::function<void(sample_buffer &&sample, std::int64_t arrival_ns)> on_audio_sample;
    };

    struct host_display
    {
        int width = 1920;
        int height = 1200;
    };

    class session
    {
    public:
        session(session_handlers handlers, host_display display);
        ~session();
        session(session const &) = delete;
        session &operator=(session const &) = delete;

        // One complete packet (length prefix included), as produced by
        // packet_reassembler.  Unknown packets are reported, not fatal.
        status handle_packet(std::span<std::uint8_t const> packet);

        // Asks the device to stop audio and video (HPA0 + HPD0).
        void request_stop();

        // Number of RELS (clock released) notifications received since start.
        [[nodiscard]] int released_clocks() const;
        [[nodiscard]] bool streaming() const;

    private:
        struct state;

        std::unique_ptr<state> impl;
    };
}
