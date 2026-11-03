// SPDX-License-Identifier: MIT
// Media engine smoke test without a phone: plays an Annex-B elementary stream
// (with access-unit delimiters) at its frame rate as a fake mirroring source,
// plus a 440 Hz tone with matching timestamps, then reports what was decoded
// and presented.  Exit code 0 when every access unit produced a picture and
// the presenter showed frames.
//
// usage: engine_smoke_test <stream.264|stream.265> <h264|h265> <fps> [auto|intel|nvidia] [quiet|loud]
//                          [zh-TW|zh-CN|en]   (interface language of the window and control bar)
import std;
import uxplay_log;
import uxplay_clock;
import uxplay_media_types;
import uxplay_media_engine;
import uxplay_video_decoder;
import uxplay_audio_decoder;
import uxplay_gpu_device;
import uxplay_ui_text;

namespace
{
    // Splits at access unit delimiters (H.264 NAL type 9, HEVC NAL type 35).
    std::vector<std::vector<std::uint8_t>> split_access_units(std::vector<std::uint8_t> const &stream, bool hevc)
    {
        std::vector<std::size_t> starts;
        for (std::size_t index = 0; index + 4 < stream.size(); ++index)
        {
            if (stream[index] != 0 || stream[index + 1] != 0)
            {
                continue;
            }
            std::size_t header = 0;
            if (stream[index + 2] == 1)
            {
                header = index + 3;
            }
            else if (stream[index + 2] == 0 && stream[index + 3] == 1)
            {
                header = index + 4;
            }
            else
            {
                continue;
            }
            int const type = hevc ? (stream[header] >> 1) & 0x3f : stream[header] & 0x1f;
            if ((hevc && type == 35) || (!hevc && type == 9))
            {
                starts.push_back(index);
            }
            index = header;
        }
        std::vector<std::vector<std::uint8_t>> units;
        for (std::size_t unit = 0; unit < starts.size(); ++unit)
        {
            std::size_t const end = unit + 1 < starts.size() ? starts[unit + 1] : stream.size();
            units.emplace_back(stream.begin() + static_cast<std::ptrdiff_t>(starts[unit]),
                               stream.begin() + static_cast<std::ptrdiff_t>(end));
        }
        return units;
    }

    void print_line(int level, char const *message)
    {
        std::println("[{}] {}", level, message);
    }
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::println(std::cerr, "usage: engine_smoke_test <stream> <h264|h265> <fps> [auto|intel|nvidia] [quiet|loud] "
                                "[zh-TW|zh-CN|en]");
        return 2;
    }
    uxplay::log::install_sink(print_line, uxplay::log::level::info);
    std::string const path = argv[1];
    bool const hevc = std::string_view{argv[2]} == "h265";
    double const fps = std::stod(argv[3]);
    std::string_view const gpu_choice = argc > 4 ? std::string_view{argv[4]} : std::string_view{"auto"};
    uxplay::media_source const source = uxplay::media_source::airplay;
    // quiet: test tone at about -61 dBFS (inaudible) for runs on a machine someone is using
    bool const quiet = argc > 5 && std::string_view{argv[5]} == "quiet";
    double const tone_amplitude = quiet ? 30.0 : 3000.0;
    std::string_view const language = argc > 6 ? std::string_view{argv[6]} : std::string_view{"en"};
    uxplay::set_ui_language(language == "zh-TW"   ? uxplay::ui_language::traditional_chinese
                            : language == "zh-CN" ? uxplay::ui_language::simplified_chinese
                                                 : uxplay::ui_language::english);

    std::ifstream file{path, std::ios::binary};
    std::vector<std::uint8_t> const stream{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    std::vector<std::vector<std::uint8_t>> const units = split_access_units(stream, hevc);
    if (units.empty())
    {
        std::println(std::cerr, "no access units found in {}", path);
        return 2;
    }

    uxplay::engine_settings settings;
    settings.window_title = L"UxPlay engine smoke test";
    settings.gpu = gpu_choice == "intel"    ? uxplay::gpu_preference::integrated
                   : gpu_choice == "nvidia" ? uxplay::gpu_preference::discrete
                                            : uxplay::gpu_preference::automatic;
    std::int64_t const created_at = uxplay::clock::now_ns();
    auto engine = uxplay::media_engine::create(settings, uxplay::engine_callbacks{});
    if (!engine)
    {
        std::println(std::cerr, "engine: {}", engine.error().describe());
        return 1;
    }
    std::println("engine created in {:.1f} ms", static_cast<double>(uxplay::clock::now_ns() - created_at) / 1e6);

    (*engine)->video_configure(source, hevc ? uxplay::video_codec::h265 : uxplay::video_codec::h264, 0, 0);
    (*engine)->audio_configure(source, uxplay::audio_codec::pcm_s16le, 48000, 2, true, 0.0);

    auto const frame_period = static_cast<std::int64_t>(1e9 / fps);
    constexpr int audio_chunk_frames = 480; // 10 ms
    std::int64_t const start = uxplay::clock::now_ns() + 50 * uxplay::clock::ns_per_ms;
    std::int64_t audio_time = start;
    std::uint64_t audio_frames_sent = 0;
    uxplay::clock::precise_waiter waiter;
    for (std::size_t index = 0; index < units.size(); ++index)
    {
        std::int64_t const media_time = start + static_cast<std::int64_t>(index) * frame_period;
        waiter.sleep_until(media_time);
        (*engine)->video_access_unit(source, units[index], media_time, uxplay::clock::now_ns());
        // audio up to the current frame time, sent as it is "captured"
        while (audio_time <= media_time)
        {
            std::vector<std::uint8_t> pcm(audio_chunk_frames * 4);
            for (int frame = 0; frame < audio_chunk_frames; ++frame)
            {
                double const phase = 2.0 * std::numbers::pi * 440.0 * static_cast<double>(audio_frames_sent + frame) / 48000.0;
                auto const sample = static_cast<std::int16_t>(std::sin(phase) * tone_amplitude);
                for (int channel = 0; channel < 2; ++channel)
                {
                    pcm[static_cast<std::size_t>(frame) * 4 + static_cast<std::size_t>(channel) * 2] =
                        static_cast<std::uint8_t>(sample & 0xff);
                    pcm[static_cast<std::size_t>(frame) * 4 + static_cast<std::size_t>(channel) * 2 + 1] =
                        static_cast<std::uint8_t>((sample >> 8) & 0xff);
                }
            }
            (*engine)->audio_packet(source, pcm, audio_time, uxplay::clock::now_ns());
            audio_frames_sent += audio_chunk_frames;
            audio_time += static_cast<std::int64_t>(audio_chunk_frames) * uxplay::clock::ns_per_second / 48000;
        }
    }
    waiter.sleep_until(uxplay::clock::now_ns() + 500 * uxplay::clock::ns_per_ms);
    uxplay::source_statistics const stats = (*engine)->statistics(source);
    double const seconds = static_cast<double>(units.size()) / fps;
    std::println("units {} decoded {} ({}) presented {} dropped {} sync delay {:.1f} ms; "
                 "presented {:.1f} fps over {:.1f} s",
                 units.size(), stats.decoded, stats.hardware_decode ? "hardware" : "software", stats.presented,
                 stats.dropped, static_cast<double>(stats.sync_delay_ns) / 1e6,
                 static_cast<double>(stats.presented) / seconds, seconds);
    (*engine)->video_stop(source);
    (*engine)->audio_stop(source);
    bool const passed = stats.decoded == units.size() && stats.presented > units.size() / 2;
    std::println("{}", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
