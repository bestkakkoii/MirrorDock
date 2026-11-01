// Unit tests of the QuickTime USB protocol implementation against packets
// captured from a real macOS QuickTime session (quicktime_video_hack
// fixtures, MIT licence, see qt_fixtures/LICENSE-quicktime_video_hack-MIT.txt).
//
// usage: qt_protocol_tests <fixture directory>
import std;
import uxplay_log;
import uxplay_qt_protocol;

namespace
{
    int failures = 0;

    void check(bool condition, std::string_view what)
    {
        if (!condition)
        {
            ++failures;
            std::println("FAIL: {}", what);
        }
        else
        {
            std::println("ok:   {}", what);
        }
    }

    std::vector<std::uint8_t> load(std::filesystem::path const &directory, std::string_view name)
    {
        std::ifstream file{directory / name, std::ios::binary};
        return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    }

    std::uint32_t read_u32(std::span<std::uint8_t const> data, std::size_t offset)
    {
        return static_cast<std::uint32_t>(data[offset]) | (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
               (static_cast<std::uint32_t>(data[offset + 2]) << 16) | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
    }

    std::uint64_t read_u64(std::span<std::uint8_t const> data, std::size_t offset)
    {
        return static_cast<std::uint64_t>(read_u32(data, offset)) | (static_cast<std::uint64_t>(read_u32(data, offset + 4)) << 32);
    }

    void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            out.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_u64(std::vector<std::uint8_t> &out, std::uint64_t value)
    {
        put_u32(out, static_cast<std::uint32_t>(value));
        put_u32(out, static_cast<std::uint32_t>(value >> 32));
    }

    std::vector<std::uint8_t> with_length_prefix(std::vector<std::uint8_t> const &body)
    {
        std::vector<std::uint8_t> packet;
        put_u32(packet, static_cast<std::uint32_t>(body.size() + 4));
        packet.insert(packet.end(), body.begin(), body.end());
        return packet;
    }

    void ignore_log(int, char const *)
    {
    }

    struct capture
    {
        std::vector<std::vector<std::uint8_t>> sent;
        std::vector<uxplay::qt::sample_buffer> video;
        std::vector<uxplay::qt::sample_buffer> audio;
        std::optional<uxplay::qt::video_description> video_format;
        std::optional<uxplay::qt::audio_stream_description> audio_format;

        uxplay::qt::session_handlers handlers()
        {
            uxplay::qt::session_handlers result;
            result.send = [this](std::vector<std::uint8_t> packet)
            {
                sent.push_back(std::move(packet));
            };
            result.on_video_format = [this](uxplay::qt::video_description const &format)
            {
                video_format = format;
            };
            result.on_video_sample = [this](uxplay::qt::sample_buffer &&sample, std::int64_t)
            {
                video.push_back(std::move(sample));
            };
            result.on_audio_format = [this](uxplay::qt::audio_stream_description const &format)
            {
                audio_format = format;
            };
            result.on_audio_sample = [this](uxplay::qt::sample_buffer &&sample, std::int64_t)
            {
                audio.push_back(std::move(sample));
            };
            return result;
        }
    };
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::println(std::cerr, "usage: qt_protocol_tests <fixture directory>");
        return 2;
    }
    uxplay::log::install_sink(ignore_log, uxplay::log::level::error);
    std::filesystem::path const fixtures{argv[1]};

    // --- reassembly of arbitrarily split bulk transfers
    {
        std::vector<std::uint8_t> stream;
        for (std::string_view name : {"cwpa-request1", "afmt-request", "cvrp-request", "clok-request", "time-request1"})
        {
            std::vector<std::uint8_t> const packet = load(fixtures, name);
            stream.insert(stream.end(), packet.begin(), packet.end());
        }
        uxplay::qt::packet_reassembler reassembler;
        int packets = 0;
        std::size_t total = 0;
        for (std::size_t offset = 0; offset < stream.size(); offset += 37)
        {
            std::size_t const length = std::min<std::size_t>(37, stream.size() - offset);
            auto const appended = reassembler.append(std::span{stream}.subspan(offset, length),
                                                     [&packets, &total](std::span<std::uint8_t const> packet)
                                                     {
                                                         ++packets;
                                                         total += packet.size();
                                                     });
            check(appended.has_value(), "reassembler accepts chunk");
            if (!appended)
            {
                break;
            }
        }
        check(packets == 5 && total == stream.size(), "reassembler yields the 5 original packets");
    }

    // --- CWPA handshake: HPD1/HPA1 must equal what macOS QuickTime sends
    {
        capture captured;
        uxplay::qt::session session{captured.handlers(), uxplay::qt::host_display{1920, 1200}};
        std::vector<std::uint8_t> const expected_hpa1 = load(fixtures, "asyn-hpa1");
        std::uint64_t const device_audio_clock = read_u64(expected_hpa1, 8);
        std::vector<std::uint8_t> cwpa;
        put_u32(cwpa, 36);
        put_u32(cwpa, uxplay::qt::fourcc("sync"));
        put_u64(cwpa, 1);
        put_u32(cwpa, uxplay::qt::fourcc("cwpa"));
        put_u64(cwpa, 0x113573de0ULL);
        put_u64(cwpa, device_audio_clock);
        check(session.handle_packet(cwpa).has_value(), "cwpa handled");
        check(captured.sent.size() == 4, "cwpa answered with HPD1, HPD1, RPLY, HPA1");
        if (captured.sent.size() == 4)
        {
            check(captured.sent[0] == load(fixtures, "asyn-hpd1"), "HPD1 is byte-identical to macOS QuickTime");
            check(captured.sent[3] == expected_hpa1, "HPA1 is byte-identical to macOS QuickTime");
            std::vector<std::uint8_t> const &reply = captured.sent[2];
            check(reply.size() == 28 && read_u32(reply, 4) == uxplay::qt::fourcc("rply") &&
                      read_u64(reply, 8) == 0x113573de0ULL && read_u64(reply, 20) == device_audio_clock + 1000,
                  "CWPA reply carries correlation id and local audio clock");
        }
    }

    // --- AFMT / OG / STOP replies equal the macOS replies
    for (auto const &[request_name, reply_name] : {std::pair{"afmt-request", "afmt-reply"},
                                                   std::pair{"og-request", "og-reply"},
                                                   std::pair{"stop-request", "stop-reply"}})
    {
        capture captured;
        uxplay::qt::session session{captured.handlers(), uxplay::qt::host_display{}};
        check(session.handle_packet(load(fixtures, request_name)).has_value(), std::format("{} handled", request_name));
        check(captured.sent.size() == 1 && captured.sent[0] == load(fixtures, reply_name),
              std::format("{} reply is byte-identical to macOS", request_name));
        if (std::string_view{request_name} == "afmt-request")
        {
            check(captured.audio_format && captured.audio_format->sample_rate == 48000.0 &&
                      captured.audio_format->channels == 2 && captured.audio_format->bits_per_channel == 16,
                  "AFMT parsed as LPCM 48 kHz 16-bit stereo");
        }
    }

    // --- CVRP: video format with parameter sets, NEED + reply
    {
        capture captured;
        uxplay::qt::session session{captured.handlers(), uxplay::qt::host_display{}};
        std::vector<std::uint8_t> const cvrp = load(fixtures, "cvrp-request");
        check(session.handle_packet(cvrp).has_value(), "cvrp handled");
        check(captured.video_format.has_value(), "cvrp yields a video format");
        if (captured.video_format)
        {
            auto const &format = *captured.video_format;
            check(format.codec == uxplay::qt::fourcc("avc1") && format.width > 0 && format.height > 0,
                  std::format("cvrp format avc1 {}x{}", format.width, format.height));
            check(format.parameter_sets.size() > 8 && format.parameter_sets[0] == 0 && format.parameter_sets[3] == 1 &&
                      (format.parameter_sets[4] & 0x1f) == 7,
                  "parameter sets start with an Annex-B SPS");
        }
        check(captured.sent.size() == 2 && read_u32(captured.sent[0], 16) == uxplay::qt::fourcc("need") &&
                  read_u64(captured.sent[0], 8) == read_u64(cvrp, 28),
              "NEED uses the device video clock from CVRP");
        check(captured.sent.size() == 2 && read_u64(captured.sent[1], 8) == read_u64(cvrp, 20),
              "CVRP reply carries the correlation id");

        // --- FEED: sample becomes Annex-B with parameter sets prepended
        check(session.handle_packet(load(fixtures, "asyn-feed")).has_value(), "feed handled");
        check(captured.video.size() == 1, "feed delivered one video sample");
        if (!captured.video.empty())
        {
            auto const &sample = captured.video.front();
            check(sample.presentation_time && sample.presentation_time->valid(), "feed has a valid presentation time");
            check(sample.data.size() > 100 && sample.data[0] == 0 && sample.data[1] == 0 && sample.data[2] == 0 &&
                      sample.data[3] == 1 && (sample.data[4] & 0x1f) == 7,
                  "first feed sample is Annex-B starting with the SPS");
        }
        check(captured.sent.size() == 3 && read_u32(captured.sent[2], 16) == uxplay::qt::fourcc("need"),
              "feed is acknowledged with NEED");
        check(session.handle_packet(with_length_prefix(load(fixtures, "asyn-feed-nofdsc"))).has_value(), "feed without fdsc handled");
        check(captured.video.size() == 2 && captured.video[1].data.size() > 4 && (captured.video[1].data[4] & 0x1f) != 7,
              "later feed samples carry no parameter sets");
    }

    // --- EAT: audio sample buffer
    {
        capture captured;
        uxplay::qt::session session{captured.handlers(), uxplay::qt::host_display{}};
        check(session.handle_packet(with_length_prefix(load(fixtures, "asyn-eat"))).has_value(), "eat handled");
        check(captured.audio.size() == 1, "eat delivered one audio sample");
        if (!captured.audio.empty())
        {
            auto const &sample = captured.audio.front();
            check(sample.presentation_time && sample.presentation_time->scale == 48000,
                  "eat presentation time is on the 48 kHz sample clock");
            check(!sample.data.empty() && sample.data.size() % 4 == 0, "eat carries whole 16-bit stereo frames");
        }
        // SKEW before enough audio: nominal rate
        std::vector<std::uint8_t> const skew_request = load(fixtures, "skew-request");
        check(session.handle_packet(skew_request).has_value(), "skew handled");
        check(captured.sent.size() == 1 && captured.sent[0].size() == 28 &&
                  std::bit_cast<double>(read_u64(captured.sent[0], 20)) == 48000.0,
              "skew reply reports 48000 when the clocks cannot be compared yet");
    }

    // --- TIME / CLOK
    {
        capture captured;
        uxplay::qt::session session{captured.handlers(), uxplay::qt::host_display{}};
        std::vector<std::uint8_t> const clok = load(fixtures, "clok-request");
        check(session.handle_packet(clok).has_value(), "clok handled");
        check(captured.sent.size() == 1 && read_u64(captured.sent[0], 20) == read_u64(clok, 8) + 0x10000,
              "clok reply creates the derived clock reference");
        check(session.handle_packet(load(fixtures, "time-request1")).has_value(), "time handled");
        check(captured.sent.size() == 2 && captured.sent[1].size() == 44 && read_u32(captured.sent[1], 28) == 1'000'000'000 &&
                  read_u32(captured.sent[1], 32) == 1,
              "time reply is a valid nanosecond CMTime");
    }

    // --- AVCC -> Annex-B conversion rejects truncated input
    {
        std::vector<std::uint8_t> const broken{0, 0, 0, 9, 0x65, 1, 2};
        check(!uxplay::qt::length_prefixed_to_annex_b(broken, 4).has_value(), "truncated NAL is rejected");
    }

    std::println("{} failure(s)", failures);
    return failures == 0 ? 0 : 1;
}
