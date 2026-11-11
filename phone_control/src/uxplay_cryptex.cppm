// SPDX-License-Identifier: GPL-3.0-or-later
// cryptexd: the phone's service that installs "cryptexes" - signed, sealed
// add-ons to the system.  From iOS 27 on the Developer Disk Image (the image
// that brings the developer services remote control relies on) is one: Xcode
// installs it through this service, and so does this module.
//
// A request names a routine and its arguments; the phone answers with the
// result or with its own description of what went wrong.  Every call opens a
// connection of its own.
//
// The requests follow what the pymobiledevice3 project found out about them
// (https://github.com/doronz88/pymobiledevice3, GPL-3.0).
export module uxplay_cryptex;

import std;
import uxplay_status;
import uxplay_remote_xpc;
import uxplay_core_device;

export namespace uxplay
{
    // the name the Developer Disk Image is installed under
    inline constexpr std::string_view developer_image_cryptex = "com.apple.MobileAsset.DDI";

    struct installed_cryptex
    {
        std::string identifier;
        std::string version;
    };

    // What identifies the phone's chip to Apple's signing service.  The
    // unique number is the phone's own: it is never logged.
    struct chip_identity
    {
        std::uint64_t chip = 0;
        std::uint64_t unique_number = 0;
        bool production = true;
    };

    // What an installation consists of.  The spans must stay valid during the call.
    struct cryptex_parts
    {
        std::span<std::uint8_t const> image;
        std::span<std::uint8_t const> trust_cache;
        std::span<std::uint8_t const> ticket; // Apple's signature for this phone and its current nonce
        std::span<std::uint8_t const> info;
        std::span<std::uint8_t const> volume_hash;
        // of the image's build identity
        bool use_product_class = true;
        std::uint64_t sub_type = 0;
        std::uint64_t nonce_domain = 0;
        std::string version;
        std::string preauthorization_version;
    };

    [[nodiscard]] bool offers_cryptexes(service_directory const &directory);
    [[nodiscard]] outcome<std::vector<installed_cryptex>> installed_cryptexes(service_directory &directory);
    [[nodiscard]] outcome<chip_identity> read_chip_identity(service_directory &directory);
    // The nonce of one of the phone's nonce domains, named by its handle (a
    // build identity's "Cryptex1,NonceDomain"): what a signature is bound to.
    [[nodiscard]] outcome<std::vector<std::uint8_t>> cryptex_nonce(service_directory &directory,
                                                                    std::uint64_t domain_handle);
    // Sends the parts and waits for the phone to say that the cryptex is
    // installed (it stays installed until the next update of iOS).  The phone
    // gives no answer at all in some failures: `patience` ends the wait.
    [[nodiscard]] status install_cryptex(service_directory &directory, cryptex_parts const &parts,
                                         std::chrono::milliseconds patience);
}
