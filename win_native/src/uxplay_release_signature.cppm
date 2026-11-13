// SPDX-License-Identifier: MIT
// Is this package a release the project's owner published?
//
// Every release package is signed with a key that is kept off the internet:
// ECDSA on the curve P-256 over the SHA-256 of the zip file, made by
// build_scripts/sign_release.ps1 and published beside the package as a text
// file of 128 hexadecimal digits (r, then s).  The program carries the public
// half of that key (release_key in the implementation) and installs nothing
// whose signature does not fit it (uxplay_update_install).  Windows' own
// cryptography (CNG) does the arithmetic.
export module uxplay_release_signature;

import std;
import uxplay_status;

export namespace uxplay
{
    struct signature_check
    {
        // the signature file is one: 128 hexadecimal digits (white space around them is allowed)
        bool well_formed = false;
        // and it is the release key's signature of the package
        bool fits = false;
        std::array<std::uint8_t, 32> digest{}; // the SHA-256 of the package
    };

    [[nodiscard]] outcome<std::array<std::uint8_t, 32>> sha256(std::span<std::uint8_t const> bytes);

    // lower-case hexadecimal digits, two a byte
    [[nodiscard]] std::string hexadecimal(std::span<std::uint8_t const> bytes);

    // package: the bytes of the zip; signature_file: the bytes of the file
    // published beside it.  A failure only when Windows' cryptography could
    // not be used; a signature that does not fit is a result, not a failure.
    [[nodiscard]] outcome<signature_check> check_release_signature(std::span<std::uint8_t const> package,
                                                                   std::span<std::uint8_t const> signature_file);
}
