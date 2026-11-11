// SPDX-License-Identifier: GPL-3.0-or-later
module uxplay_remote_xpc;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_byte_stream;

namespace uxplay
{
    namespace
    {
        // ---- XPC objects (little endian) -----------------------------------------------
        constexpr std::uint32_t type_null = 0x1000;
        constexpr std::uint32_t type_flag = 0x2000;
        constexpr std::uint32_t type_signed = 0x3000;
        constexpr std::uint32_t type_unsigned = 0x4000;
        constexpr std::uint32_t type_real = 0x5000;
        constexpr std::uint32_t type_date = 0x7000;
        constexpr std::uint32_t type_data = 0x8000;
        constexpr std::uint32_t type_text = 0x9000;
        constexpr std::uint32_t type_uuid = 0xA000;
        constexpr std::uint32_t type_array = 0xE000;
        constexpr std::uint32_t type_dictionary = 0xF000;
        constexpr std::uint32_t type_file_transfer = 0x1A000;

        constexpr std::uint32_t wrapper_magic = 0x29B00B92;
        constexpr std::uint32_t payload_magic = 0x42133742;
        constexpr std::uint32_t payload_version = 5;
        constexpr std::size_t wrapper_header_bytes = 24;
        constexpr std::uint32_t flag_always = 0x00000001;
        constexpr std::uint32_t flag_data_present = 0x00000100;
        constexpr std::uint32_t flag_wanting_reply = 0x00010000;
        constexpr std::uint32_t flag_file_stream_request = 0x00100000;
        constexpr std::uint32_t flag_init_handshake = 0x00400000;
        // nesting deeper than this is not a message of a service
        constexpr std::size_t deepest_nesting = 64;
        // a message larger than this is not one either
        constexpr std::uint64_t longest_message = 64 * 1024 * 1024;

        // ---- HTTP/2 --------------------------------------------------------------------
        constexpr std::string_view connection_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        constexpr std::size_t frame_header_bytes = 9;
        constexpr std::uint8_t frame_data = 0;
        constexpr std::uint8_t frame_headers = 1;
        constexpr std::uint8_t frame_reset = 3;
        constexpr std::uint8_t frame_settings = 4;
        constexpr std::uint8_t frame_ping = 6;
        constexpr std::uint8_t frame_go_away = 7;
        constexpr std::uint8_t frame_window_update = 8;
        constexpr std::uint8_t frame_flag_ack = 0x1;
        constexpr std::uint8_t frame_flag_end_stream = 0x1; // of DATA
        constexpr std::uint8_t frame_flag_end_headers = 0x4;
        constexpr std::uint16_t setting_concurrent_streams = 3;
        constexpr std::uint16_t setting_initial_window = 4;
        constexpr std::uint32_t root_stream = 1;
        constexpr std::uint32_t reply_stream = 3;
        // streams this end opens have odd numbers; a file transfer takes the next one
        constexpr std::uint32_t first_file_stream = 5;
        constexpr std::uint32_t own_initial_window = 16 * 1024 * 1024;
        constexpr std::int64_t default_window = 65535;
        // DATA frames of this end are not larger than what every peer accepts
        constexpr std::size_t largest_frame = 16384;
        // the phone is told about room again after this much was received
        constexpr std::uint32_t replenish_after = 1024 * 1024;
        constexpr auto send_patience = std::chrono::seconds{20};

        class byte_writer
        {
        public:
            void u32(std::uint32_t value)
            {
                for (int shift = 0; shift < 32; shift += 8)
                {
                    out.push_back(static_cast<std::uint8_t>(value >> shift));
                }
            }

            void u64(std::uint64_t value)
            {
                for (int shift = 0; shift < 64; shift += 8)
                {
                    out.push_back(static_cast<std::uint8_t>(value >> shift));
                }
            }

            void raw(std::span<std::uint8_t const> bytes)
            {
                out.insert(out.end(), bytes.begin(), bytes.end());
            }

            // to a multiple of four, counted from `start`
            void pad_from(std::size_t start)
            {
                while ((out.size() - start) % 4 != 0)
                {
                    out.push_back(0);
                }
            }

            void patch_u32(std::size_t position, std::uint32_t value)
            {
                for (int index = 0; index < 4; ++index)
                {
                    out[position + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(value >> (8 * index));
                }
            }

            std::vector<std::uint8_t> out;
        };

        void encode(byte_writer &target, xpc_value const &value)
        {
            switch (value.type)
            {
            case xpc_value::kind::null:
                target.u32(type_null);
                break;
            case xpc_value::kind::flag:
                target.u32(type_flag);
                target.u32(value.number != 0 ? 1 : 0);
                break;
            case xpc_value::kind::signed_number:
                target.u32(type_signed);
                target.u64(value.number);
                break;
            case xpc_value::kind::unsigned_number:
                target.u32(type_unsigned);
                target.u64(value.number);
                break;
            case xpc_value::kind::real:
                target.u32(type_real);
                target.u64(std::bit_cast<std::uint64_t>(value.real));
                break;
            case xpc_value::kind::date:
                target.u32(type_date);
                target.u64(value.number);
                break;
            case xpc_value::kind::data:
            {
                target.u32(type_data);
                std::size_t const start = target.out.size();
                target.u32(static_cast<std::uint32_t>(value.bytes.size()));
                target.raw(value.bytes);
                target.pad_from(start);
                break;
            }
            case xpc_value::kind::text:
            {
                target.u32(type_text);
                std::size_t const start = target.out.size();
                target.u32(static_cast<std::uint32_t>(value.text.size() + 1)); // with its terminating zero
                target.raw(std::span{reinterpret_cast<std::uint8_t const *>(value.text.data()), value.text.size()});
                target.out.push_back(0);
                target.pad_from(start);
                break;
            }
            case xpc_value::kind::uuid:
            {
                target.u32(type_uuid);
                std::array<std::uint8_t, 16> padded{};
                std::copy_n(value.bytes.begin(), std::min<std::size_t>(value.bytes.size(), 16), padded.begin());
                target.raw(padded);
                break;
            }
            case xpc_value::kind::array:
            {
                target.u32(type_array);
                std::size_t const length_position = target.out.size();
                target.u32(0);
                target.u32(static_cast<std::uint32_t>(value.items.size()));
                for (xpc_value const &item : value.items)
                {
                    encode(target, item);
                }
                target.patch_u32(length_position, static_cast<std::uint32_t>(target.out.size() - length_position - 4));
                break;
            }
            case xpc_value::kind::dictionary:
            {
                target.u32(type_dictionary);
                std::size_t const length_position = target.out.size();
                target.u32(0);
                target.u32(static_cast<std::uint32_t>(value.entries.size()));
                for (auto const &[key, item] : value.entries)
                {
                    std::size_t const start = target.out.size();
                    target.raw(std::span{reinterpret_cast<std::uint8_t const *>(key.data()), key.size()});
                    target.out.push_back(0);
                    target.pad_from(start);
                    encode(target, item);
                }
                target.patch_u32(length_position, static_cast<std::uint32_t>(target.out.size() - length_position - 4));
                break;
            }
            case xpc_value::kind::file_transfer:
            {
                // the number that names the transfer, then an object that says how large the file is
                target.u32(type_file_transfer);
                target.u64(value.transfer);
                xpc_value size = xpc_value::of_dictionary();
                size.set("s", xpc_value::of_unsigned(value.number));
                encode(target, size);
                break;
            }
            }
        }

        class byte_reader
        {
        public:
            explicit byte_reader(std::span<std::uint8_t const> bytes)
                : data{bytes}
            {
            }

            [[nodiscard]] bool has(std::size_t count) const
            {
                return count <= data.size() - position;
            }

            [[nodiscard]] std::optional<std::uint32_t> u32()
            {
                if (!has(4))
                {
                    return std::nullopt;
                }
                std::uint32_t value = 0;
                for (int index = 0; index < 4; ++index)
                {
                    value |= static_cast<std::uint32_t>(data[position + static_cast<std::size_t>(index)]) << (8 * index);
                }
                position += 4;
                return value;
            }

            [[nodiscard]] std::optional<std::uint64_t> u64()
            {
                if (!has(8))
                {
                    return std::nullopt;
                }
                std::uint64_t value = 0;
                for (int index = 0; index < 8; ++index)
                {
                    value |= static_cast<std::uint64_t>(data[position + static_cast<std::size_t>(index)]) << (8 * index);
                }
                position += 8;
                return value;
            }

            [[nodiscard]] std::optional<std::span<std::uint8_t const>> take(std::size_t count)
            {
                if (!has(count))
                {
                    return std::nullopt;
                }
                std::span<std::uint8_t const> const part = data.subspan(position, count);
                position += count;
                return part;
            }

            // a zero-terminated text; the reader then stands behind the zero
            [[nodiscard]] std::optional<std::string> terminated_text()
            {
                std::size_t end = position;
                while (end < data.size() && data[end] != 0)
                {
                    ++end;
                }
                if (end >= data.size())
                {
                    return std::nullopt;
                }
                std::string text{reinterpret_cast<char const *>(data.data() + position), end - position};
                position = end + 1;
                return text;
            }

            // to a multiple of four, counted from `start`
            void skip_padding_from(std::size_t start)
            {
                while ((position - start) % 4 != 0 && position < data.size())
                {
                    ++position;
                }
            }

            std::span<std::uint8_t const> data;
            std::size_t position = 0;
        };

        [[nodiscard]] std::optional<xpc_value> decode(byte_reader &source, std::size_t depth)
        {
            if (depth > deepest_nesting)
            {
                return std::nullopt;
            }
            std::optional<std::uint32_t> const type = source.u32();
            if (!type)
            {
                return std::nullopt;
            }
            xpc_value value;
            switch (*type)
            {
            case type_null:
                return value;
            case type_flag:
            {
                std::optional<std::uint32_t> const raw = source.u32();
                if (!raw)
                {
                    return std::nullopt;
                }
                value.type = xpc_value::kind::flag;
                value.number = *raw != 0 ? 1 : 0;
                return value;
            }
            case type_signed:
            case type_unsigned:
            case type_date:
            case type_real:
            {
                std::optional<std::uint64_t> const raw = source.u64();
                if (!raw)
                {
                    return std::nullopt;
                }
                if (*type == type_real)
                {
                    value.type = xpc_value::kind::real;
                    value.real = std::bit_cast<double>(*raw);
                }
                else
                {
                    value.type = *type == type_signed     ? xpc_value::kind::signed_number
                                 : *type == type_unsigned ? xpc_value::kind::unsigned_number
                                                          : xpc_value::kind::date;
                    value.number = *raw;
                }
                return value;
            }
            case type_data:
            {
                std::size_t const start = source.position;
                std::optional<std::uint32_t> const length = source.u32();
                if (!length)
                {
                    return std::nullopt;
                }
                auto const bytes = source.take(*length);
                if (!bytes)
                {
                    return std::nullopt;
                }
                value.type = xpc_value::kind::data;
                value.bytes.assign(bytes->begin(), bytes->end());
                source.skip_padding_from(start);
                return value;
            }
            case type_text:
            {
                std::size_t const start = source.position;
                std::optional<std::uint32_t> const length = source.u32();
                if (!length || *length == 0)
                {
                    return std::nullopt;
                }
                auto const bytes = source.take(*length);
                if (!bytes)
                {
                    return std::nullopt;
                }
                value.type = xpc_value::kind::text;
                // without the terminating zero (and anything after an earlier one)
                std::string_view const whole{reinterpret_cast<char const *>(bytes->data()), bytes->size()};
                value.text = std::string{whole.substr(0, whole.find('\0'))};
                source.skip_padding_from(start);
                return value;
            }
            case type_uuid:
            {
                auto const bytes = source.take(16);
                if (!bytes)
                {
                    return std::nullopt;
                }
                value.type = xpc_value::kind::uuid;
                value.bytes.assign(bytes->begin(), bytes->end());
                return value;
            }
            case type_array:
            {
                std::optional<std::uint32_t> const length = source.u32();
                std::optional<std::uint32_t> const count = source.u32();
                if (!length || !count || *count > source.data.size())
                {
                    return std::nullopt;
                }
                value.type = xpc_value::kind::array;
                for (std::uint32_t index = 0; index < *count; ++index)
                {
                    std::optional<xpc_value> item = decode(source, depth + 1);
                    if (!item)
                    {
                        return std::nullopt;
                    }
                    value.items.push_back(std::move(*item));
                }
                return value;
            }
            case type_dictionary:
            {
                std::optional<std::uint32_t> const length = source.u32();
                std::optional<std::uint32_t> const count = source.u32();
                if (!length || !count || *count > source.data.size())
                {
                    return std::nullopt;
                }
                value.type = xpc_value::kind::dictionary;
                for (std::uint32_t index = 0; index < *count; ++index)
                {
                    std::size_t const start = source.position;
                    std::optional<std::string> key = source.terminated_text();
                    if (!key)
                    {
                        return std::nullopt;
                    }
                    source.skip_padding_from(start);
                    std::optional<xpc_value> item = decode(source, depth + 1);
                    if (!item)
                    {
                        return std::nullopt;
                    }
                    value.entries.emplace_back(std::move(*key), std::move(*item));
                }
                return value;
            }
            case type_file_transfer:
            {
                std::optional<std::uint64_t> const transfer = source.u64();
                if (!transfer)
                {
                    return std::nullopt;
                }
                std::optional<xpc_value> const size = decode(source, depth + 1);
                if (!size)
                {
                    return std::nullopt;
                }
                value.type = xpc_value::kind::file_transfer;
                value.transfer = *transfer;
                if (xpc_value const *const bytes = size->find("s"); bytes != nullptr)
                {
                    value.number = bytes->as_unsigned().value_or(0);
                }
                return value;
            }
            default:
                // a kind this program has no use for (descriptors, ports): the
                // message cannot be read further
                return std::nullopt;
            }
        }

        // one XPC message on the wire: header, then (when there is a payload) the object
        [[nodiscard]] std::vector<std::uint8_t> wrap(std::uint32_t flags, std::uint64_t message_identifier,
                                                     xpc_value const *payload)
        {
            byte_writer body;
            if (payload != nullptr)
            {
                body.u32(payload_magic);
                body.u32(payload_version);
                encode(body, *payload);
            }
            byte_writer message;
            message.u32(wrapper_magic);
            message.u32(flags);
            message.u64(body.out.size());
            message.u64(message_identifier);
            message.raw(body.out);
            return std::move(message.out);
        }

        [[nodiscard]] std::vector<std::uint8_t> frame(std::uint8_t type, std::uint8_t flags, std::uint32_t stream,
                                                      std::span<std::uint8_t const> payload)
        {
            std::vector<std::uint8_t> bytes(frame_header_bytes + payload.size());
            bytes[0] = static_cast<std::uint8_t>(payload.size() >> 16);
            bytes[1] = static_cast<std::uint8_t>(payload.size() >> 8);
            bytes[2] = static_cast<std::uint8_t>(payload.size());
            bytes[3] = type;
            bytes[4] = flags;
            bytes[5] = static_cast<std::uint8_t>((stream >> 24) & 0x7F);
            bytes[6] = static_cast<std::uint8_t>(stream >> 16);
            bytes[7] = static_cast<std::uint8_t>(stream >> 8);
            bytes[8] = static_cast<std::uint8_t>(stream);
            if (!payload.empty())
            {
                std::memcpy(bytes.data() + frame_header_bytes, payload.data(), payload.size());
            }
            return bytes;
        }

        [[nodiscard]] std::uint32_t be32(std::uint8_t const *source)
        {
            return (static_cast<std::uint32_t>(source[0]) << 24) | (static_cast<std::uint32_t>(source[1]) << 16) |
                   (static_cast<std::uint32_t>(source[2]) << 8) | static_cast<std::uint32_t>(source[3]);
        }

        [[nodiscard]] std::array<std::uint8_t, 4> be32_bytes(std::uint32_t value)
        {
            return {static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16),
                    static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
        }
    }

    xpc_value xpc_value::of_flag(bool value)
    {
        xpc_value made;
        made.type = kind::flag;
        made.number = value ? 1 : 0;
        return made;
    }

    xpc_value xpc_value::of_signed(std::int64_t value)
    {
        xpc_value made;
        made.type = kind::signed_number;
        made.number = static_cast<std::uint64_t>(value);
        return made;
    }

    xpc_value xpc_value::of_unsigned(std::uint64_t value)
    {
        xpc_value made;
        made.type = kind::unsigned_number;
        made.number = value;
        return made;
    }

    xpc_value xpc_value::of_text(std::string_view value)
    {
        xpc_value made;
        made.type = kind::text;
        made.text = std::string{value};
        return made;
    }

    xpc_value xpc_value::of_data(std::span<std::uint8_t const> value)
    {
        xpc_value made;
        made.type = kind::data;
        made.bytes.assign(value.begin(), value.end());
        return made;
    }

    xpc_value xpc_value::of_uuid(std::span<std::uint8_t const, 16> value)
    {
        xpc_value made;
        made.type = kind::uuid;
        made.bytes.assign(value.begin(), value.end());
        return made;
    }

    xpc_value xpc_value::of_array(std::vector<xpc_value> values)
    {
        xpc_value made;
        made.type = kind::array;
        made.items = std::move(values);
        return made;
    }

    xpc_value xpc_value::of_dictionary()
    {
        xpc_value made;
        made.type = kind::dictionary;
        return made;
    }

    xpc_value xpc_value::of_file_transfer(std::uint64_t transfer, std::uint64_t size)
    {
        xpc_value made;
        made.type = kind::file_transfer;
        made.transfer = transfer;
        made.number = size;
        return made;
    }

    xpc_value &xpc_value::set(std::string_view key, xpc_value value)
    {
        type = kind::dictionary;
        for (auto &[existing_key, existing_value] : entries)
        {
            if (existing_key == key)
            {
                existing_value = std::move(value);
                return *this;
            }
        }
        entries.emplace_back(std::string{key}, std::move(value));
        return *this;
    }

    xpc_value const *xpc_value::find(std::string_view key) const
    {
        if (type != kind::dictionary)
        {
            return nullptr;
        }
        for (auto const &[existing_key, existing_value] : entries)
        {
            if (existing_key == key)
            {
                return &existing_value;
            }
        }
        return nullptr;
    }

    std::optional<std::string_view> xpc_value::as_text() const
    {
        if (type != kind::text)
        {
            return std::nullopt;
        }
        return std::string_view{text};
    }

    std::optional<std::uint64_t> xpc_value::as_unsigned() const
    {
        if (type == kind::unsigned_number || (type == kind::signed_number && static_cast<std::int64_t>(number) >= 0))
        {
            return number;
        }
        return std::nullopt;
    }

    std::optional<bool> xpc_value::as_flag() const
    {
        if (type != kind::flag)
        {
            return std::nullopt;
        }
        return number != 0;
    }

    std::string xpc_value::describe(std::size_t depth) const
    {
        std::string const indent(depth * 2, ' ');
        switch (type)
        {
        case kind::null:
            return "null";
        case kind::flag:
            return number != 0 ? "true" : "false";
        case kind::signed_number:
            return std::format("{}", static_cast<std::int64_t>(number));
        case kind::unsigned_number:
            return std::format("{}u", number);
        case kind::real:
            return std::format("{}", real);
        case kind::date:
            return std::format("date({})", static_cast<std::int64_t>(number));
        case kind::data:
            return std::format("data({} bytes)", bytes.size());
        case kind::text:
            return std::format("\"{}\"", text.size() > 80 ? text.substr(0, 80) + "..." : text);
        case kind::uuid:
            return "uuid";
        case kind::array:
        {
            std::string out = std::format("[{} item(s)]", items.size());
            for (std::size_t index = 0; index < items.size() && index < 8; ++index)
            {
                out += std::format("\n{}  - {}", indent, items[index].describe(depth + 1));
            }
            return out;
        }
        case kind::dictionary:
        {
            std::string out = std::format("{{{} entr(ies)}}", entries.size());
            for (auto const &[key, value] : entries)
            {
                out += std::format("\n{}  {}: {}", indent, key, value.describe(depth + 1));
            }
            return out;
        }
        case kind::file_transfer:
            return std::format("file({} bytes)", number);
        }
        return "?";
    }

    struct remote_xpc_connection::state
    {
        std::unique_ptr<byte_stream> stream;
        std::thread reader;
        std::mutex mutex;
        std::condition_variable changed;
        // guarded by mutex
        std::deque<xpc_value> received;
        std::string failure_text; // not empty: the connection is over
        std::int64_t connection_window = default_window; // what the phone still takes: all streams
        // ... and on each stream this end sends data on (the root stream, file transfers under way)
        std::map<std::uint32_t, std::int64_t> stream_windows{{root_stream, default_window}};
        std::int64_t peer_initial_window = default_window;
        std::uint32_t next_file_stream = first_file_stream;
        std::uint64_t next_message = 0;
        std::map<std::uint32_t, std::vector<std::uint8_t>> partial; // per stream: bytes of a message not complete yet
        std::uint32_t received_since_update = 0;
        std::mutex send_mutex;    // one frame at a time on the stream
        std::mutex request_mutex; // one request and its answer at a time

        [[nodiscard]] status write_frame(std::uint8_t type, std::uint8_t flags, std::uint32_t stream_identifier,
                                         std::span<std::uint8_t const> payload)
        {
            std::vector<std::uint8_t> const bytes = frame(type, flags, stream_identifier, payload);
            std::scoped_lock lock{send_mutex};
            return stream->write_all(bytes);
        }

        void fail(std::string reason)
        {
            {
                std::scoped_lock lock{mutex};
                if (failure_text.empty())
                {
                    failure_text = std::move(reason);
                }
            }
            changed.notify_all();
        }

        // DATA on a stream of this end, within what the phone said it takes
        [[nodiscard]] status send_data(std::uint32_t stream_identifier, std::span<std::uint8_t const> message)
        {
            std::size_t offset = 0;
            while (offset < message.size())
            {
                std::size_t chunk = 0;
                {
                    std::unique_lock lock{mutex};
                    std::int64_t &stream_window = stream_windows[stream_identifier];
                    bool const ready = changed.wait_for(lock, send_patience, [this, &stream_window]
                    {
                        return !failure_text.empty() || (connection_window > 0 && stream_window > 0);
                    });
                    if (!failure_text.empty())
                    {
                        return std::unexpected(make_failure("RemoteXPC", 0, failure_text));
                    }
                    if (!ready)
                    {
                        return std::unexpected(make_failure("RemoteXPC", 0, "the phone does not take any more data"));
                    }
                    chunk = std::min<std::size_t>(
                        {message.size() - offset, largest_frame, static_cast<std::size_t>(connection_window),
                         static_cast<std::size_t>(stream_window)});
                    connection_window -= static_cast<std::int64_t>(chunk);
                    stream_window -= static_cast<std::int64_t>(chunk);
                }
                if (status const sent = write_frame(frame_data, 0, stream_identifier, message.subspan(offset, chunk));
                    !sent)
                {
                    fail(sent.error().describe());
                    return sent;
                }
                offset += chunk;
            }
            return {};
        }

        // messages that are complete in the bytes of one stream
        void take_messages(std::vector<std::uint8_t> &bytes)
        {
            for (;;)
            {
                if (bytes.size() < wrapper_header_bytes)
                {
                    return;
                }
                byte_reader header{bytes};
                std::uint32_t const magic = header.u32().value_or(0);
                static_cast<void>(header.u32()); // flags
                std::uint64_t const size = header.u64().value_or(0);
                if (magic != wrapper_magic || size > longest_message)
                {
                    bytes.clear();
                    failure_text = "the phone sent something that is no XPC message";
                    return;
                }
                if (bytes.size() < wrapper_header_bytes + size)
                {
                    return; // the rest comes with the next frame
                }
                if (size > 8)
                {
                    byte_reader body{std::span<std::uint8_t const>{bytes}.subspan(wrapper_header_bytes, size)};
                    std::uint32_t const body_magic = body.u32().value_or(0);
                    static_cast<void>(body.u32()); // version
                    if (body_magic == payload_magic)
                    {
                        if (std::optional<xpc_value> value = decode(body, 0);
                            value && value->type == xpc_value::kind::dictionary && !value->entries.empty())
                        {
                            received.push_back(std::move(*value));
                        }
                    }
                }
                bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(wrapper_header_bytes + size));
            }
        }

        void read_loop()
        {
            std::vector<std::uint8_t> payload;
            for (;;)
            {
                std::array<std::uint8_t, frame_header_bytes> header{};
                if (status const got = stream->read_exact(header); !got)
                {
                    fail(got.error().describe());
                    return;
                }
                std::size_t const length = (static_cast<std::size_t>(header[0]) << 16) |
                                           (static_cast<std::size_t>(header[1]) << 8) | header[2];
                std::uint8_t const type = header[3];
                std::uint8_t const flags = header[4];
                std::uint32_t const stream_identifier = be32(header.data() + 5) & 0x7FFFFFFF;
                payload.resize(length);
                if (length > 0)
                {
                    if (status const got = stream->read_exact(payload); !got)
                    {
                        fail(got.error().describe());
                        return;
                    }
                }
                switch (type)
                {
                case frame_data:
                {
                    std::uint32_t replenish = 0;
                    {
                        std::scoped_lock lock{mutex};
                        std::vector<std::uint8_t> &bytes = partial[stream_identifier];
                        bytes.insert(bytes.end(), payload.begin(), payload.end());
                        take_messages(bytes);
                        received_since_update += static_cast<std::uint32_t>(length);
                        if (received_since_update >= replenish_after)
                        {
                            replenish = std::exchange(received_since_update, 0);
                        }
                    }
                    changed.notify_all();
                    if (replenish > 0)
                    {
                        // room again, for the connection and for that stream
                        std::array<std::uint8_t, 4> const increment = be32_bytes(replenish);
                        if (status const sent = write_frame(frame_window_update, 0, 0, increment); !sent)
                        {
                            fail(sent.error().describe());
                            return;
                        }
                        static_cast<void>(write_frame(frame_window_update, 0, stream_identifier, increment));
                    }
                    break;
                }
                case frame_settings:
                    if ((flags & frame_flag_ack) == 0)
                    {
                        {
                            std::scoped_lock lock{mutex};
                            for (std::size_t offset = 0; offset + 6 <= payload.size(); offset += 6)
                            {
                                auto const identifier =
                                    static_cast<std::uint16_t>((payload[offset] << 8) | payload[offset + 1]);
                                std::uint32_t const value = be32(payload.data() + offset + 2);
                                if (identifier == setting_initial_window)
                                {
                                    // a new initial window moves the window of every open stream
                                    for (auto &[open_stream, window] : stream_windows)
                                    {
                                        window += static_cast<std::int64_t>(value) - peer_initial_window;
                                    }
                                    peer_initial_window = value;
                                }
                            }
                        }
                        changed.notify_all();
                        if (status const sent = write_frame(frame_settings, frame_flag_ack, 0, {}); !sent)
                        {
                            fail(sent.error().describe());
                            return;
                        }
                    }
                    break;
                case frame_window_update:
                    if (payload.size() >= 4)
                    {
                        std::int64_t const increment = be32(payload.data()) & 0x7FFFFFFF;
                        std::scoped_lock lock{mutex};
                        if (stream_identifier == 0)
                        {
                            connection_window += increment;
                        }
                        else if (auto const open_stream = stream_windows.find(stream_identifier);
                                 open_stream != stream_windows.end())
                        {
                            open_stream->second += increment;
                        }
                    }
                    changed.notify_all();
                    break;
                case frame_ping:
                    if ((flags & frame_flag_ack) == 0)
                    {
                        static_cast<void>(write_frame(frame_ping, frame_flag_ack, 0, payload));
                    }
                    break;
                case frame_go_away:
                    fail("the phone closed the connection (GOAWAY)");
                    return;
                case frame_reset:
                    if (stream_identifier == root_stream || stream_identifier == reply_stream)
                    {
                        fail("the phone reset the stream");
                        return;
                    }
                    break;
                default:
                    break; // HEADERS of the phone and anything else: nothing to act on
                }
            }
        }

        [[nodiscard]] outcome<xpc_value> await_message(std::chrono::milliseconds patience)
        {
            std::unique_lock lock{mutex};
            bool const arrived = changed.wait_for(lock, patience, [this]
            {
                return !received.empty() || !failure_text.empty();
            });
            if (!received.empty())
            {
                xpc_value message = std::move(received.front());
                received.pop_front();
                return message;
            }
            if (!failure_text.empty())
            {
                return std::unexpected(make_failure("RemoteXPC", 0, failure_text));
            }
            static_cast<void>(arrived);
            return std::unexpected(make_failure("RemoteXPC", 0, "the phone did not answer in time"));
        }

        [[nodiscard]] status send_message(xpc_value const &message, bool wanting_reply)
        {
            std::uint64_t identifier = 0;
            {
                std::scoped_lock lock{mutex};
                identifier = next_message;
                // this end's messages have odd numbers (only the empty first one is 0)
                next_message = (next_message + 1) | 1;
            }
            std::uint32_t flags = flag_always;
            if (!message.entries.empty())
            {
                flags |= flag_data_present;
            }
            if (wanting_reply)
            {
                flags |= flag_wanting_reply;
            }
            return send_data(root_stream, wrap(flags, identifier, &message));
        }

        // The content of an announced file: a stream of its own, opened with
        // a message that carries the transfer's number, then the bytes.  (The
        // phone confirms the stream only after the content: nothing to wait for.)
        [[nodiscard]] status send_file(std::uint64_t transfer, std::span<std::uint8_t const> content)
        {
            std::uint32_t stream_identifier = 0;
            {
                std::scoped_lock lock{mutex};
                stream_identifier = next_file_stream;
                next_file_stream += 2;
                stream_windows[stream_identifier] = peer_initial_window;
            }
            status sent = write_frame(frame_headers, frame_flag_end_headers, stream_identifier, {});
            if (sent)
            {
                sent = send_data(stream_identifier, wrap(flag_always | flag_file_stream_request, transfer, nullptr));
            }
            if (sent)
            {
                sent = send_data(stream_identifier, content);
            }
            if (sent)
            {
                sent = write_frame(frame_data, frame_flag_end_stream, stream_identifier, {});
            }
            {
                // what the phone says about this stream from now on changes nothing
                std::scoped_lock lock{mutex};
                stream_windows.erase(stream_identifier);
            }
            if (!sent)
            {
                fail(sent.error().describe());
            }
            return sent;
        }
    };

    remote_xpc_connection::remote_xpc_connection(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    remote_xpc_connection::~remote_xpc_connection()
    {
        impl->fail("closed here");
        impl->stream->shut_down();
        if (impl->reader.joinable())
        {
            impl->reader.join();
        }
    }

    outcome<std::unique_ptr<remote_xpc_connection>> remote_xpc_connection::open(std::unique_ptr<byte_stream> stream)
    {
        auto created = std::make_unique<state>();
        created->stream = std::move(stream);
        state *const running = created.get();

        // The opening, in the order Apple's own client uses (the phone checks it):
        // preface, SETTINGS, WINDOW_UPDATE, HEADERS(1), an empty message on 1,
        // HEADERS(3), the "data present" marker on 1, the handshake marker on 3.
        if (status const sent = running->stream->write_all(
                std::span{reinterpret_cast<std::uint8_t const *>(connection_preface.data()), connection_preface.size()});
            !sent)
        {
            return std::unexpected(sent.error());
        }
        std::array<std::uint8_t, 12> settings{};
        settings[1] = static_cast<std::uint8_t>(setting_concurrent_streams);
        settings[5] = 100;
        settings[7] = static_cast<std::uint8_t>(setting_initial_window);
        std::array<std::uint8_t, 4> const window_bytes = be32_bytes(own_initial_window);
        std::copy(window_bytes.begin(), window_bytes.end(), settings.begin() + 8);
        std::array<std::uint8_t, 4> const increment = be32_bytes(own_initial_window - 65535);
        xpc_value const empty = xpc_value::of_dictionary();
        std::vector<std::uint8_t> const first_message = wrap(flag_always, 0, &empty);
        std::vector<std::uint8_t> const data_marker = wrap(flag_always | flag_data_present, 0, nullptr);
        std::vector<std::uint8_t> const handshake_marker = wrap(flag_always | flag_init_handshake, 0, nullptr);
        struct step
        {
            std::uint8_t type;
            std::uint8_t flags;
            std::uint32_t stream_identifier;
            std::span<std::uint8_t const> payload;
        };
        for (step const &next : {step{frame_settings, 0, 0, settings}, step{frame_window_update, 0, 0, increment},
                                 step{frame_headers, frame_flag_end_headers, root_stream, {}},
                                 step{frame_data, 0, root_stream, first_message},
                                 step{frame_headers, frame_flag_end_headers, reply_stream, {}},
                                 step{frame_data, 0, root_stream, data_marker},
                                 step{frame_data, 0, reply_stream, handshake_marker}})
        {
            if (status const sent = running->write_frame(next.type, next.flags, next.stream_identifier, next.payload);
                !sent)
            {
                return std::unexpected(sent.error());
            }
        }
        // what was sent on the root stream so far counts against the phone's windows
        running->connection_window -=
            static_cast<std::int64_t>(first_message.size() + data_marker.size() + handshake_marker.size());
        running->stream_windows[root_stream] -= static_cast<std::int64_t>(first_message.size() + data_marker.size());
        running->next_message = 1;
        running->reader = std::thread{[running] { running->read_loop(); }};
        return std::unique_ptr<remote_xpc_connection>{new remote_xpc_connection{std::move(created)}};
    }

    status remote_xpc_connection::send(xpc_value const &message)
    {
        return impl->send_message(message, false);
    }

    outcome<xpc_value> remote_xpc_connection::request(xpc_value const &message, std::chrono::milliseconds patience)
    {
        std::scoped_lock lock{impl->request_mutex};
        if (status const sent = impl->send_message(message, true); !sent)
        {
            return std::unexpected(sent.error());
        }
        return impl->await_message(patience);
    }

    status remote_xpc_connection::begin_request(xpc_value const &message)
    {
        return impl->send_message(message, true);
    }

    status remote_xpc_connection::send_file(std::uint64_t transfer, std::span<std::uint8_t const> content)
    {
        return impl->send_file(transfer, content);
    }

    outcome<xpc_value> remote_xpc_connection::receive(std::chrono::milliseconds patience)
    {
        return impl->await_message(patience);
    }
}
