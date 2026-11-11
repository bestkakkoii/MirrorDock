// SPDX-License-Identifier: GPL-3.0-or-later
module;

#include <openssl/evp.h>

module uxplay_developer_image;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_property_list;
import uxplay_http_client;
import uxplay_core_device;
import uxplay_cryptex;

// The signing request and the reading of the build manifest follow what the
// pymobiledevice3 project found out about them
// (https://github.com/doronz88/pymobiledevice3, GPL-3.0).
namespace uxplay
{
    namespace
    {
        // ---- where the image comes from ------------------------------------------------------
        // The images Xcode ships, as the DeveloperDiskImage repository of the
        // pymobiledevice3 project publishes them: https://github.com/doronz88/DeveloperDiskImage,
        // branch main at the commit of 2026-11-11 ("Update DDIs to 27B52").  The hashes are
        // of the files at that commit; a file with another hash is not used.
        constexpr std::string_view image_source =
            "https://raw.githubusercontent.com/doronz88/DeveloperDiskImage/"
            "a1020f81b6429f5ce40408cbe21c690512b613b5/PersonalizedImages/Xcode_iOS_DDI_Cryptex/";

        struct image_file
        {
            std::string_view name;
            std::size_t size;
            std::string_view sha256;
        };

        constexpr image_file manifest_file{"BuildManifest.plist", 804097,
                                           "537391eb2f37e604fc693f422c1568af97220067853e663391ba3e8258569d6e"};
        constexpr std::array payload_files{
            image_file{"Image.dmg", 17239040, "500d4139f7963650e6c796d950267ed38909e7d183ad26bba073257529bfe173"},
            image_file{"Image.dmg.trustcache", 1851,
                       "2719aa3f1c99245432512116fb8a56d55ec224c706e545772d4c17c1233ac475"},
            image_file{"Image.dmg.cryptex_info", 428,
                       "0180ee20ce5daa7978fe38f24c742361726789ea6af82c3c1d2afc480734a531"},
            image_file{"Image.dmg.root_hash", 229,
                       "5f3281961396be5fe008e11641ffc25c7db24c11d3789ff0c4ff0fa6d47b94ca"},
        };

        // the one build identity (of about 140) that describes the image as a cryptex
        constexpr std::string_view cryptex_variant = "Developer Disk Image Cryptex";
        // what the parts are called in that identity's manifest
        constexpr std::string_view image_key = "Cryptex1,GenericDmg";
        constexpr std::string_view trust_cache_key = "Cryptex1,GenericTrustCache";
        constexpr std::string_view info_key = "Cryptex1,CryptexInfoPlist";
        constexpr std::string_view volume_hash_key = "Cryptex1,GenericVolume";
        constexpr std::string_view cryptex_prefix = "Cryptex1,";

        // ---- Apple's signing service ---------------------------------------------------------
        constexpr std::string_view signing_service = "https://gs.apple.com/TSS/controller?action=2";
        // what Apple's own tools say of themselves there
        constexpr std::string_view signing_agent = "InetURL/1.0";
        constexpr std::string_view signing_client = "libauthinstall-1104.0.9";
        constexpr std::size_t longest_signing_answer = 4 * 1024 * 1024;

        // The signing service's certificate comes from Apple's own authority,
        // which Windows does not trust by itself.  This is that authority's
        // root ("Apple Root CA", valid until 2035) as Apple publishes it at
        // https://www.apple.com/appleca/AppleIncRootCertificate.cer
        // (SHA-256 b0b1730ecbc7ff4505142c49f1295e6eda6bcaed7e2c68c5be91b5a11001f024):
        // the signing request goes to a server whose certificate leads to it and to no other.
        constexpr auto apple_root_certificate = std::to_array<std::uint8_t>({
            0x30, 0x82, 0x04, 0xBB, 0x30, 0x82, 0x03, 0xA3, 0xA0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x01, 0x02,
            0x30, 0x0D, 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x05, 0x05, 0x00, 0x30,
            0x62, 0x31, 0x0B, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06, 0x13, 0x02, 0x55, 0x53, 0x31, 0x13,
            0x30, 0x11, 0x06, 0x03, 0x55, 0x04, 0x0A, 0x13, 0x0A, 0x41, 0x70, 0x70, 0x6C, 0x65, 0x20, 0x49,
            0x6E, 0x63, 0x2E, 0x31, 0x26, 0x30, 0x24, 0x06, 0x03, 0x55, 0x04, 0x0B, 0x13, 0x1D, 0x41, 0x70,
            0x70, 0x6C, 0x65, 0x20, 0x43, 0x65, 0x72, 0x74, 0x69, 0x66, 0x69, 0x63, 0x61, 0x74, 0x69, 0x6F,
            0x6E, 0x20, 0x41, 0x75, 0x74, 0x68, 0x6F, 0x72, 0x69, 0x74, 0x79, 0x31, 0x16, 0x30, 0x14, 0x06,
            0x03, 0x55, 0x04, 0x03, 0x13, 0x0D, 0x41, 0x70, 0x70, 0x6C, 0x65, 0x20, 0x52, 0x6F, 0x6F, 0x74,
            0x20, 0x43, 0x41, 0x30, 0x1E, 0x17, 0x0D, 0x30, 0x36, 0x30, 0x34, 0x32, 0x35, 0x32, 0x31, 0x34,
            0x30, 0x33, 0x36, 0x5A, 0x17, 0x0D, 0x33, 0x35, 0x30, 0x32, 0x30, 0x39, 0x32, 0x31, 0x34, 0x30,
            0x33, 0x36, 0x5A, 0x30, 0x62, 0x31, 0x0B, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06, 0x13, 0x02,
            0x55, 0x53, 0x31, 0x13, 0x30, 0x11, 0x06, 0x03, 0x55, 0x04, 0x0A, 0x13, 0x0A, 0x41, 0x70, 0x70,
            0x6C, 0x65, 0x20, 0x49, 0x6E, 0x63, 0x2E, 0x31, 0x26, 0x30, 0x24, 0x06, 0x03, 0x55, 0x04, 0x0B,
            0x13, 0x1D, 0x41, 0x70, 0x70, 0x6C, 0x65, 0x20, 0x43, 0x65, 0x72, 0x74, 0x69, 0x66, 0x69, 0x63,
            0x61, 0x74, 0x69, 0x6F, 0x6E, 0x20, 0x41, 0x75, 0x74, 0x68, 0x6F, 0x72, 0x69, 0x74, 0x79, 0x31,
            0x16, 0x30, 0x14, 0x06, 0x03, 0x55, 0x04, 0x03, 0x13, 0x0D, 0x41, 0x70, 0x70, 0x6C, 0x65, 0x20,
            0x52, 0x6F, 0x6F, 0x74, 0x20, 0x43, 0x41, 0x30, 0x82, 0x01, 0x22, 0x30, 0x0D, 0x06, 0x09, 0x2A,
            0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01, 0x05, 0x00, 0x03, 0x82, 0x01, 0x0F, 0x00, 0x30,
            0x82, 0x01, 0x0A, 0x02, 0x82, 0x01, 0x01, 0x00, 0xE4, 0x91, 0xA9, 0x09, 0x1F, 0x91, 0xDB, 0x1E,
            0x47, 0x50, 0xEB, 0x05, 0xED, 0x5E, 0x79, 0x84, 0x2D, 0xEB, 0x36, 0xA2, 0x57, 0x4C, 0x55, 0xEC,
            0x8B, 0x19, 0x89, 0xDE, 0xF9, 0x4B, 0x6C, 0xF5, 0x07, 0xAB, 0x22, 0x30, 0x02, 0xE8, 0x18, 0x3E,
            0xF8, 0x50, 0x09, 0xD3, 0x7F, 0x41, 0xA8, 0x98, 0xF9, 0xD1, 0xCA, 0x66, 0x9C, 0x24, 0x6B, 0x11,
            0xD0, 0xA3, 0xBB, 0xE4, 0x1B, 0x2A, 0xC3, 0x1F, 0x95, 0x9E, 0x7A, 0x0C, 0xA4, 0x47, 0x8B, 0x5B,
            0xD4, 0x16, 0x37, 0x33, 0xCB, 0xC4, 0x0F, 0x4D, 0xCE, 0x14, 0x69, 0xD1, 0xC9, 0x19, 0x72, 0xF5,
            0x5D, 0x0E, 0xD5, 0x7F, 0x5F, 0x9B, 0xF2, 0x25, 0x03, 0xBA, 0x55, 0x8F, 0x4D, 0x5D, 0x0D, 0xF1,
            0x64, 0x35, 0x23, 0x15, 0x4B, 0x15, 0x59, 0x1D, 0xB3, 0x94, 0xF7, 0xF6, 0x9C, 0x9E, 0xCF, 0x50,
            0xBA, 0xC1, 0x58, 0x50, 0x67, 0x8F, 0x08, 0xB4, 0x20, 0xF7, 0xCB, 0xAC, 0x2C, 0x20, 0x6F, 0x70,
            0xB6, 0x3F, 0x01, 0x30, 0x8C, 0xB7, 0x43, 0xCF, 0x0F, 0x9D, 0x3D, 0xF3, 0x2B, 0x49, 0x28, 0x1A,
            0xC8, 0xFE, 0xCE, 0xB5, 0xB9, 0x0E, 0xD9, 0x5E, 0x1C, 0xD6, 0xCB, 0x3D, 0xB5, 0x3A, 0xAD, 0xF4,
            0x0F, 0x0E, 0x00, 0x92, 0x0B, 0xB1, 0x21, 0x16, 0x2E, 0x74, 0xD5, 0x3C, 0x0D, 0xDB, 0x62, 0x16,
            0xAB, 0xA3, 0x71, 0x92, 0x47, 0x53, 0x55, 0xC1, 0xAF, 0x2F, 0x41, 0xB3, 0xF8, 0xFB, 0xE3, 0x70,
            0xCD, 0xE6, 0xA3, 0x4C, 0x45, 0x7E, 0x1F, 0x4C, 0x6B, 0x50, 0x96, 0x41, 0x89, 0xC4, 0x74, 0x62,
            0x0B, 0x10, 0x83, 0x41, 0x87, 0x33, 0x8A, 0x81, 0xB1, 0x30, 0x58, 0xEC, 0x5A, 0x04, 0x32, 0x8C,
            0x68, 0xB3, 0x8F, 0x1D, 0xDE, 0x65, 0x73, 0xFF, 0x67, 0x5E, 0x65, 0xBC, 0x49, 0xD8, 0x76, 0x9F,
            0x33, 0x14, 0x65, 0xA1, 0x77, 0x94, 0xC9, 0x2D, 0x02, 0x03, 0x01, 0x00, 0x01, 0xA3, 0x82, 0x01,
            0x7A, 0x30, 0x82, 0x01, 0x76, 0x30, 0x0E, 0x06, 0x03, 0x55, 0x1D, 0x0F, 0x01, 0x01, 0xFF, 0x04,
            0x04, 0x03, 0x02, 0x01, 0x06, 0x30, 0x0F, 0x06, 0x03, 0x55, 0x1D, 0x13, 0x01, 0x01, 0xFF, 0x04,
            0x05, 0x30, 0x03, 0x01, 0x01, 0xFF, 0x30, 0x1D, 0x06, 0x03, 0x55, 0x1D, 0x0E, 0x04, 0x16, 0x04,
            0x14, 0x2B, 0xD0, 0x69, 0x47, 0x94, 0x76, 0x09, 0xFE, 0xF4, 0x6B, 0x8D, 0x2E, 0x40, 0xA6, 0xF7,
            0x47, 0x4D, 0x7F, 0x08, 0x5E, 0x30, 0x1F, 0x06, 0x03, 0x55, 0x1D, 0x23, 0x04, 0x18, 0x30, 0x16,
            0x80, 0x14, 0x2B, 0xD0, 0x69, 0x47, 0x94, 0x76, 0x09, 0xFE, 0xF4, 0x6B, 0x8D, 0x2E, 0x40, 0xA6,
            0xF7, 0x47, 0x4D, 0x7F, 0x08, 0x5E, 0x30, 0x82, 0x01, 0x11, 0x06, 0x03, 0x55, 0x1D, 0x20, 0x04,
            0x82, 0x01, 0x08, 0x30, 0x82, 0x01, 0x04, 0x30, 0x82, 0x01, 0x00, 0x06, 0x09, 0x2A, 0x86, 0x48,
            0x86, 0xF7, 0x63, 0x64, 0x05, 0x01, 0x30, 0x81, 0xF2, 0x30, 0x2A, 0x06, 0x08, 0x2B, 0x06, 0x01,
            0x05, 0x05, 0x07, 0x02, 0x01, 0x16, 0x1E, 0x68, 0x74, 0x74, 0x70, 0x73, 0x3A, 0x2F, 0x2F, 0x77,
            0x77, 0x77, 0x2E, 0x61, 0x70, 0x70, 0x6C, 0x65, 0x2E, 0x63, 0x6F, 0x6D, 0x2F, 0x61, 0x70, 0x70,
            0x6C, 0x65, 0x63, 0x61, 0x2F, 0x30, 0x81, 0xC3, 0x06, 0x08, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07,
            0x02, 0x02, 0x30, 0x81, 0xB6, 0x1A, 0x81, 0xB3, 0x52, 0x65, 0x6C, 0x69, 0x61, 0x6E, 0x63, 0x65,
            0x20, 0x6F, 0x6E, 0x20, 0x74, 0x68, 0x69, 0x73, 0x20, 0x63, 0x65, 0x72, 0x74, 0x69, 0x66, 0x69,
            0x63, 0x61, 0x74, 0x65, 0x20, 0x62, 0x79, 0x20, 0x61, 0x6E, 0x79, 0x20, 0x70, 0x61, 0x72, 0x74,
            0x79, 0x20, 0x61, 0x73, 0x73, 0x75, 0x6D, 0x65, 0x73, 0x20, 0x61, 0x63, 0x63, 0x65, 0x70, 0x74,
            0x61, 0x6E, 0x63, 0x65, 0x20, 0x6F, 0x66, 0x20, 0x74, 0x68, 0x65, 0x20, 0x74, 0x68, 0x65, 0x6E,
            0x20, 0x61, 0x70, 0x70, 0x6C, 0x69, 0x63, 0x61, 0x62, 0x6C, 0x65, 0x20, 0x73, 0x74, 0x61, 0x6E,
            0x64, 0x61, 0x72, 0x64, 0x20, 0x74, 0x65, 0x72, 0x6D, 0x73, 0x20, 0x61, 0x6E, 0x64, 0x20, 0x63,
            0x6F, 0x6E, 0x64, 0x69, 0x74, 0x69, 0x6F, 0x6E, 0x73, 0x20, 0x6F, 0x66, 0x20, 0x75, 0x73, 0x65,
            0x2C, 0x20, 0x63, 0x65, 0x72, 0x74, 0x69, 0x66, 0x69, 0x63, 0x61, 0x74, 0x65, 0x20, 0x70, 0x6F,
            0x6C, 0x69, 0x63, 0x79, 0x20, 0x61, 0x6E, 0x64, 0x20, 0x63, 0x65, 0x72, 0x74, 0x69, 0x66, 0x69,
            0x63, 0x61, 0x74, 0x69, 0x6F, 0x6E, 0x20, 0x70, 0x72, 0x61, 0x63, 0x74, 0x69, 0x63, 0x65, 0x20,
            0x73, 0x74, 0x61, 0x74, 0x65, 0x6D, 0x65, 0x6E, 0x74, 0x73, 0x2E, 0x30, 0x0D, 0x06, 0x09, 0x2A,
            0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x05, 0x05, 0x00, 0x03, 0x82, 0x01, 0x01, 0x00, 0x5C,
            0x36, 0x99, 0x4C, 0x2D, 0x78, 0xB7, 0xED, 0x8C, 0x9B, 0xDC, 0xF3, 0x77, 0x9B, 0xF2, 0x76, 0xD2,
            0x77, 0x30, 0x4F, 0xC1, 0x1F, 0x85, 0x83, 0x85, 0x1B, 0x99, 0x3D, 0x47, 0x37, 0xF2, 0xA9, 0x9B,
            0x40, 0x8E, 0x2C, 0xD4, 0xB1, 0x90, 0x12, 0xD8, 0xBE, 0xF4, 0x73, 0x9B, 0xEE, 0xD2, 0x64, 0x0F,
            0xCB, 0x79, 0x4F, 0x34, 0xD8, 0xA2, 0x3E, 0xF9, 0x78, 0xFF, 0x6B, 0xC8, 0x07, 0xEC, 0x7D, 0x39,
            0x83, 0x8B, 0x53, 0x20, 0xD3, 0x38, 0xC4, 0xB1, 0xBF, 0x9A, 0x4F, 0x0A, 0x6B, 0xFF, 0x2B, 0xFC,
            0x59, 0xA7, 0x05, 0x09, 0x7C, 0x17, 0x40, 0x56, 0x11, 0x1E, 0x74, 0xD3, 0xB7, 0x8B, 0x23, 0x3B,
            0x47, 0xA3, 0xD5, 0x6F, 0x24, 0xE2, 0xEB, 0xD1, 0xB7, 0x70, 0xDF, 0x0F, 0x45, 0xE1, 0x27, 0xCA,
            0xF1, 0x6D, 0x78, 0xED, 0xE7, 0xB5, 0x17, 0x17, 0xA8, 0xDC, 0x7E, 0x22, 0x35, 0xCA, 0x25, 0xD5,
            0xD9, 0x0F, 0xD6, 0x6B, 0xD4, 0xA2, 0x24, 0x23, 0x11, 0xF7, 0xA1, 0xAC, 0x8F, 0x73, 0x81, 0x60,
            0xC6, 0x1B, 0x5B, 0x09, 0x2F, 0x92, 0xB2, 0xF8, 0x44, 0x48, 0xF0, 0x60, 0x38, 0x9E, 0x15, 0xF5,
            0x3D, 0x26, 0x67, 0x20, 0x8A, 0x33, 0x6A, 0xF7, 0x0D, 0x82, 0xCF, 0xDE, 0xEB, 0xA3, 0x2F, 0xF9,
            0x53, 0x6A, 0x5B, 0x64, 0xC0, 0x63, 0x33, 0x77, 0xF7, 0x3A, 0x07, 0x2C, 0x56, 0xEB, 0xDA, 0x0F,
            0x21, 0x0E, 0xDA, 0xBA, 0x73, 0x19, 0x4F, 0xB5, 0xD9, 0x36, 0x7F, 0xC1, 0x87, 0x55, 0xD9, 0xA7,
            0x99, 0xB9, 0x32, 0x42, 0xFB, 0xD8, 0xD5, 0x71, 0x9E, 0x7E, 0xA1, 0x52, 0xB7, 0x1B, 0xBD, 0x93,
            0x42, 0x24, 0x12, 0x2A, 0xC7, 0x0F, 0x1D, 0xB6, 0x4D, 0x9C, 0x5E, 0x63, 0xC8, 0x4B, 0x80, 0x17,
            0x50, 0xAA, 0x8A, 0xD5, 0xDA, 0xE4, 0xFC, 0xD0, 0x09, 0x07, 0x37, 0xB0, 0x75, 0x75, 0x21,
        });

        // the phone confirms an installation within seconds; in some failures it says nothing at all
        constexpr auto install_patience = std::chrono::seconds{90};

        [[nodiscard]] std::string sha256_text(std::span<std::uint8_t const> bytes)
        {
            std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
            unsigned int length = 0;
            if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1)
            {
                return {};
            }
            std::string text;
            for (unsigned int index = 0; index < length; ++index)
            {
                std::format_to(std::back_inserter(text), "{:02x}", digest[index]);
            }
            return text;
        }

        [[nodiscard]] bool is_expected(image_file const &file, std::span<std::uint8_t const> bytes)
        {
            return bytes.size() == file.size && sha256_text(bytes) == file.sha256;
        }

        [[nodiscard]] std::optional<std::vector<std::uint8_t>> read_file(std::filesystem::path const &path)
        {
            std::ifstream source{path, std::ios::binary};
            if (!source)
            {
                return std::nullopt;
            }
            std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{source}, std::istreambuf_iterator<char>{}};
            return bytes;
        }

        // (not being able to keep a part costs a download next time, nothing else: said in the log)
        void keep_file(std::filesystem::path const &path, std::span<std::uint8_t const> bytes)
        {
            std::error_code problem;
            std::filesystem::create_directories(path.parent_path(), problem);
            std::filesystem::path partial = path;
            partial += ".part";
            {
                std::ofstream target{partial, std::ios::binary | std::ios::trunc};
                target.write(reinterpret_cast<char const *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!target)
                {
                    log::warning("developer image: {} could not be written to the cache", path.filename().string());
                    return;
                }
            }
            std::filesystem::rename(partial, path, problem);
            if (problem)
            {
                log::warning("developer image: {} could not be kept in the cache ({})", path.filename().string(),
                             problem.message());
            }
        }

        // one of the image's files: from the cache when it is there and whole, from the internet otherwise
        [[nodiscard]] outcome<std::vector<std::uint8_t>> obtain(image_file const &file,
                                                                std::filesystem::path const &cache,
                                                                cancel_check const &cancelled)
        {
            if (!cache.empty())
            {
                if (std::optional<std::vector<std::uint8_t>> kept = read_file(cache / file.name);
                    kept && is_expected(file, *kept))
                {
                    return std::move(*kept);
                }
            }
            log::info("developer image: fetching {} ({} bytes)", file.name, file.size);
            http_request request;
            request.url = std::format("{}{}", image_source, file.name);
            request.longest_answer = file.size;
            request.cancelled = cancelled;
            outcome<http_answer> answer = http_exchange(request);
            if (!answer)
            {
                return std::unexpected(make_failure(std::format("download of {}", file.name), answer.error().code,
                                                    answer.error().describe()));
            }
            if (answer->status != 200)
            {
                return std::unexpected(make_failure(std::format("download of {}", file.name), answer->status,
                                                    "the server answered with this HTTP status"));
            }
            if (!is_expected(file, answer->body))
            {
                return std::unexpected(make_failure(std::format("download of {}", file.name),
                                                    static_cast<std::int64_t>(answer->body.size()),
                                                    "what was received is not the expected file (size, hash)"));
            }
            if (!cache.empty())
            {
                keep_file(cache / file.name, answer->body);
            }
            return std::move(answer->body);
        }

        // "0xFF10" and 65296 alike (the manifest has numbers in both forms)
        [[nodiscard]] std::optional<std::uint64_t> number_of(property_node node)
        {
            if (std::optional<std::uint64_t> const number = node.number(); number)
            {
                return number;
            }
            std::optional<std::string> const text = node.text();
            if (!text || text->empty())
            {
                return std::nullopt;
            }
            std::string_view digits{*text};
            int base = 10;
            if (digits.starts_with("0x") || digits.starts_with("0X"))
            {
                digits.remove_prefix(2);
                base = 16;
            }
            std::uint64_t value = 0;
            auto const parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
            if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
            {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]] failure manifest_failure(std::string_view what)
        {
            return make_failure("developer image manifest", 0, what);
        }

        void put_be64(std::vector<std::uint8_t> &target, std::uint64_t value)
        {
            for (int shift = 56; shift >= 0; shift -= 8)
            {
                target.push_back(static_cast<std::uint8_t>(value >> shift));
            }
        }
    }

    outcome<developer_image> load_developer_image(std::filesystem::path const &cache, cancel_check const &cancelled)
    {
        outcome<std::vector<std::uint8_t>> const manifest_bytes = obtain(manifest_file, cache, cancelled);
        if (!manifest_bytes)
        {
            return std::unexpected(manifest_bytes.error());
        }
        outcome<property_list> const manifest = property_list::parse(*manifest_bytes);
        if (!manifest)
        {
            return std::unexpected(manifest.error());
        }
        developer_image image;
        image.build = manifest->root().item("ProductBuildVersion").text().value_or("");

        property_node const identities = manifest->root().item("BuildIdentities");
        property_node identity;
        for (std::size_t index = 0; index < identities.size(); ++index)
        {
            property_node const candidate = identities.element(index);
            if (candidate.item("Info").item("Variant").text().value_or("").ends_with(cryptex_variant))
            {
                identity = candidate;
                break;
            }
        }
        if (!identity.present())
        {
            return std::unexpected(manifest_failure("no build identity describes the image as a cryptex"));
        }
        std::optional<std::uint64_t> const chip = number_of(identity.item("Cryptex1,ChipID"));
        std::optional<std::uint64_t> const type = number_of(identity.item("Cryptex1,Type"));
        std::optional<std::uint64_t> const sub_type = number_of(identity.item("Cryptex1,SubType"));
        std::optional<std::uint64_t> const product_class = number_of(identity.item("Cryptex1,ProductClass"));
        std::optional<std::uint64_t> const nonce_domain = number_of(identity.item("Cryptex1,NonceDomain"));
        std::optional<bool> const use_product_class = identity.item("Cryptex1,UseProductClass").flag();
        std::optional<std::string> version = identity.item("Cryptex1,Version").text();
        std::optional<std::string> preauthorization = identity.item("Cryptex1,PreauthorizationVersion").text();
        if (!chip || !type || !sub_type || !product_class || !nonce_domain || !use_product_class || !version ||
            !preauthorization)
        {
            return std::unexpected(manifest_failure("the cryptex identity lacks one of its properties"));
        }
        image.chip = *chip;
        image.type = *type;
        image.sub_type = *sub_type;
        image.product_class = *product_class;
        image.nonce_domain = *nonce_domain;
        image.use_product_class = *use_product_class;
        image.version = std::move(*version);
        image.preauthorization_version = std::move(*preauthorization);

        property_node const parts = identity.item("Manifest");
        for (std::string const &key : parts.keys())
        {
            if (!key.starts_with(cryptex_prefix))
            {
                continue;
            }
            property_node const part = parts.item(key);
            // the image itself is the same for every phone: it is not among what Apple signs
            if (part.item("Info").item("Personalize").flag().value_or(false))
            {
                std::optional<std::vector<std::uint8_t>> digest = part.item("Digest").data();
                if (!digest)
                {
                    return std::unexpected(manifest_failure(std::format("{} has no digest", key)));
                }
                image.signed_parts.emplace_back(key, std::move(*digest));
            }
        }

        struct payload
        {
            std::string_view key;
            std::vector<std::uint8_t> *target;
        };
        for (payload const &wanted :
             {payload{image_key, &image.image}, payload{trust_cache_key, &image.trust_cache},
              payload{info_key, &image.info}, payload{volume_hash_key, &image.volume_hash}})
        {
            // the manifest says which file holds the part
            std::string const path = parts.item(wanted.key).item("Info").item("Path").text().value_or("");
            auto const file = std::ranges::find(payload_files, std::string_view{path}, &image_file::name);
            if (file == payload_files.end())
            {
                return std::unexpected(
                    manifest_failure(std::format("{} is in a file this program does not know", wanted.key)));
            }
            outcome<std::vector<std::uint8_t>> bytes = obtain(*file, cache, cancelled);
            if (!bytes)
            {
                return std::unexpected(bytes.error());
            }
            *wanted.target = std::move(*bytes);
        }
        return image;
    }

    outcome<std::vector<std::uint8_t>> request_image_ticket(developer_image const &image, chip_identity const &chip,
                                                            std::span<std::uint8_t const> nonce,
                                                            cancel_check const &cancelled)
    {
        // the phone is named by the number of its chip model and its own unique number
        std::vector<std::uint8_t> device;
        put_be64(device, chip.chip);
        put_be64(device, chip.unique_number);

        property_list request;
        request.set_text("@HostPlatformInfo", "mac");
        request.set_text("@VersionInfo", signing_client);
        request.set_text("@UUID", uuid_text(new_uuid()));
        request.set_flag("@Cryptex1,Ticket", true);
        request.set_number("Cryptex1,ChipID", image.chip);
        request.set_number("Cryptex1,Type", image.type);
        request.set_number("Cryptex1,SubType", image.sub_type);
        request.set_number("Cryptex1,ProductClass", image.product_class);
        request.set_flag("Cryptex1,UseProductClass", image.use_product_class);
        request.set_number("Cryptex1,NonceDomain", image.nonce_domain);
        request.set_text("Cryptex1,Version", image.version);
        request.set_text("Cryptex1,PreauthorizationVersion", image.preauthorization_version);
        request.set_data("Cryptex1,Nonce", nonce);
        request.set_flag("Cryptex1,ProductionMode", chip.production);
        request.set_data("Cryptex1,UDID", device);
        request.set_data("Cryptex1,UniqueTagList", {});
        for (auto const &[key, digest] : image.signed_parts)
        {
            property_list part;
            part.set_data("Digest", digest);
            request.set_list(key, std::move(part));
        }
        outcome<std::string> const body = request.to_xml();
        if (!body)
        {
            return std::unexpected(body.error());
        }

        http_request exchange;
        exchange.url = std::string{signing_service};
        exchange.user_agent = std::string{signing_agent};
        exchange.content_type = "text/xml; charset=\"utf-8\"";
        exchange.body = std::span{reinterpret_cast<std::uint8_t const *>(body->data()), body->size()};
        exchange.longest_answer = longest_signing_answer;
        exchange.cancelled = cancelled;
        exchange.only_trusted_root = apple_root_certificate;
        outcome<http_answer> const answer = http_exchange(exchange);
        if (!answer)
        {
            return std::unexpected(
                make_failure("Apple's signing service", answer.error().code, answer.error().describe()));
        }
        // "STATUS=0&MESSAGE=SUCCESS&REQUEST_STRING=<property list>"
        std::string_view const text{reinterpret_cast<char const *>(answer->body.data()), answer->body.size()};
        constexpr std::string_view message_mark = "MESSAGE=";
        constexpr std::string_view list_mark = "REQUEST_STRING=";
        std::size_t const message_at = text.find(message_mark);
        if (answer->status != 200 || message_at == std::string_view::npos)
        {
            return std::unexpected(make_failure("Apple's signing service", answer->status,
                                                "an answer of this HTTP status that is none of the service's"));
        }
        std::string_view message = text.substr(message_at + message_mark.size());
        message = message.substr(0, message.find('&'));
        if (message != "SUCCESS")
        {
            return std::unexpected(make_failure("Apple's signing service", 0,
                                                std::format("the request was refused: {}", message.substr(0, 200))));
        }
        std::size_t const list_at = text.find(list_mark);
        if (list_at == std::string_view::npos)
        {
            return std::unexpected(make_failure("Apple's signing service", 0, "the answer holds no signature"));
        }
        outcome<property_list> const signed_list =
            property_list::parse(std::span<std::uint8_t const>{answer->body}.subspan(list_at + list_mark.size()));
        if (!signed_list)
        {
            return std::unexpected(signed_list.error());
        }
        std::optional<std::vector<std::uint8_t>> ticket = signed_list->root().item("Cryptex1,Ticket").data();
        if (!ticket)
        {
            ticket = signed_list->root().item("ApImg4Ticket").data();
        }
        if (!ticket || ticket->empty())
        {
            return std::unexpected(make_failure("Apple's signing service", 0, "the answer holds no signature"));
        }
        return std::move(*ticket);
    }

    outcome<bool> ensure_developer_image(service_directory &directory, std::filesystem::path const &cache,
                                         std::function<void(std::string_view)> const &progress,
                                         cancel_check const &cancelled)
    {
        auto const installed = [&directory]() -> outcome<bool>
        {
            outcome<std::vector<installed_cryptex>> const list = installed_cryptexes(directory);
            if (!list)
            {
                return std::unexpected(list.error());
            }
            return std::ranges::contains(*list, developer_image_cryptex, &installed_cryptex::identifier);
        };
        auto const say = [&progress](std::string_view words)
        {
            if (progress)
            {
                progress(words);
            }
        };

        outcome<bool> const there = installed();
        if (!there)
        {
            return std::unexpected(there.error());
        }
        if (*there)
        {
            return false;
        }
        say("fetching the Developer Disk Image");
        outcome<developer_image> const image = load_developer_image(cache, cancelled);
        if (!image)
        {
            return std::unexpected(image.error());
        }
        outcome<chip_identity> const chip = read_chip_identity(directory);
        if (!chip)
        {
            return std::unexpected(chip.error());
        }
        outcome<std::vector<std::uint8_t>> const nonce = cryptex_nonce(directory, image->nonce_domain);
        if (!nonce)
        {
            return std::unexpected(nonce.error());
        }
        say("asking Apple's signing service to sign the Developer Disk Image for this iPhone");
        outcome<std::vector<std::uint8_t>> const ticket = request_image_ticket(*image, *chip, *nonce, cancelled);
        if (!ticket)
        {
            return std::unexpected(ticket.error());
        }
        if (cancelled && cancelled())
        {
            return std::unexpected(make_failure("developer image", 0, "ended here before the installation"));
        }
        say(std::format("installing the Developer Disk Image {} on the iPhone ({} bytes)", image->build,
                        image->image.size()));
        cryptex_parts parts;
        parts.image = image->image;
        parts.trust_cache = image->trust_cache;
        parts.ticket = *ticket;
        parts.info = image->info;
        parts.volume_hash = image->volume_hash;
        parts.use_product_class = image->use_product_class;
        parts.sub_type = image->sub_type;
        parts.nonce_domain = image->nonce_domain;
        parts.version = image->version;
        parts.preauthorization_version = image->preauthorization_version;
        if (status const done = install_cryptex(directory, parts, install_patience); !done)
        {
            return std::unexpected(done.error());
        }
        outcome<bool> const now = installed();
        if (!now)
        {
            return std::unexpected(now.error());
        }
        if (!*now)
        {
            return std::unexpected(make_failure("developer image", 0,
                                                "the phone confirmed the installation but does not list the image"));
        }
        return true;
    }
}
