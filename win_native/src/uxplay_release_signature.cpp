// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

module uxplay_release_signature;

import std;
import uxplay_status;

namespace uxplay
{
    namespace
    {
        // The public half of the key the project's releases are signed with:
        // the point (X, Y) on the curve P-256, as build_scripts/sign_release.ps1
        // prints it.  Whoever builds a program of their own that is to update
        // itself from releases of their own puts their own key here.
        constexpr std::array<std::uint8_t, 64> release_key{
            0x0c, 0xbf, 0x98, 0x86, 0x94, 0x8e, 0x21, 0xcb, 0xe2, 0x02, 0xce, 0xcf, 0x35, 0x1d, 0x51, 0xf6,
            0x2a, 0xbc, 0x03, 0x5e, 0xe6, 0x74, 0x1c, 0xc8, 0x32, 0x43, 0x4f, 0x7a, 0x37, 0x77, 0x24, 0xae,
            0x6f, 0xc5, 0xcc, 0xe8, 0xb3, 0x39, 0x29, 0x23, 0x94, 0x1a, 0x87, 0x46, 0x35, 0x6e, 0xf1, 0xda,
            0x55, 0xf7, 0xdc, 0x54, 0xce, 0xe4, 0xff, 0x46, 0xfa, 0x3d, 0x61, 0xf8, 0x2e, 0x34, 0x04, 0xca};

        constexpr NTSTATUS signature_does_not_fit = static_cast<NTSTATUS>(0xC000A000); // STATUS_INVALID_SIGNATURE

        [[nodiscard]] failure crypto_failure(std::string_view step, NTSTATUS code)
        {
            return make_failure(step, static_cast<std::int64_t>(static_cast<std::uint32_t>(code)),
                                "Windows' cryptography reports this status");
        }

        struct algorithm_provider
        {
            BCRYPT_ALG_HANDLE handle = nullptr;

            algorithm_provider() = default;
            algorithm_provider(algorithm_provider const &) = delete;
            algorithm_provider &operator=(algorithm_provider const &) = delete;

            ~algorithm_provider()
            {
                if (handle != nullptr)
                {
                    BCryptCloseAlgorithmProvider(handle, 0);
                }
            }
        };

        // the signature file as its 64 bytes; nothing when it is no signature
        [[nodiscard]] std::optional<std::array<std::uint8_t, 64>> signature_from(std::span<std::uint8_t const> text)
        {
            std::array<std::uint8_t, 64> signature{};
            std::size_t digits = 0;
            for (std::uint8_t const character : text)
            {
                int value = 0;
                if (character >= '0' && character <= '9')
                {
                    value = character - '0';
                }
                else if (character >= 'a' && character <= 'f')
                {
                    value = character - 'a' + 10;
                }
                else if (character >= 'A' && character <= 'F')
                {
                    value = character - 'A' + 10;
                }
                else if (character == ' ' || character == '\t' || character == '\r' || character == '\n')
                {
                    continue;
                }
                else
                {
                    return std::nullopt;
                }
                if (digits >= signature.size() * 2)
                {
                    return std::nullopt;
                }
                signature[digits / 2] = static_cast<std::uint8_t>((signature[digits / 2] << 4) | value);
                ++digits;
            }
            if (digits != signature.size() * 2)
            {
                return std::nullopt;
            }
            return signature;
        }

        // Whether `signature` (r then s, 32 bytes each) is the release key's signature of `digest`.
        [[nodiscard]] outcome<bool> signed_with_release_key(std::array<std::uint8_t, 32> const &digest,
                                                            std::array<std::uint8_t, 64> const &signature)
        {
            algorithm_provider provider;
            NTSTATUS result = BCryptOpenAlgorithmProvider(&provider.handle, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
            if (!BCRYPT_SUCCESS(result))
            {
                return std::unexpected(crypto_failure("BCryptOpenAlgorithmProvider (ECDSA P-256)", result));
            }
            // BCRYPT_ECCKEY_BLOB: what it is, the length of a coordinate, then X and Y
            std::array<std::uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + release_key.size()> blob{};
            BCRYPT_ECCKEY_BLOB const header{BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32};
            std::memcpy(blob.data(), &header, sizeof(header));
            std::memcpy(blob.data() + sizeof(header), release_key.data(), release_key.size());
            BCRYPT_KEY_HANDLE key = nullptr;
            result = BCryptImportKeyPair(provider.handle, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob.data(),
                                         static_cast<ULONG>(blob.size()), 0);
            if (!BCRYPT_SUCCESS(result))
            {
                return std::unexpected(crypto_failure("BCryptImportKeyPair (release key)", result));
            }
            // (neither is changed; the interface has no const)
            result = BCryptVerifySignature(key, nullptr, const_cast<PUCHAR>(digest.data()),
                                           static_cast<ULONG>(digest.size()), const_cast<PUCHAR>(signature.data()),
                                           static_cast<ULONG>(signature.size()), 0);
            BCryptDestroyKey(key);
            if (result == signature_does_not_fit)
            {
                return false;
            }
            if (!BCRYPT_SUCCESS(result))
            {
                return std::unexpected(crypto_failure("BCryptVerifySignature", result));
            }
            return true;
        }
    }

    outcome<std::array<std::uint8_t, 32>> sha256(std::span<std::uint8_t const> bytes)
    {
        algorithm_provider provider;
        NTSTATUS result = BCryptOpenAlgorithmProvider(&provider.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (!BCRYPT_SUCCESS(result))
        {
            return std::unexpected(crypto_failure("BCryptOpenAlgorithmProvider (SHA-256)", result));
        }
        BCRYPT_HASH_HANDLE hash = nullptr;
        result = BCryptCreateHash(provider.handle, &hash, nullptr, 0, nullptr, 0, 0);
        if (!BCRYPT_SUCCESS(result))
        {
            return std::unexpected(crypto_failure("BCryptCreateHash", result));
        }
        std::array<std::uint8_t, 32> digest{};
        // in pieces: the length is a 32-bit number (the data is not changed; the interface has no const)
        for (std::size_t offset = 0; offset < bytes.size() && BCRYPT_SUCCESS(result);)
        {
            auto const piece = static_cast<ULONG>(std::min<std::size_t>(bytes.size() - offset, 1U << 24));
            result = BCryptHashData(hash, const_cast<PUCHAR>(bytes.data() + offset), piece, 0);
            offset += piece;
        }
        if (BCRYPT_SUCCESS(result))
        {
            result = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
        }
        BCryptDestroyHash(hash);
        if (!BCRYPT_SUCCESS(result))
        {
            return std::unexpected(crypto_failure("SHA-256", result));
        }
        return digest;
    }

    std::string hexadecimal(std::span<std::uint8_t const> bytes)
    {
        std::string text;
        text.reserve(bytes.size() * 2);
        for (std::uint8_t const byte : bytes)
        {
            text += std::format("{:02x}", byte);
        }
        return text;
    }

    outcome<signature_check> check_release_signature(std::span<std::uint8_t const> package,
                                                     std::span<std::uint8_t const> signature_file)
    {
        signature_check checked;
        auto const digest = sha256(package);
        if (!digest)
        {
            return std::unexpected(digest.error());
        }
        checked.digest = *digest;
        std::optional<std::array<std::uint8_t, 64>> const signature = signature_from(signature_file);
        if (!signature)
        {
            return checked;
        }
        checked.well_formed = true;
        auto const fits = signed_with_release_key(*digest, *signature);
        if (!fits)
        {
            return std::unexpected(fits.error());
        }
        checked.fits = *fits;
        return checked;
    }
}
