// SPDX-License-Identifier: MIT
// Reads a zip archive that is in memory: the package of a release, for the
// update (uxplay_update_install).
//
// It reads what such a package is made of: files that are stored or deflated
// (RFC 1951), no encryption, no zip64, one disk.  Anything else is refused
// with a failure that says what it was.  Every file is checked against the
// CRC-32 the archive gives for it.  Names are handed out as they are written
// in the archive: whoever unpacks decides what a name may be.
export module uxplay_zip_archive;

import std;
import uxplay_status;

export namespace uxplay
{
    struct zip_entry
    {
        std::string name;       // "folder/file.txt"; the name of a folder ends with '/'
        std::uint64_t size = 0; // of the file when it is unpacked
        bool folder = false;
    };

    class zip_archive
    {
    public:
        // `bytes` is not copied: it must stay for as long as the object is used
        [[nodiscard]] static outcome<zip_archive> open(std::span<std::uint8_t const> bytes);

        [[nodiscard]] std::span<zip_entry const> entries() const;

        // the unpacked contents of entries()[index]
        [[nodiscard]] outcome<std::vector<std::uint8_t>> read(std::size_t index) const;

    private:
        // where an entry's data is, and what it must unpack to
        struct place
        {
            std::uint64_t header_offset = 0;
            std::uint64_t packed_size = 0;
            std::uint32_t crc = 0;
            std::uint16_t method = 0;
        };

        std::span<std::uint8_t const> archive;
        std::vector<zip_entry> names;
        std::vector<place> places;
    };

    // RFC 1951.  `size`: how many bytes must come out; more or fewer is a failure.
    [[nodiscard]] outcome<std::vector<std::uint8_t>> inflate(std::span<std::uint8_t const> deflated, std::size_t size);

    // the CRC-32 of zip, PNG and Ethernet
    [[nodiscard]] std::uint32_t crc32(std::span<std::uint8_t const> bytes);
}
