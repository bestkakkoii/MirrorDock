// SPDX-License-Identifier: MIT
module uxplay_zip_archive;

import std;
import uxplay_status;

namespace uxplay
{
    namespace
    {
        constexpr std::uint32_t end_record_mark = 0x06054b50;
        constexpr std::uint32_t directory_mark = 0x02014b50;
        constexpr std::uint32_t file_mark = 0x04034b50;
        constexpr std::size_t end_record_size = 22;
        constexpr std::size_t directory_header_size = 46;
        constexpr std::size_t file_header_size = 30;
        constexpr std::size_t longest_comment = 65535;
        constexpr std::uint16_t method_stored = 0;
        constexpr std::uint16_t method_deflated = 8;
        constexpr std::uint16_t flag_encrypted = 1;
        // no file of a release is anywhere near this; a size beyond it is damage or malice
        constexpr std::uint64_t largest_file = 512ULL * 1024 * 1024;

        [[nodiscard]] std::uint16_t word(std::span<std::uint8_t const> bytes, std::size_t offset)
        {
            return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
        }

        [[nodiscard]] std::uint32_t long_word(std::span<std::uint8_t const> bytes, std::size_t offset)
        {
            return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
                   (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
                   (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
        }

        [[nodiscard]] failure archive_failure(std::string_view what)
        {
            return make_failure("zip archive", 0, what);
        }

        constexpr std::array<std::uint32_t, 256> crc_table = []
        {
            std::array<std::uint32_t, 256> table{};
            for (std::uint32_t index = 0; index < 256; ++index)
            {
                std::uint32_t value = index;
                for (int bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1U) != 0 ? 0xEDB88320U ^ (value >> 1) : value >> 1;
                }
                table[index] = value;
            }
            return table;
        }();

        // ---- inflate (RFC 1951) ------------------------------------------------

        // the bits of the deflated data, least significant first
        struct bit_source
        {
            std::span<std::uint8_t const> bytes;
            std::size_t position = 0;
            std::uint32_t buffer = 0;
            int held = 0;
            bool exhausted = false; // more bits were asked for than there are

            // count: 0..16
            [[nodiscard]] std::uint32_t take(int count)
            {
                while (held < count)
                {
                    if (position >= bytes.size())
                    {
                        exhausted = true;
                        return 0;
                    }
                    buffer |= static_cast<std::uint32_t>(bytes[position++]) << held;
                    held += 8;
                }
                std::uint32_t const value = buffer & ((1U << count) - 1U);
                buffer >>= count;
                held -= count;
                return value;
            }
        };

        constexpr int longest_code = 15;
        constexpr std::size_t most_length_codes = 286;
        constexpr std::size_t most_distance_codes = 30;
        constexpr std::size_t fixed_length_codes = 288;

        // a canonical Huffman code: how many codes each length has, and the symbols in the order of their codes
        struct code_table
        {
            std::array<std::uint16_t, longest_code + 1> count{};
            std::array<std::uint16_t, fixed_length_codes> symbol{};
        };

        // Makes the table from the code length of each symbol.  0: the code is
        // complete; above 0: codes are left unused; below 0: more codes than
        // the lengths allow (unusable).
        [[nodiscard]] int build(code_table &table, std::span<std::uint8_t const> lengths)
        {
            table.count.fill(0);
            for (std::uint8_t const length : lengths)
            {
                ++table.count[length];
            }
            if (table.count[0] == lengths.size())
            {
                return 0; // no code at all: nothing can be decoded with it, which decode() reports
            }
            int left = 1;
            for (int length = 1; length <= longest_code; ++length)
            {
                left <<= 1;
                left -= table.count[static_cast<std::size_t>(length)];
                if (left < 0)
                {
                    return left;
                }
            }
            std::array<std::uint16_t, longest_code + 2> offset{};
            for (std::size_t length = 1; length <= longest_code; ++length)
            {
                offset[length + 1] = static_cast<std::uint16_t>(offset[length] + table.count[length]);
            }
            for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol)
            {
                if (lengths[symbol] != 0)
                {
                    table.symbol[offset[lengths[symbol]]++] = static_cast<std::uint16_t>(symbol);
                }
            }
            return left;
        }

        // the next symbol; -1: a code that is in no table, or the data ended
        [[nodiscard]] int decode(bit_source &bits, code_table const &table)
        {
            int code = 0;
            int first = 0;
            int index = 0;
            for (std::size_t length = 1; length <= longest_code; ++length)
            {
                code |= static_cast<int>(bits.take(1));
                if (bits.exhausted)
                {
                    return -1;
                }
                int const count = table.count[length];
                if (code - count < first)
                {
                    return table.symbol[static_cast<std::size_t>(index + (code - first))];
                }
                index += count;
                first += count;
                first <<= 1;
                code <<= 1;
            }
            return -1;
        }

        constexpr std::array<std::uint16_t, 29> length_base{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        constexpr std::array<std::uint8_t, 29> length_extra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                            2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        constexpr std::array<std::uint16_t, 30> distance_base{1,   2,   3,   4,   5,    7,    9,    13,   17,   25,
                                                              33,  49,  65,  97,  129,  193,  257,  385,  513,  769,
                                                              1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        constexpr std::array<std::uint8_t, 30> distance_extra{0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

        // The literals and copies of one block.  Nothing on success, else what is wrong.
        [[nodiscard]] char const *block_data(bit_source &bits, std::vector<std::uint8_t> &output, std::size_t limit,
                                             code_table const &lengths, code_table const &distances)
        {
            for (;;)
            {
                int const symbol = decode(bits, lengths);
                if (symbol < 0)
                {
                    return "it has a code that is in no table, or ends early";
                }
                if (symbol < 256)
                {
                    if (output.size() >= limit)
                    {
                        return "it unpacks to more than the size it gives";
                    }
                    output.push_back(static_cast<std::uint8_t>(symbol));
                    continue;
                }
                if (symbol == 256)
                {
                    return nullptr; // the end of the block
                }
                auto const length_code = static_cast<std::size_t>(symbol - 257);
                if (length_code >= length_base.size())
                {
                    return "it has a length code that does not exist";
                }
                std::size_t const length = length_base[length_code] + bits.take(length_extra[length_code]);
                int const distance_code = decode(bits, distances);
                if (distance_code < 0 || static_cast<std::size_t>(distance_code) >= distance_base.size())
                {
                    return "it has a distance code that does not exist, or ends early";
                }
                std::size_t const distance = distance_base[static_cast<std::size_t>(distance_code)] +
                                             bits.take(distance_extra[static_cast<std::size_t>(distance_code)]);
                if (bits.exhausted)
                {
                    return "it ends early";
                }
                if (distance > output.size())
                {
                    return "it copies from before its start";
                }
                if (length > limit - output.size())
                {
                    return "it unpacks to more than the size it gives";
                }
                // (the source may overlap what is written: byte by byte)
                std::size_t const from = output.size() - distance;
                for (std::size_t copied = 0; copied < length; ++copied)
                {
                    output.push_back(output[from + copied]);
                }
            }
        }

        [[nodiscard]] char const *stored_block(bit_source &bits, std::vector<std::uint8_t> &output, std::size_t limit)
        {
            // the rest of the byte is skipped; whole bytes follow
            bits.buffer = 0;
            bits.held = 0;
            if (bits.bytes.size() - bits.position < 4)
            {
                return "it ends early";
            }
            std::size_t const length = word(bits.bytes, bits.position);
            if (word(bits.bytes, bits.position + 2) != static_cast<std::uint16_t>(~length))
            {
                return "a stored block has lengths that do not match";
            }
            bits.position += 4;
            if (length > bits.bytes.size() - bits.position)
            {
                return "it ends early";
            }
            if (length > limit - output.size())
            {
                return "it unpacks to more than the size it gives";
            }
            output.insert(output.end(), bits.bytes.begin() + static_cast<std::ptrdiff_t>(bits.position),
                          bits.bytes.begin() + static_cast<std::ptrdiff_t>(bits.position + length));
            bits.position += length;
            return nullptr;
        }

        [[nodiscard]] char const *fixed_block(bit_source &bits, std::vector<std::uint8_t> &output, std::size_t limit)
        {
            static code_table const lengths = []
            {
                std::array<std::uint8_t, fixed_length_codes> each{};
                std::fill(each.begin(), each.begin() + 144, std::uint8_t{8});
                std::fill(each.begin() + 144, each.begin() + 256, std::uint8_t{9});
                std::fill(each.begin() + 256, each.begin() + 280, std::uint8_t{7});
                std::fill(each.begin() + 280, each.end(), std::uint8_t{8});
                code_table table;
                (void) build(table, each);
                return table;
            }();
            static code_table const distances = []
            {
                std::array<std::uint8_t, most_distance_codes> each{};
                each.fill(5);
                code_table table;
                (void) build(table, each);
                return table;
            }();
            return block_data(bits, output, limit, lengths, distances);
        }

        [[nodiscard]] char const *dynamic_block(bit_source &bits, std::vector<std::uint8_t> &output, std::size_t limit)
        {
            std::size_t const length_codes = bits.take(5) + 257;
            std::size_t const distance_codes = bits.take(5) + 1;
            std::size_t const length_length_codes = bits.take(4) + 4;
            if (bits.exhausted)
            {
                return "it ends early";
            }
            if (length_codes > most_length_codes || distance_codes > most_distance_codes)
            {
                return "a block announces more codes than exist";
            }
            // the order in which the lengths of the code-length code are written
            constexpr std::array<std::uint8_t, 19> order{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::array<std::uint8_t, most_length_codes + most_distance_codes> each{};
            for (std::size_t index = 0; index < length_length_codes; ++index)
            {
                each[order[index]] = static_cast<std::uint8_t>(bits.take(3));
            }
            code_table length_lengths;
            if (build(length_lengths, std::span{each}.first(order.size())) != 0)
            {
                return "the code of a block's code lengths is not complete";
            }
            std::size_t filled = 0;
            while (filled < length_codes + distance_codes)
            {
                int const symbol = decode(bits, length_lengths);
                if (symbol < 0)
                {
                    return "it has a code that is in no table, or ends early";
                }
                if (symbol < 16)
                {
                    each[filled++] = static_cast<std::uint8_t>(symbol);
                    continue;
                }
                std::uint8_t repeated = 0;
                std::size_t times = 0;
                if (symbol == 16)
                {
                    if (filled == 0)
                    {
                        return "a block repeats a code length before the first one";
                    }
                    repeated = each[filled - 1];
                    times = 3 + bits.take(2);
                }
                else if (symbol == 17)
                {
                    times = 3 + bits.take(3);
                }
                else
                {
                    times = 11 + bits.take(7);
                }
                if (filled + times > length_codes + distance_codes)
                {
                    return "a block gives more code lengths than it announced";
                }
                std::fill_n(each.begin() + static_cast<std::ptrdiff_t>(filled), times, repeated);
                filled += times;
            }
            if (bits.exhausted)
            {
                return "it ends early";
            }
            if (each[256] == 0)
            {
                return "a block has no code for its end";
            }
            // an incomplete code is allowed only when it has a single code
            code_table lengths;
            int left = build(lengths, std::span{each}.first(length_codes));
            if (left < 0 || (left > 0 && length_codes - lengths.count[0] != 1))
            {
                return "the code of a block's literals and lengths is not usable";
            }
            code_table distances;
            left = build(distances, std::span{each}.subspan(length_codes, distance_codes));
            if (left < 0 || (left > 0 && distance_codes - distances.count[0] != 1))
            {
                return "the code of a block's distances is not usable";
            }
            return block_data(bits, output, limit, lengths, distances);
        }
    }

    std::uint32_t crc32(std::span<std::uint8_t const> bytes)
    {
        std::uint32_t value = 0xFFFFFFFFU;
        for (std::uint8_t const byte : bytes)
        {
            value = crc_table[(value ^ byte) & 0xFFU] ^ (value >> 8);
        }
        return value ^ 0xFFFFFFFFU;
    }

    outcome<std::vector<std::uint8_t>> inflate(std::span<std::uint8_t const> deflated, std::size_t size)
    {
        std::vector<std::uint8_t> output;
        output.reserve(size);
        bit_source bits{deflated};
        bool last = false;
        while (!last)
        {
            last = bits.take(1) != 0;
            std::uint32_t const kind = bits.take(2);
            char const *problem = "it ends early";
            if (!bits.exhausted)
            {
                problem = kind == 0   ? stored_block(bits, output, size)
                          : kind == 1 ? fixed_block(bits, output, size)
                          : kind == 2 ? dynamic_block(bits, output, size)
                                      : "it has a block of a kind that does not exist";
            }
            if (problem != nullptr)
            {
                return std::unexpected(make_failure("deflated data", static_cast<std::int64_t>(output.size()),
                                                    std::format("{} (after this many unpacked bytes)", problem)));
            }
        }
        if (output.size() != size)
        {
            return std::unexpected(make_failure("deflated data", static_cast<std::int64_t>(output.size()),
                                                std::format("it unpacks to this many bytes, not to the {} it gives",
                                                            size)));
        }
        return output;
    }

    outcome<zip_archive> zip_archive::open(std::span<std::uint8_t const> bytes)
    {
        if (bytes.size() < end_record_size)
        {
            return std::unexpected(archive_failure("it is too short to be one"));
        }
        // the end record is the last thing in the file, before a comment of at most 65535 bytes
        std::size_t const lowest = bytes.size() - std::min(bytes.size(), end_record_size + longest_comment);
        std::optional<std::size_t> end;
        for (std::size_t offset = bytes.size() - end_record_size + 1; offset-- > lowest;)
        {
            if (long_word(bytes, offset) == end_record_mark &&
                offset + end_record_size + word(bytes, offset + 20) == bytes.size())
            {
                end = offset;
                break;
            }
        }
        if (!end)
        {
            return std::unexpected(archive_failure("its end record was not found: the file is cut off or no zip archive"));
        }
        std::size_t const count = word(bytes, *end + 10);
        std::uint64_t const directory_size = long_word(bytes, *end + 12);
        std::uint64_t const directory_offset = long_word(bytes, *end + 16);
        if (word(bytes, *end + 4) != 0 || word(bytes, *end + 6) != 0 || word(bytes, *end + 8) != count)
        {
            return std::unexpected(archive_failure("it is one part of an archive in several parts"));
        }
        if (count == 0xFFFF || directory_size == 0xFFFFFFFFU || directory_offset == 0xFFFFFFFFU)
        {
            return std::unexpected(archive_failure("it is a zip64 archive, which is not read here"));
        }
        if (directory_offset + directory_size > *end)
        {
            return std::unexpected(archive_failure("its directory lies outside the file"));
        }
        zip_archive opened;
        opened.archive = bytes;
        opened.names.reserve(count);
        opened.places.reserve(count);
        auto offset = static_cast<std::size_t>(directory_offset);
        std::size_t const directory_end = offset + static_cast<std::size_t>(directory_size);
        for (std::size_t index = 0; index < count; ++index)
        {
            if (directory_end - offset < directory_header_size || long_word(bytes, offset) != directory_mark)
            {
                return std::unexpected(make_failure("zip archive", static_cast<std::int64_t>(index),
                                                    "its directory ends before this entry, or the entry is damaged"));
            }
            std::uint16_t const flags = word(bytes, offset + 8);
            place where;
            where.method = word(bytes, offset + 10);
            where.crc = long_word(bytes, offset + 16);
            where.packed_size = long_word(bytes, offset + 20);
            std::uint64_t const size = long_word(bytes, offset + 24);
            std::size_t const name_length = word(bytes, offset + 28);
            std::size_t const rest_length = std::size_t{word(bytes, offset + 30)} + word(bytes, offset + 32);
            where.header_offset = long_word(bytes, offset + 42);
            if (directory_end - offset - directory_header_size < name_length + rest_length)
            {
                return std::unexpected(make_failure("zip archive", static_cast<std::int64_t>(index),
                                                    "the name of this entry lies outside the directory"));
            }
            zip_entry entry;
            entry.name.assign(reinterpret_cast<char const *>(bytes.data()) + offset + directory_header_size, name_length);
            entry.size = size;
            entry.folder = entry.name.ends_with('/');
            if ((flags & flag_encrypted) != 0)
            {
                return std::unexpected(archive_failure(std::format("\"{}\" is encrypted", entry.name)));
            }
            if (where.method != method_stored && where.method != method_deflated)
            {
                return std::unexpected(make_failure("zip archive", where.method,
                                                    std::format("\"{}\" is packed with this method, which is not read here",
                                                                entry.name)));
            }
            if (where.packed_size == 0xFFFFFFFFU || size == 0xFFFFFFFFU || where.header_offset == 0xFFFFFFFFU ||
                size > largest_file)
            {
                return std::unexpected(archive_failure(std::format("\"{}\" is larger than a file read here may be", entry.name)));
            }
            opened.names.push_back(std::move(entry));
            opened.places.push_back(where);
            offset += directory_header_size + name_length + rest_length;
        }
        return opened;
    }

    std::span<zip_entry const> zip_archive::entries() const
    {
        return names;
    }

    outcome<std::vector<std::uint8_t>> zip_archive::read(std::size_t index) const
    {
        if (index >= names.size())
        {
            return std::unexpected(make_failure("zip archive", static_cast<std::int64_t>(index), "it has no entry of this number"));
        }
        zip_entry const &entry = names[index];
        place const &where = places[index];
        auto const failed = [&entry](std::string_view what)
        {
            return std::unexpected(archive_failure(std::format("\"{}\": {}", entry.name, what)));
        };
        if (where.header_offset > archive.size() || archive.size() - where.header_offset < file_header_size ||
            long_word(archive, static_cast<std::size_t>(where.header_offset)) != file_mark)
        {
            return failed("its data is not where the directory says");
        }
        auto const header = static_cast<std::size_t>(where.header_offset);
        std::size_t const data = header + file_header_size + word(archive, header + 26) + word(archive, header + 28);
        if (data > archive.size() || archive.size() - data < where.packed_size)
        {
            return failed("its data lies outside the file");
        }
        std::span<std::uint8_t const> const packed = archive.subspan(data, static_cast<std::size_t>(where.packed_size));
        std::vector<std::uint8_t> contents;
        if (where.method == method_stored)
        {
            if (where.packed_size != entry.size)
            {
                return failed("it is stored as it is, yet its two sizes differ");
            }
            contents.assign(packed.begin(), packed.end());
        }
        else
        {
            auto unpacked = inflate(packed, static_cast<std::size_t>(entry.size));
            if (!unpacked)
            {
                return failed(unpacked.error().describe());
            }
            contents = std::move(*unpacked);
        }
        if (std::uint32_t const found = crc32(contents); found != where.crc)
        {
            return failed(std::format("its checksum is {:08x}, the archive says {:08x}", found, where.crc));
        }
        return contents;
    }
}
