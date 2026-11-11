// SPDX-License-Identifier: GPL-3.0-or-later
// The Developer Disk Image: Apple's add-on to iOS (it ships with Xcode) that
// brings the developer services remote control relies on.  A phone gets it
// once; it stays until the next update of iOS.
//
// What installing it takes, as Xcode does it on iOS 27:
//   1. the image and its companions (a trust cache, an info list, a volume
//      hash and the build manifest that describes them) - not part of this
//      program: fetched from the internet when first needed, checked against
//      the hashes below and kept in a cache directory;
//   2. Apple's signature for them, made for this one phone: Apple's signing
//      service is told the phone's chip numbers and a nonce the phone made;
//   3. the phone's cryptex service (uxplay_cryptex), which takes all of it.
//
// So the first use contacts two hosts: raw.githubusercontent.com (the image)
// and gs.apple.com (the signature).  Later uses contact nobody.
export module uxplay_developer_image;

import std;
import uxplay_status;
import uxplay_core_device;
import uxplay_cryptex;

export namespace uxplay
{
    // the image and what its build manifest says about installing it
    struct developer_image
    {
        std::string build; // of the image, e.g. "27B52"
        std::vector<std::uint8_t> image;
        std::vector<std::uint8_t> trust_cache;
        std::vector<std::uint8_t> info;
        std::vector<std::uint8_t> volume_hash;
        // of the build identity that describes the image as a cryptex
        std::uint64_t chip = 0;
        std::uint64_t type = 0;
        std::uint64_t sub_type = 0;
        std::uint64_t product_class = 0;
        bool use_product_class = true;
        std::uint64_t nonce_domain = 0;
        std::string version;
        std::string preauthorization_version;
        // the parts Apple signs for one phone: their names in the manifest and their digests
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> signed_parts;
    };

    // asked now and then during a long step; true ends the step with a failure
    using cancel_check = std::function<bool()>;

    // The image out of `cache` (a directory; created when it is not there).
    // A part that is missing or does not have the expected hash is fetched
    // again.  Without a cache (an empty path) everything is fetched each time.
    [[nodiscard]] outcome<developer_image> load_developer_image(std::filesystem::path const &cache,
                                                                cancel_check const &cancelled);

    // Apple's signature (a "ticket") with which the phone of this chip accepts
    // the image while its nonce stays the one given.
    [[nodiscard]] outcome<std::vector<std::uint8_t>> request_image_ticket(developer_image const &image,
                                                                          chip_identity const &chip,
                                                                          std::span<std::uint8_t const> nonce,
                                                                          cancel_check const &cancelled);

    // Installs the Developer Disk Image on the phone unless it is installed
    // already; true when it had to be installed.  `progress` gets a few words
    // before each step that can take long (for the log).
    [[nodiscard]] outcome<bool> ensure_developer_image(service_directory &directory,
                                                       std::filesystem::path const &cache,
                                                       std::function<void(std::string_view)> const &progress,
                                                       cancel_check const &cancelled);
}
