module uxplay_qt_protocol;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_clock;

namespace uxplay::qt
{
    namespace
    {
        constexpr std::uint32_t magic_ping = fourcc("ping");
        constexpr std::uint32_t magic_sync = fourcc("sync");
        constexpr std::uint32_t magic_asyn = fourcc("asyn");
        constexpr std::uint32_t magic_rply = fourcc("rply");

        constexpr std::uint32_t sync_cwpa = fourcc("cwpa");
        constexpr std::uint32_t sync_afmt = fourcc("afmt");
        constexpr std::uint32_t sync_cvrp = fourcc("cvrp");
        constexpr std::uint32_t sync_clok = fourcc("clok");
        constexpr std::uint32_t sync_time = fourcc("time");
        constexpr std::uint32_t sync_skew = fourcc("skew");
        constexpr std::uint32_t sync_og = fourcc("go! ");
        constexpr std::uint32_t sync_stop = fourcc("stop");

        constexpr std::uint32_t asyn_feed = fourcc("feed");
        constexpr std::uint32_t asyn_eat = fourcc("eat!");
        constexpr std::uint32_t asyn_sprp = fourcc("sprp");
        constexpr std::uint32_t asyn_srat = fourcc("srat");
        constexpr std::uint32_t asyn_tbas = fourcc("tbas");
        constexpr std::uint32_t asyn_tjmp = fourcc("tjmp");
        constexpr std::uint32_t asyn_rels = fourcc("rels");
        constexpr std::uint32_t asyn_hpd1 = fourcc("hpd1");
        constexpr std::uint32_t asyn_hpa1 = fourcc("hpa1");
        constexpr std::uint32_t asyn_need = fourcc("need");
        constexpr std::uint32_t asyn_hpd0 = fourcc("hpd0");
        constexpr std::uint32_t asyn_hpa0 = fourcc("hpa0");

        constexpr std::uint32_t atom_sbuf = fourcc("sbuf");
        constexpr std::uint32_t atom_opts = fourcc("opts");
        constexpr std::uint32_t atom_stia = fourcc("stia");
        constexpr std::uint32_t atom_sdat = fourcc("sdat");
        constexpr std::uint32_t atom_nsmp = fourcc("nsmp");
        constexpr std::uint32_t atom_fdsc = fourcc("fdsc");
        constexpr std::uint32_t atom_mdia = fourcc("mdia");
        constexpr std::uint32_t atom_vdim = fourcc("vdim");
        constexpr std::uint32_t atom_codc = fourcc("codc");
        constexpr std::uint32_t atom_extn = fourcc("extn");
        constexpr std::uint32_t atom_asbd = fourcc("asbd");
        constexpr std::uint32_t media_video = fourcc("vide");
        constexpr std::uint32_t media_sound = fourcc("soun");
        constexpr std::uint32_t codec_hvc1 = fourcc("hvc1");
        constexpr std::uint32_t codec_hev1 = fourcc("hev1");

        constexpr std::uint32_t dict_magic = fourcc("dict");
        constexpr std::uint32_t key_value_magic = fourcc("keyv");
        constexpr std::uint32_t string_key_magic = fourcc("strk");
        constexpr std::uint32_t bool_value_magic = fourcc("bulv");
        constexpr std::uint32_t string_value_magic = fourcc("strv");
        constexpr std::uint32_t data_value_magic = fourcc("datv");
        constexpr std::uint32_t number_value_magic = fourcc("nmbv");
        constexpr std::uint32_t lpcm_format = fourcc("lpcm");

        constexpr std::uint64_t empty_clock_ref = 1;
        constexpr std::size_t cm_time_size = 24;
        constexpr std::size_t timing_info_size = 3 * cm_time_size;
        constexpr std::uint32_t max_packet_size = 64U * 1024U * 1024U;

        std::uint32_t read_u32(std::span<std::uint8_t const> data, std::size_t offset)
        {
            return static_cast<std::uint32_t>(data[offset]) | (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
                   (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
                   (static_cast<std::uint32_t>(data[offset + 3]) << 24);
        }

        std::uint64_t read_u64(std::span<std::uint8_t const> data, std::size_t offset)
        {
            return static_cast<std::uint64_t>(read_u32(data, offset)) |
                   (static_cast<std::uint64_t>(read_u32(data, offset + 4)) << 32);
        }

        double read_f64(std::span<std::uint8_t const> data, std::size_t offset)
        {
            return std::bit_cast<double>(read_u64(data, offset));
        }

        std::uint16_t read_u16_be(std::span<std::uint8_t const> data, std::size_t offset)
        {
            return static_cast<std::uint16_t>((data[offset] << 8) | data[offset + 1]);
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

        void patch_u32(std::vector<std::uint8_t> &out, std::size_t offset, std::uint32_t value)
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                out[offset + static_cast<std::size_t>(shift / 8)] = static_cast<std::uint8_t>(value >> shift);
            }
        }

        cm_time read_cm_time(std::span<std::uint8_t const> data, std::size_t offset)
        {
            cm_time time;
            time.value = static_cast<std::int64_t>(read_u64(data, offset));
            time.scale = static_cast<std::int32_t>(read_u32(data, offset + 8));
            time.flags = read_u32(data, offset + 12);
            time.epoch = static_cast<std::int64_t>(read_u64(data, offset + 16));
            return time;
        }

        // ---- atom iteration -------------------------------------------------

        struct atom
        {
            std::uint32_t magic = 0;
            std::span<std::uint8_t const> whole;   // including header
            std::span<std::uint8_t const> payload; // after the 8-byte header
        };

        outcome<atom> read_atom(std::span<std::uint8_t const> data, std::size_t offset)
        {
            if (offset + 8 > data.size())
            {
                return std::unexpected(make_failure("qt atom header", static_cast<std::int64_t>(offset), "truncated"));
            }
            std::uint32_t const length = read_u32(data, offset);
            if (length < 8 || offset + length > data.size())
            {
                return std::unexpected(make_failure("qt atom length", length, "out of bounds"));
            }
            atom result;
            result.magic = read_u32(data, offset + 4);
            result.whole = data.subspan(offset, length);
            result.payload = result.whole.subspan(8);
            return result;
        }

        // Visits every child atom of `container` payload.
        status for_each_atom(std::span<std::uint8_t const> payload, std::function<status(atom const &)> const &visit)
        {
            std::size_t offset = 0;
            while (offset < payload.size())
            {
                auto child = read_atom(payload, offset);
                if (!child)
                {
                    return std::unexpected(child.error());
                }
                status const visited = visit(*child);
                if (!visited)
                {
                    return visited;
                }
                offset += child->whole.size();
            }
            return {};
        }

        // ---- dictionaries -----------------------------------------------------

        struct dict_value
        {
            std::uint32_t magic = 0;
            std::span<std::uint8_t const> whole;
            std::span<std::uint8_t const> payload;
        };

        // Collects all values (recursively through nested dicts) with their
        // string key (empty for index keys).
        status collect_dict_values(std::span<std::uint8_t const> dict_payload,
                                   std::vector<std::pair<std::string, dict_value>> &values, int depth)
        {
            if (depth > 8)
            {
                return std::unexpected(make_failure("qt dict", depth, "nesting too deep"));
            }
            return for_each_atom(dict_payload, [&values, depth](atom const &entry) -> status
            {
                if (entry.magic != key_value_magic)
                {
                    return {};
                }
                auto key = read_atom(entry.payload, 0);
                if (!key)
                {
                    return std::unexpected(key.error());
                }
                std::string key_text;
                if (key->magic == string_key_magic)
                {
                    key_text.assign(reinterpret_cast<char const *>(key->payload.data()), key->payload.size());
                }
                auto value = read_atom(entry.payload, key->whole.size());
                if (!value)
                {
                    return std::unexpected(value.error());
                }
                values.emplace_back(key_text, dict_value{value->magic, value->whole, value->payload});
                if (value->magic == dict_magic)
                {
                    return collect_dict_values(value->payload, values, depth + 1);
                }
                return {};
            });
        }

        class dict_writer
        {
        public:
            dict_writer()
            {
                start = bytes.size();
                put_u32(bytes, 0);
                put_u32(bytes, dict_magic);
            }

            void add_bool(std::string_view key, bool value)
            {
                std::size_t const entry = begin_entry(key);
                put_u32(bytes, 9);
                put_u32(bytes, bool_value_magic);
                bytes.push_back(value ? 1 : 0);
                end_entry(entry);
            }

            void add_f64(std::string_view key, double value)
            {
                std::size_t const entry = begin_entry(key);
                put_u32(bytes, 17);
                put_u32(bytes, number_value_magic);
                bytes.push_back(6);
                put_u64(bytes, std::bit_cast<std::uint64_t>(value));
                end_entry(entry);
            }

            void add_u32(std::string_view key, std::uint32_t value)
            {
                std::size_t const entry = begin_entry(key);
                put_u32(bytes, 13);
                put_u32(bytes, number_value_magic);
                bytes.push_back(3);
                put_u32(bytes, value);
                end_entry(entry);
            }

            void add_string(std::string_view key, std::string_view value)
            {
                std::size_t const entry = begin_entry(key);
                put_u32(bytes, static_cast<std::uint32_t>(8 + value.size()));
                put_u32(bytes, string_value_magic);
                bytes.insert(bytes.end(), value.begin(), value.end());
                end_entry(entry);
            }

            void add_data(std::string_view key, std::span<std::uint8_t const> value)
            {
                std::size_t const entry = begin_entry(key);
                put_u32(bytes, static_cast<std::uint32_t>(8 + value.size()));
                put_u32(bytes, data_value_magic);
                bytes.insert(bytes.end(), value.begin(), value.end());
                end_entry(entry);
            }

            void add_dict(std::string_view key, dict_writer nested)
            {
                std::size_t const entry = begin_entry(key);
                std::vector<std::uint8_t> const serialized = std::move(nested).finish();
                bytes.insert(bytes.end(), serialized.begin(), serialized.end());
                end_entry(entry);
            }

            [[nodiscard]] std::vector<std::uint8_t> finish() &&
            {
                patch_u32(bytes, start, static_cast<std::uint32_t>(bytes.size() - start));
                return std::move(bytes);
            }

        private:
            std::size_t begin_entry(std::string_view key)
            {
                std::size_t const entry = bytes.size();
                put_u32(bytes, 0);
                put_u32(bytes, key_value_magic);
                put_u32(bytes, static_cast<std::uint32_t>(8 + key.size()));
                put_u32(bytes, string_key_magic);
                bytes.insert(bytes.end(), key.begin(), key.end());
                return entry;
            }

            void end_entry(std::size_t entry)
            {
                patch_u32(bytes, entry, static_cast<std::uint32_t>(bytes.size() - entry));
            }

            std::vector<std::uint8_t> bytes;
            std::size_t start = 0;
        };

        // ---- codec configuration records ------------------------------------

        void append_start_code_nal(std::vector<std::uint8_t> &out, std::span<std::uint8_t const> nal)
        {
            constexpr std::array<std::uint8_t, 4> start_code{0, 0, 0, 1};
            out.insert(out.end(), start_code.begin(), start_code.end());
            out.insert(out.end(), nal.begin(), nal.end());
        }

        outcome<std::pair<std::vector<std::uint8_t>, int>> parse_avcc(std::span<std::uint8_t const> record)
        {
            if (record.size() < 7 || record[0] != 1)
            {
                return std::unexpected(make_failure("avcC", 0, "not an AVC decoder configuration record"));
            }
            int const length_size = (record[4] & 0x03) + 1;
            std::vector<std::uint8_t> sets;
            std::size_t offset = 6;
            int const sps_count = record[5] & 0x1f;
            for (int index = 0; index < sps_count; ++index)
            {
                if (offset + 2 > record.size())
                {
                    return std::unexpected(make_failure("avcC SPS", index, "truncated"));
                }
                std::size_t const length = read_u16_be(record, offset);
                offset += 2;
                if (offset + length > record.size())
                {
                    return std::unexpected(make_failure("avcC SPS", index, "truncated"));
                }
                append_start_code_nal(sets, record.subspan(offset, length));
                offset += length;
            }
            if (offset >= record.size())
            {
                return std::unexpected(make_failure("avcC PPS count", 0, "truncated"));
            }
            int const pps_count = record[offset++];
            for (int index = 0; index < pps_count; ++index)
            {
                if (offset + 2 > record.size())
                {
                    return std::unexpected(make_failure("avcC PPS", index, "truncated"));
                }
                std::size_t const length = read_u16_be(record, offset);
                offset += 2;
                if (offset + length > record.size())
                {
                    return std::unexpected(make_failure("avcC PPS", index, "truncated"));
                }
                append_start_code_nal(sets, record.subspan(offset, length));
                offset += length;
            }
            if (sets.empty())
            {
                return std::unexpected(make_failure("avcC", 0, "no parameter sets"));
            }
            return std::pair{std::move(sets), length_size};
        }

        outcome<std::pair<std::vector<std::uint8_t>, int>> parse_hvcc(std::span<std::uint8_t const> record)
        {
            if (record.size() < 23 || record[0] != 1)
            {
                return std::unexpected(make_failure("hvcC", 0, "not an HEVC decoder configuration record"));
            }
            int const length_size = (record[21] & 0x03) + 1;
            int const array_count = record[22];
            std::vector<std::uint8_t> sets;
            std::size_t offset = 23;
            for (int array = 0; array < array_count; ++array)
            {
                if (offset + 3 > record.size())
                {
                    return std::unexpected(make_failure("hvcC array", array, "truncated"));
                }
                int const nal_count = read_u16_be(record, offset + 1);
                offset += 3;
                for (int index = 0; index < nal_count; ++index)
                {
                    if (offset + 2 > record.size())
                    {
                        return std::unexpected(make_failure("hvcC NAL", index, "truncated"));
                    }
                    std::size_t const length = read_u16_be(record, offset);
                    offset += 2;
                    if (offset + length > record.size())
                    {
                        return std::unexpected(make_failure("hvcC NAL", index, "truncated"));
                    }
                    append_start_code_nal(sets, record.subspan(offset, length));
                    offset += length;
                }
            }
            if (sets.empty())
            {
                return std::unexpected(make_failure("hvcC", 0, "no parameter sets"));
            }
            return std::pair{std::move(sets), length_size};
        }

        std::vector<std::uint8_t> sync_reply_header(std::uint64_t correlation_id)
        {
            std::vector<std::uint8_t> packet;
            put_u32(packet, 0);
            put_u32(packet, magic_rply);
            put_u64(packet, correlation_id);
            put_u32(packet, 0);
            return packet;
        }

        void finish_packet(std::vector<std::uint8_t> &packet)
        {
            patch_u32(packet, 0, static_cast<std::uint32_t>(packet.size()));
        }

        std::vector<std::uint8_t> clock_reply(std::uint64_t correlation_id, std::uint64_t clock_ref)
        {
            std::vector<std::uint8_t> packet = sync_reply_header(correlation_id);
            put_u64(packet, clock_ref);
            finish_packet(packet);
            return packet;
        }

        std::vector<std::uint8_t> asyn_packet(std::uint64_t clock_ref, std::uint32_t subtype,
                                              std::span<std::uint8_t const> payload = {})
        {
            std::vector<std::uint8_t> packet;
            put_u32(packet, 0);
            put_u32(packet, magic_asyn);
            put_u64(packet, clock_ref);
            put_u32(packet, subtype);
            packet.insert(packet.end(), payload.begin(), payload.end());
            finish_packet(packet);
            return packet;
        }

        std::vector<std::uint8_t> ping_packet()
        {
            std::vector<std::uint8_t> packet;
            put_u32(packet, 16);
            put_u32(packet, magic_ping);
            put_u32(packet, 0);
            put_u32(packet, 1);
            return packet;
        }

        std::string fourcc_text(std::uint32_t value)
        {
            std::string text(4, ' ');
            for (int index = 0; index < 4; ++index)
            {
                char const character = static_cast<char>((value >> (24 - 8 * index)) & 0xff);
                text[static_cast<std::size_t>(index)] = (character >= 0x20 && character < 0x7f) ? character : '?';
            }
            return text;
        }
    }

    // ---- public parsing helpers ---------------------------------------------

    bool cm_time::valid() const
    {
        return (flags & 1U) != 0 && scale > 0;
    }

    std::int64_t cm_time::to_ns() const
    {
        std::int64_t const whole = value / scale;
        std::int64_t const remainder = value % scale;
        return whole * clock::ns_per_second + (remainder * clock::ns_per_second) / scale;
    }

    outcome<format_description> parse_format_description(std::span<std::uint8_t const> bytes)
    {
        auto container = read_atom(bytes, 0);
        if (!container || container->magic != atom_fdsc)
        {
            return std::unexpected(make_failure("fdsc", 0, "missing format description atom"));
        }
        std::uint32_t media = 0;
        video_description video;
        std::optional<audio_stream_description> audio;
        std::vector<std::span<std::uint8_t const>> configuration_candidates;

        status const parsed = for_each_atom(container->payload, [&](atom const &child) -> status
        {
            if (child.magic == atom_mdia && child.payload.size() >= 4)
            {
                media = read_u32(child.payload, 0);
            }
            else if (child.magic == atom_vdim && child.payload.size() >= 8)
            {
                video.width = static_cast<int>(read_u32(child.payload, 0));
                video.height = static_cast<int>(read_u32(child.payload, 4));
            }
            else if (child.magic == atom_codc && child.payload.size() >= 4)
            {
                video.codec = read_u32(child.payload, 0);
            }
            else if (child.magic == atom_extn)
            {
                std::vector<std::pair<std::string, dict_value>> values;
                status const collected = collect_dict_values(child.payload, values, 0);
                if (!collected)
                {
                    return collected;
                }
                for (auto const &[key, value] : values)
                {
                    if (value.magic == data_value_magic && value.payload.size() >= 7)
                    {
                        configuration_candidates.push_back(value.payload);
                    }
                }
            }
            else if (child.magic == atom_asbd && child.payload.size() >= 36)
            {
                audio_stream_description description;
                description.sample_rate = read_f64(child.payload, 0);
                description.format_id = read_u32(child.payload, 8);
                description.format_flags = read_u32(child.payload, 12);
                description.bytes_per_packet = read_u32(child.payload, 16);
                description.frames_per_packet = read_u32(child.payload, 20);
                description.bytes_per_frame = read_u32(child.payload, 24);
                description.channels = read_u32(child.payload, 28);
                description.bits_per_channel = read_u32(child.payload, 32);
                audio = description;
            }
            return {};
        });
        if (!parsed)
        {
            return std::unexpected(parsed.error());
        }

        format_description result;
        if (media == media_sound)
        {
            result.audio = audio;
            return result;
        }
        if (media != media_video)
        {
            return std::unexpected(make_failure("fdsc media type", media, fourcc_text(media)));
        }
        bool const hevc = video.codec == codec_hvc1 || video.codec == codec_hev1;
        for (std::span<std::uint8_t const> candidate : configuration_candidates)
        {
            auto configuration = hevc ? parse_hvcc(candidate) : parse_avcc(candidate);
            if (configuration)
            {
                video.parameter_sets = std::move(configuration->first);
                video.nal_length_size = configuration->second;
                break;
            }
        }
        result.video = std::move(video);
        return result;
    }

    outcome<sample_buffer> parse_sample_buffer(std::span<std::uint8_t const> bytes)
    {
        auto container = read_atom(bytes, 0);
        if (!container || container->magic != atom_sbuf)
        {
            return std::unexpected(make_failure("sbuf", 0, "missing sample buffer atom"));
        }
        sample_buffer sample;
        std::optional<cm_time> output_time;
        std::optional<cm_time> timing_time;
        status const parsed = for_each_atom(container->payload, [&](atom const &child) -> status
        {
            if (child.magic == atom_opts && child.payload.size() >= cm_time_size)
            {
                output_time = read_cm_time(child.payload, 0);
            }
            else if (child.magic == atom_stia && child.payload.size() >= timing_info_size)
            {
                timing_time = read_cm_time(child.payload, cm_time_size); // [duration, pts, dts]
            }
            else if (child.magic == atom_sdat)
            {
                sample.data.assign(child.payload.begin(), child.payload.end());
            }
            else if (child.magic == atom_nsmp && child.payload.size() >= 4)
            {
                sample.sample_count = static_cast<int>(read_u32(child.payload, 0));
            }
            else if (child.magic == atom_fdsc)
            {
                auto description = parse_format_description(child.whole);
                if (!description)
                {
                    return std::unexpected(description.error());
                }
                sample.video_format = std::move(description->video);
                sample.audio_format = description->audio;
            }
            // satt, sary, ssiz and unknown atoms carry nothing needed for playback
            return {};
        });
        if (!parsed)
        {
            return std::unexpected(parsed.error());
        }
        if (timing_time && timing_time->valid())
        {
            sample.presentation_time = timing_time;
        }
        else if (output_time && output_time->valid())
        {
            sample.presentation_time = output_time;
        }
        return sample;
    }

    outcome<std::vector<std::uint8_t>> length_prefixed_to_annex_b(std::span<std::uint8_t const> data, int length_size)
    {
        if (length_size < 1 || length_size > 4)
        {
            return std::unexpected(make_failure("NAL length size", length_size, "unsupported"));
        }
        std::vector<std::uint8_t> out;
        out.reserve(data.size() + 64);
        std::size_t offset = 0;
        while (offset < data.size())
        {
            if (offset + static_cast<std::size_t>(length_size) > data.size())
            {
                return std::unexpected(make_failure("NAL length", static_cast<std::int64_t>(offset), "truncated"));
            }
            std::size_t length = 0;
            for (int index = 0; index < length_size; ++index)
            {
                length = (length << 8) | data[offset + static_cast<std::size_t>(index)];
            }
            offset += static_cast<std::size_t>(length_size);
            if (length == 0 || offset + length > data.size())
            {
                return std::unexpected(make_failure("NAL unit", static_cast<std::int64_t>(length), "out of bounds"));
            }
            append_start_code_nal(out, data.subspan(offset, length));
            offset += length;
        }
        return out;
    }

    status packet_reassembler::append(std::span<std::uint8_t const> data,
                                      std::function<void(std::span<std::uint8_t const>)> const &sink)
    {
        pending.insert(pending.end(), data.begin(), data.end());
        std::size_t offset = 0;
        while (pending.size() - offset >= 4)
        {
            std::span<std::uint8_t const> const remaining{pending.data() + offset, pending.size() - offset};
            std::uint32_t const length = read_u32(remaining, 0);
            if (length < 8 || length > max_packet_size)
            {
                pending.clear();
                return std::unexpected(make_failure("qt packet length", length, "stream desynchronized"));
            }
            if (remaining.size() < length)
            {
                break;
            }
            sink(remaining.first(length));
            offset += length;
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(offset));
        return {};
    }

    // ---- session --------------------------------------------------------------

    struct session::state
    {
        session_handlers handlers;
        host_display display;
        std::uint64_t device_audio_clock = 0;
        std::uint64_t video_need_clock = 0;
        std::vector<std::uint8_t> need_packet;
        std::int64_t time_clock_start_ns = 0;
        std::optional<video_description> video_format;
        bool parameter_sets_pending = false;
        bool audio_started = false;
        std::int64_t first_audio_local_ns = 0;
        std::int64_t last_audio_local_ns = 0;
        std::optional<cm_time> first_audio_time;
        std::optional<cm_time> last_audio_time;
        int released = 0;
        bool is_streaming = false;
        std::uint64_t unknown_packets = 0;

        void send(std::vector<std::uint8_t> packet) const
        {
            if (handlers.send)
            {
                handlers.send(std::move(packet));
            }
        }

        [[nodiscard]] std::vector<std::uint8_t> hpd1() const;
        [[nodiscard]] std::vector<std::uint8_t> hpa1() const;
        void apply_video_format(video_description format);
        status handle_sync(std::span<std::uint8_t const> packet);
        status handle_asyn(std::span<std::uint8_t const> packet);
        [[nodiscard]] double skew() const;
    };

    std::vector<std::uint8_t> session::state::hpd1() const
    {
        dict_writer display_size;
        display_size.add_f64("Width", static_cast<double>(display.width));
        display_size.add_f64("Height", static_cast<double>(display.height));
        dict_writer root;
        root.add_bool("Valeria", true);
        root.add_bool("HEVCDecoderSupports444", true);
        root.add_dict("DisplaySize", std::move(display_size));
        std::vector<std::uint8_t> const payload = std::move(root).finish();
        return asyn_packet(empty_clock_ref, asyn_hpd1, payload);
    }

    std::vector<std::uint8_t> session::state::hpa1() const
    {
        // AudioStreamBasicDescription (LPCM 48 kHz, 16-bit, 2 ch) + rate range
        std::vector<std::uint8_t> formats;
        put_u64(formats, std::bit_cast<std::uint64_t>(48000.0));
        put_u32(formats, lpcm_format);
        put_u32(formats, 12); // signed integer | packed
        put_u32(formats, 4);
        put_u32(formats, 1);
        put_u32(formats, 4);
        put_u32(formats, 2);
        put_u32(formats, 16);
        put_u32(formats, 0);
        put_u64(formats, std::bit_cast<std::uint64_t>(48000.0));
        put_u64(formats, std::bit_cast<std::uint64_t>(48000.0));

        dict_writer root;
        // exactly the value macOS QuickTime sends (0.07300000000000001)
        root.add_f64("BufferAheadInterval", std::bit_cast<double>(std::uint64_t{0x3FB2B020C49BA5E4}));
        root.add_string("deviceUID", "Valeria");
        root.add_f64("ScreenLatency", 0.04);
        root.add_data("formats", formats);
        root.add_u32("EDIDAC3Support", 0);
        root.add_string("deviceName", "Valeria");
        std::vector<std::uint8_t> const payload = std::move(root).finish();
        return asyn_packet(device_audio_clock, asyn_hpa1, payload);
    }

    void session::state::apply_video_format(video_description format)
    {
        bool const changed = !video_format || video_format->width != format.width ||
                             video_format->height != format.height || video_format->codec != format.codec ||
                             video_format->parameter_sets != format.parameter_sets;
        if (!changed)
        {
            return;
        }
        video_format = std::move(format);
        parameter_sets_pending = true;
        log::info("USB video format: {} {}x{} ({} bytes of parameter sets)", fourcc_text(video_format->codec),
                  video_format->width, video_format->height, video_format->parameter_sets.size());
        if (handlers.on_video_format)
        {
            handlers.on_video_format(*video_format);
        }
    }

    double session::state::skew() const
    {
        if (!first_audio_time || !last_audio_time || first_audio_time->scale <= 0)
        {
            return 48000.0;
        }
        double const device_scale = static_cast<double>(first_audio_time->scale);
        double const device_elapsed =
            static_cast<double>(last_audio_time->value - first_audio_time->value) / device_scale;
        double const local_elapsed = static_cast<double>(last_audio_local_ns - first_audio_local_ns) / 1e9;
        if (device_elapsed <= 0.5 || local_elapsed <= 0.0)
        {
            return device_scale;
        }
        return device_scale * local_elapsed / device_elapsed;
    }

    status session::state::handle_sync(std::span<std::uint8_t const> packet)
    {
        if (packet.size() < 28)
        {
            return std::unexpected(make_failure("qt sync packet", static_cast<std::int64_t>(packet.size()), "too short"));
        }
        std::uint64_t const clock_ref = read_u64(packet, 8);
        std::uint32_t const subtype = read_u32(packet, 16);
        std::uint64_t const correlation = read_u64(packet, 20);

        switch (subtype)
        {
        case sync_cwpa:
        {
            if (packet.size() < 36)
            {
                return std::unexpected(make_failure("qt cwpa", static_cast<std::int64_t>(packet.size()), "too short"));
            }
            device_audio_clock = read_u64(packet, 28);
            std::uint64_t const local_audio_clock = device_audio_clock + 1000;
            std::vector<std::uint8_t> const device_info = hpd1();
            send(device_info);
            send(device_info);
            send(clock_reply(correlation, local_audio_clock));
            send(hpa1());
            log::debug("USB: cwpa device audio clock {:#x}", device_audio_clock);
            return {};
        }
        case sync_afmt:
        {
            if (packet.size() >= 28 + 36)
            {
                audio_stream_description description;
                std::span<std::uint8_t const> const payload = packet.subspan(28);
                description.sample_rate = read_f64(payload, 0);
                description.format_id = read_u32(payload, 8);
                description.format_flags = read_u32(payload, 12);
                description.bytes_per_packet = read_u32(payload, 16);
                description.frames_per_packet = read_u32(payload, 20);
                description.bytes_per_frame = read_u32(payload, 24);
                description.channels = read_u32(payload, 28);
                description.bits_per_channel = read_u32(payload, 32);
                log::info("USB audio format: {} {:.0f} Hz {} ch {} bit flags {:#x}", fourcc_text(description.format_id),
                          description.sample_rate, description.channels, description.bits_per_channel,
                          description.format_flags);
                if (handlers.on_audio_format)
                {
                    handlers.on_audio_format(description);
                }
            }
            dict_writer reply_dict;
            reply_dict.add_u32("Error", 0);
            std::vector<std::uint8_t> reply = sync_reply_header(correlation);
            std::vector<std::uint8_t> const dict_bytes = std::move(reply_dict).finish();
            reply.insert(reply.end(), dict_bytes.begin(), dict_bytes.end());
            finish_packet(reply);
            send(std::move(reply));
            return {};
        }
        case sync_cvrp:
        {
            if (packet.size() < 36)
            {
                return std::unexpected(make_failure("qt cvrp", static_cast<std::int64_t>(packet.size()), "too short"));
            }
            video_need_clock = read_u64(packet, 28);
            need_packet = asyn_packet(video_need_clock, asyn_need);
            if (packet.size() > 36)
            {
                auto dict_atom = read_atom(packet, 36);
                if (dict_atom && dict_atom->magic == dict_magic)
                {
                    std::vector<std::pair<std::string, dict_value>> values;
                    if (collect_dict_values(dict_atom->payload, values, 0))
                    {
                        for (auto const &[key, value] : values)
                        {
                            if (value.magic == atom_fdsc)
                            {
                                auto description = parse_format_description(value.whole);
                                if (description && description->video)
                                {
                                    apply_video_format(std::move(*description->video));
                                }
                                break;
                            }
                        }
                    }
                }
            }
            send(need_packet);
            send(clock_reply(correlation, video_need_clock + 0x1000AF));
            is_streaming = true;
            return {};
        }
        case sync_clok:
            time_clock_start_ns = clock::now_ns();
            send(clock_reply(correlation, clock_ref + 0x10000));
            return {};
        case sync_time:
        {
            std::vector<std::uint8_t> reply = sync_reply_header(correlation);
            put_u64(reply, static_cast<std::uint64_t>(clock::now_ns() - time_clock_start_ns));
            put_u32(reply, 1'000'000'000);
            put_u32(reply, 1); // kCMTimeFlags_Valid
            put_u64(reply, 0);
            finish_packet(reply);
            send(std::move(reply));
            return {};
        }
        case sync_skew:
        {
            std::vector<std::uint8_t> reply = sync_reply_header(correlation);
            put_u64(reply, std::bit_cast<std::uint64_t>(skew()));
            finish_packet(reply);
            send(std::move(reply));
            return {};
        }
        case sync_og:
        case sync_stop:
        default:
        {
            if (subtype != sync_og && subtype != sync_stop && ++unknown_packets <= 10)
            {
                log::warning("USB: unknown sync packet '{}', sending empty reply", fourcc_text(subtype));
            }
            std::vector<std::uint8_t> reply;
            put_u32(reply, 24);
            put_u32(reply, magic_rply);
            put_u64(reply, correlation);
            put_u64(reply, 0);
            send(std::move(reply));
            return {};
        }
        }
    }

    status session::state::handle_asyn(std::span<std::uint8_t const> packet)
    {
        if (packet.size() < 20)
        {
            return std::unexpected(make_failure("qt asyn packet", static_cast<std::int64_t>(packet.size()), "too short"));
        }
        std::uint32_t const subtype = read_u32(packet, 16);
        std::int64_t const arrival = clock::now_ns();
        switch (subtype)
        {
        case asyn_feed:
        {
            auto sample = parse_sample_buffer(packet.subspan(20));
            if (!need_packet.empty())
            {
                send(need_packet); // keep the device sending, even after a bad sample
            }
            if (!sample)
            {
                return std::unexpected(sample.error());
            }
            if (sample->video_format)
            {
                apply_video_format(std::move(*sample->video_format));
                sample->video_format.reset();
            }
            if (!video_format || sample->data.empty())
            {
                return {};
            }
            auto annex_b = length_prefixed_to_annex_b(sample->data, video_format->nal_length_size);
            if (!annex_b)
            {
                return std::unexpected(annex_b.error());
            }
            if (parameter_sets_pending)
            {
                annex_b->insert(annex_b->begin(), video_format->parameter_sets.begin(),
                                video_format->parameter_sets.end());
                parameter_sets_pending = false;
            }
            sample->data = std::move(*annex_b);
            if (handlers.on_video_sample)
            {
                handlers.on_video_sample(std::move(*sample), arrival);
            }
            return {};
        }
        case asyn_eat:
        {
            auto sample = parse_sample_buffer(packet.subspan(20));
            if (!sample)
            {
                return std::unexpected(sample.error());
            }
            if (sample->presentation_time)
            {
                if (!audio_started)
                {
                    audio_started = true;
                    first_audio_time = sample->presentation_time;
                    first_audio_local_ns = arrival;
                }
                last_audio_time = sample->presentation_time;
                last_audio_local_ns = arrival;
            }
            if (sample->audio_format && handlers.on_audio_format)
            {
                handlers.on_audio_format(*sample->audio_format);
            }
            if (handlers.on_audio_sample && !sample->data.empty())
            {
                handlers.on_audio_sample(std::move(*sample), arrival);
            }
            return {};
        }
        case asyn_rels:
            ++released;
            return {};
        case asyn_sprp:
        case asyn_srat:
        case asyn_tbas:
        case asyn_tjmp:
            return {};
        default:
            if (++unknown_packets <= 10)
            {
                log::warning("USB: unknown asyn packet '{}'", fourcc_text(subtype));
            }
            return {};
        }
    }

    session::session(session_handlers handlers, host_display display)
        : impl{std::make_unique<state>()}
    {
        impl->handlers = std::move(handlers);
        impl->display = display;
        impl->time_clock_start_ns = clock::now_ns();
    }

    session::~session() = default;

    status session::handle_packet(std::span<std::uint8_t const> packet)
    {
        if (packet.size() < 8)
        {
            return std::unexpected(make_failure("qt packet", static_cast<std::int64_t>(packet.size()), "too short"));
        }
        std::uint32_t const magic = read_u32(packet, 4);
        if (log::enabled(log::level::debug) && packet.size() >= 20)
        {
            log::debug("USB rx {} {} ({} bytes)", fourcc_text(magic), fourcc_text(read_u32(packet, 16)), packet.size());
        }
        if (magic == magic_ping)
        {
            log::info("USB: ping from the iPhone, capture handshake started");
            impl->send(ping_packet());
            return {};
        }
        if (magic == magic_sync)
        {
            return impl->handle_sync(packet);
        }
        if (magic == magic_asyn)
        {
            return impl->handle_asyn(packet);
        }
        return std::unexpected(make_failure("qt packet magic", magic, fourcc_text(magic)));
    }

    void session::request_stop()
    {
        impl->send(asyn_packet(impl->device_audio_clock, asyn_hpa0));
        impl->send(asyn_packet(empty_clock_ref, asyn_hpd0));
        impl->is_streaming = false;
    }

    int session::released_clocks() const
    {
        return impl->released;
    }

    bool session::streaming() const
    {
        return impl->is_streaming;
    }
}
