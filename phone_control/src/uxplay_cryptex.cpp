// SPDX-License-Identifier: GPL-3.0-or-later
module uxplay_cryptex;

import std;
import uxplay_status;
import uxplay_remote_xpc;
import uxplay_core_device;

namespace uxplay
{
    namespace
    {
        constexpr std::string_view service_name = "com.apple.security.cryptexd.remote";
        constexpr auto answer_patience = std::chrono::seconds{15};
        // what Xcode says of itself when it installs (the version of the service's protocol)
        constexpr std::uint64_t client_version = 3;
        // the kind of image, by its place in the phone's table: a generic disk image of a "Cryptex1"
        constexpr std::int64_t generic_image_index = 10;
        // "until the next software update" for the cryptex and for the nonce it is bound to
        constexpr std::uint64_t lasting_persistence = 2;
        constexpr std::uint64_t lasting_nonce_persistence = 1;
        // the nonce comes inside a small structure: two bytes, the nonce, and
        // (in the last four bytes) how long the nonce is
        constexpr std::size_t nonce_lead_bytes = 2;
        constexpr std::size_t nonce_length_bytes = 4;

        [[nodiscard]] xpc_value routine_request(std::string_view routine, xpc_value arguments)
        {
            xpc_value request = xpc_value::of_dictionary();
            request.set("routine", xpc_value::of_text(routine));
            request.set("argv", std::move(arguments));
            return request;
        }

        // the result of a routine out of the phone's answer, or the phone's account of the failure
        [[nodiscard]] outcome<xpc_value> routine_result(std::string_view routine, xpc_value const &answer)
        {
            std::string const stage = std::format("cryptexd {}", routine);
            if (xpc_value const *const problem = answer.find("cferr"); problem != nullptr)
            {
                std::int64_t code = 0;
                std::string words = "the phone gave no reason";
                if (xpc_value const *const number = problem->find("cferr_code"); number != nullptr)
                {
                    code = static_cast<std::int64_t>(number->number);
                }
                if (xpc_value const *const user_info = problem->find("cferr_userinfo"); user_info != nullptr)
                {
                    if (xpc_value const *const text = user_info->find("NSLocalizedDescription"); text != nullptr)
                    {
                        words = std::string{text->as_text().value_or(words)};
                    }
                }
                if (xpc_value const *const domain = problem->find("cferr_domain"); domain != nullptr)
                {
                    words = std::format("{} ({})", words, domain->as_text().value_or("unknown domain"));
                }
                return std::unexpected(make_failure(stage, code, words));
            }
            // (a routine that has no "error" in its answer succeeded)
            if (xpc_value const *const error = answer.find("error"); error != nullptr && error->number != 0)
            {
                return std::unexpected(make_failure(stage, static_cast<std::int64_t>(error->number),
                                                    "the phone reports this error number (errno)"));
            }
            if (xpc_value const *const result = answer.find("argv"); result != nullptr)
            {
                return *result;
            }
            return xpc_value::of_dictionary();
        }

        [[nodiscard]] outcome<xpc_value> call(service_directory &directory, std::string_view routine,
                                              xpc_value arguments)
        {
            outcome<std::unique_ptr<remote_xpc_connection>> const connection = directory.connect(service_name);
            if (!connection)
            {
                return std::unexpected(connection.error());
            }
            outcome<xpc_value> const answer =
                (*connection)->request(routine_request(routine, std::move(arguments)), answer_patience);
            if (!answer)
            {
                return std::unexpected(answer.error());
            }
            return routine_result(routine, *answer);
        }
    }

    bool offers_cryptexes(service_directory const &directory)
    {
        return directory.offers(service_name);
    }

    outcome<std::vector<installed_cryptex>> installed_cryptexes(service_directory &directory)
    {
        outcome<xpc_value> const result = call(directory, "copy-installed", xpc_value::of_dictionary());
        if (!result)
        {
            return std::unexpected(result.error());
        }
        std::vector<installed_cryptex> installed;
        if (xpc_value const *const list = result->find("remote-cryptex-array"); list != nullptr)
        {
            for (xpc_value const &entry : list->items)
            {
                xpc_value const *const identifier = entry.find("remote-cryptex-identifier");
                xpc_value const *const version = entry.find("remote-cryptex-version");
                installed.push_back(installed_cryptex{
                    identifier != nullptr ? std::string{identifier->as_text().value_or("")} : std::string{},
                    version != nullptr ? std::string{version->as_text().value_or("")} : std::string{}});
            }
        }
        return installed;
    }

    outcome<chip_identity> read_chip_identity(service_directory &directory)
    {
        outcome<xpc_value> const result = call(directory, "read-personalization-id", xpc_value::of_dictionary());
        if (!result)
        {
            return std::unexpected(result.error());
        }
        xpc_value const *const chip = result->find("img4_chip_chip");
        xpc_value const *const unique_number = result->find("img4_chip_ecid");
        if (chip == nullptr || unique_number == nullptr || !chip->as_unsigned() || !unique_number->as_unsigned())
        {
            return std::unexpected(make_failure("cryptexd read-personalization-id", 0,
                                                "the phone's answer does not name its chip"));
        }
        chip_identity identity;
        identity.chip = *chip->as_unsigned();
        identity.unique_number = *unique_number->as_unsigned();
        if (xpc_value const *const production = result->find("img4_chip_cpro"); production != nullptr)
        {
            identity.production = production->as_flag().value_or(production->number != 0);
        }
        return identity;
    }

    outcome<std::vector<std::uint8_t>> cryptex_nonce(service_directory &directory, std::uint64_t domain_handle)
    {
        xpc_value arguments = xpc_value::of_dictionary();
        arguments.set("nonce-domain-handle", xpc_value::of_unsigned(domain_handle));
        outcome<xpc_value> const result = call(directory, "get-nonce", std::move(arguments));
        if (!result)
        {
            return std::unexpected(result.error());
        }
        xpc_value const *const structure = result->find("nonce");
        if (structure == nullptr || structure->type != xpc_value::kind::data ||
            structure->bytes.size() < nonce_lead_bytes + nonce_length_bytes)
        {
            return std::unexpected(make_failure("cryptexd get-nonce", 0, "the phone's answer holds no nonce"));
        }
        std::vector<std::uint8_t> const &bytes = structure->bytes;
        std::size_t length = 0;
        for (std::size_t index = 0; index < nonce_length_bytes; ++index)
        {
            length |= static_cast<std::size_t>(bytes[bytes.size() - nonce_length_bytes + index]) << (8 * index);
        }
        if (length == 0 || length > bytes.size() - nonce_lead_bytes - nonce_length_bytes)
        {
            return std::unexpected(make_failure("cryptexd get-nonce", static_cast<std::int64_t>(length),
                                                "the nonce's length does not fit its structure"));
        }
        return std::vector<std::uint8_t>(bytes.begin() + nonce_lead_bytes,
                                         bytes.begin() + static_cast<std::ptrdiff_t>(nonce_lead_bytes + length));
    }

    status install_cryptex(service_directory &directory, cryptex_parts const &parts,
                           std::chrono::milliseconds patience)
    {
        // (the numbers among the properties must be unsigned: the phone refuses the others)
        xpc_value properties = xpc_value::of_dictionary();
        properties.set("Cryptex1,UseProductClass", xpc_value::of_flag(parts.use_product_class));
        properties.set("MountedCryptex", xpc_value::of_flag(false));
        properties.set("Cryptex1,SubType", xpc_value::of_unsigned(parts.sub_type));
        properties.set("Cryptex1,NonceDomain", xpc_value::of_unsigned(parts.nonce_domain));
        properties.set("Cryptex1,Version", xpc_value::of_text(parts.version));
        properties.set("Cryptex1,PreauthVersion", xpc_value::of_text(parts.preauthorization_version));

        struct announced_file
        {
            std::string_view name;
            std::span<std::uint8_t const> content;
        };
        std::array const files{announced_file{"image", parts.image}, announced_file{"trustcache", parts.trust_cache},
                               announced_file{"im4m", parts.ticket}, announced_file{"info", parts.info},
                               announced_file{"volumehash", parts.volume_hash}};

        xpc_value arguments = xpc_value::of_dictionary();
        arguments.set("auth", xpc_value::of_unsigned(0));
        arguments.set("client-version", xpc_value::of_unsigned(client_version));
        arguments.set("cryptex1-properties", std::move(properties));
        arguments.set("image-type-index", xpc_value::of_signed(generic_image_index));
        arguments.set("nonce-persistence", xpc_value::of_unsigned(lasting_nonce_persistence));
        arguments.set("persistence", xpc_value::of_unsigned(lasting_persistence));
        for (std::size_t index = 0; index < files.size(); ++index)
        {
            arguments.set(files[index].name, xpc_value::of_file_transfer(index + 1, files[index].content.size()));
        }

        outcome<std::unique_ptr<remote_xpc_connection>> const connection = directory.connect(service_name);
        if (!connection)
        {
            return std::unexpected(connection.error());
        }
        if (status const asked = (*connection)->begin_request(routine_request("install", std::move(arguments)));
            !asked)
        {
            return asked;
        }
        for (std::size_t index = 0; index < files.size(); ++index)
        {
            if (status const sent = (*connection)->send_file(index + 1, files[index].content); !sent)
            {
                return std::unexpected(make_failure(std::format("cryptexd install ({})", files[index].name),
                                                    sent.error().code, sent.error().describe()));
            }
        }
        outcome<xpc_value> const answer = (*connection)->receive(patience);
        if (!answer)
        {
            return std::unexpected(make_failure("cryptexd install", answer.error().code,
                                                std::format("no answer after the parts were sent ({})",
                                                            answer.error().describe())));
        }
        if (outcome<xpc_value> const result = routine_result("install", *answer); !result)
        {
            return std::unexpected(result.error());
        }
        return {};
    }
}
