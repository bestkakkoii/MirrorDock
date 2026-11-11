// SPDX-License-Identifier: GPL-3.0-or-later
module uxplay_core_device;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_byte_stream;
import uxplay_property_list;
import uxplay_remote_xpc;
import uxplay_device_tunnel;

namespace uxplay
{
    namespace
    {
        constexpr std::uint64_t messaging_protocol_version = 7;
        constexpr std::uint64_t remote_xpc_version_flags = 0x0100000000000006ULL;
        constexpr auto directory_patience = std::chrono::seconds{6};

        // what a caller of the developer services says it is (as Xcode's tools do)
        constexpr std::string_view core_device_version = "629.3";
        constexpr std::array<std::uint64_t, 2> core_device_version_parts{629, 3};

        // ---- the media offer: a small protocol-buffer message --------------------------
        void put_varint(std::vector<std::uint8_t> &out, std::uint64_t value)
        {
            while (value >= 0x80)
            {
                out.push_back(static_cast<std::uint8_t>(value | 0x80));
                value >>= 7;
            }
            out.push_back(static_cast<std::uint8_t>(value));
        }

        void put_number(std::vector<std::uint8_t> &out, std::uint32_t field, std::uint64_t value)
        {
            put_varint(out, static_cast<std::uint64_t>(field) << 3);
            put_varint(out, value);
        }

        void put_bytes(std::vector<std::uint8_t> &out, std::uint32_t field, std::span<std::uint8_t const> value)
        {
            put_varint(out, (static_cast<std::uint64_t>(field) << 3) | 2);
            put_varint(out, value.size());
            out.insert(out.end(), value.begin(), value.end());
        }

        void put_text(std::vector<std::uint8_t> &out, std::uint32_t field, std::string_view value)
        {
            put_bytes(out, field, std::span{reinterpret_cast<std::uint8_t const *>(value.data()), value.size()});
        }

        // a number in exactly five bytes (the session number has a fixed place in Apple's offers)
        void put_padded_number(std::vector<std::uint8_t> &out, std::uint32_t field, std::uint32_t value)
        {
            put_varint(out, static_cast<std::uint64_t>(field) << 3);
            for (int index = 0; index < 4; ++index)
            {
                out.push_back(static_cast<std::uint8_t>(((value >> (7 * index)) & 0x7F) | 0x80));
            }
            out.push_back(static_cast<std::uint8_t>((value >> 28) & 0x0F));
        }

        // one codec with its capability entries and feature list
        [[nodiscard]] std::vector<std::uint8_t> codec_bank(std::uint32_t payload_type, std::string_view features,
                                                           std::uint32_t kind, int entries)
        {
            std::vector<std::uint8_t> bank;
            put_number(bank, 1, payload_type);
            for (int index = 0; index < entries; ++index)
            {
                std::vector<std::uint8_t> entry;
                put_number(entry, 1, 1);
                put_number(entry, 2, static_cast<std::uint64_t>(1 + index % 2));
                put_number(entry, 3, 50115); // the capability identifier of every such entry
                put_number(entry, 4, 0);
                put_bytes(bank, 2, entry);
            }
            put_text(bank, 3, features);
            put_number(bank, 4, kind);
            return bank;
        }

        // The description of what this end can receive: HEVC and AVC, each
        // with the plain feature list ("FLS;SW:1;" - with "VRAE:0" the phone's
        // encoder drops frames under motion), no long-term reference
        // pictures, forward error correction on.
        [[nodiscard]] std::vector<std::uint8_t> video_media_description(std::uint32_t session)
        {
            std::vector<std::uint8_t> settings;
            put_padded_number(settings, 1, session);
            put_number(settings, 2, 0);                                  // no RTCP feedback
            put_bytes(settings, 3, codec_bank(123, "FLS;SW:1;", 1, 4));  // HEVC
            put_bytes(settings, 3, codec_bank(100, "FLS;SW:1;", 14, 2)); // AVC
            put_number(settings, 7, 0);                                  // no long-term reference pictures
            put_number(settings, 8, 63);
            put_number(settings, 10, 1); // forward error correction
            put_number(settings, 12, 1);

            std::vector<std::uint8_t> description;
            put_number(description, 1, 1);
            put_number(description, 2, 1);
            put_bytes(description, 5, settings);
            put_text(description, 6, "Viceroy 1.7.0"); // the decoder this end claims to have
            put_number(description, 8, 0);
            struct tier
            {
                std::uint64_t kind;
                std::uint64_t value;
                std::optional<std::uint64_t> buffer;
            };
            // Apple's table of bit-rate tiers, in the order of its own offers
            for (tier const &entry : {tier{4074, 0, 16384}, tier{0, 75'000'000, 524288}, tier{0, 40'000'000, 12288},
                                      tier{16, 4100, std::nullopt}, tier{0, 20'000'000, 98304},
                                      tier{4, 6500, std::nullopt}, tier{0, 6'000'000, 131072},
                                      tier{0, 100'000'000, 1048576}, tier{0, 60'000'000, 262144},
                                      tier{1, 299, std::nullopt}})
            {
                std::vector<std::uint8_t> body;
                put_number(body, 1, entry.kind);
                put_number(body, 2, entry.value);
                if (entry.buffer)
                {
                    put_number(body, 3, *entry.buffer);
                }
                put_bytes(description, 9, body);
            }
            put_number(description, 13, 17137042128614416384ULL); // a time stamp the phone does not look at
            put_number(description, 14, 2);
            put_number(description, 16, 0);
            put_number(description, 18, 1);
            return description;
        }

        // zlib's container around data that is stored as it is (no compression:
        // the description is a few hundred bytes)
        [[nodiscard]] std::vector<std::uint8_t> zlib_stored(std::span<std::uint8_t const> data)
        {
            std::vector<std::uint8_t> out{0x78, 0x01};
            std::size_t offset = 0;
            do
            {
                std::size_t const chunk = std::min<std::size_t>(data.size() - offset, 0xFFFF);
                bool const last = offset + chunk == data.size();
                out.push_back(last ? 1 : 0);
                out.push_back(static_cast<std::uint8_t>(chunk));
                out.push_back(static_cast<std::uint8_t>(chunk >> 8));
                out.push_back(static_cast<std::uint8_t>(~chunk));
                out.push_back(static_cast<std::uint8_t>(~chunk >> 8));
                out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                           data.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
                offset += chunk;
            } while (offset < data.size());
            std::uint32_t low = 1;
            std::uint32_t high = 0;
            for (std::uint8_t const byte : data)
            {
                low = (low + byte) % 65521;
                high = (high + low) % 65521;
            }
            std::uint32_t const checksum = (high << 16) | low;
            for (int shift = 24; shift >= 0; shift -= 8)
            {
                out.push_back(static_cast<std::uint8_t>(checksum >> shift));
            }
            return out;
        }

        // {"int": n}, {"string": s}, {"uuid": u}: how option values are wrapped
        [[nodiscard]] xpc_value tagged(std::string_view tag, xpc_value value)
        {
            xpc_value wrapped = xpc_value::of_dictionary();
            wrapped.set(tag, std::move(value));
            return wrapped;
        }
    }

    std::array<std::uint8_t, 16> new_uuid()
    {
        std::array<std::uint8_t, 16> uuid{};
        std::random_device source;
        for (std::size_t index = 0; index < uuid.size(); index += 4)
        {
            std::uint32_t const word = source();
            for (std::size_t byte = 0; byte < 4; ++byte)
            {
                uuid[index + byte] = static_cast<std::uint8_t>(word >> (8 * byte));
            }
        }
        uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0F) | 0x40); // version 4
        uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3F) | 0x80); // variant
        return uuid;
    }

    std::string uuid_text(std::span<std::uint8_t const, 16> uuid)
    {
        std::string text;
        for (std::size_t index = 0; index < uuid.size(); ++index)
        {
            if (index == 4 || index == 6 || index == 8 || index == 10)
            {
                text += '-';
            }
            text += std::format("{:02X}", uuid[index]);
        }
        return text;
    }

    struct service_directory::state
    {
        device_tunnel *tunnel = nullptr;
        std::unique_ptr<remote_xpc_connection> connection; // kept open: the phone ends the services with it
        std::map<std::string, std::uint16_t, std::less<>> ports;
    };

    service_directory::service_directory(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    service_directory::~service_directory() = default;

    outcome<std::unique_ptr<service_directory>> service_directory::open(device_tunnel &tunnel,
                                                                        std::span<std::uint8_t const, 16> identity)
    {
        auto created = std::make_unique<state>();
        created->tunnel = &tunnel;
        outcome<std::unique_ptr<byte_stream>> stream = tunnel.connect(tunnel.discovery_port());
        if (!stream)
        {
            return std::unexpected(stream.error());
        }
        outcome<std::unique_ptr<remote_xpc_connection>> connection = remote_xpc_connection::open(std::move(*stream));
        if (!connection)
        {
            return std::unexpected(connection.error());
        }
        created->connection = std::move(*connection);

        xpc_value properties = xpc_value::of_dictionary();
        properties.set("RemoteXPCVersionFlags", xpc_value::of_unsigned(remote_xpc_version_flags));
        properties.set("SensitivePropertiesVisible", xpc_value::of_flag(true));
        xpc_value handshake = xpc_value::of_dictionary();
        handshake.set("MessageType", xpc_value::of_text("Handshake"));
        handshake.set("MessagingProtocolVersion", xpc_value::of_unsigned(messaging_protocol_version));
        handshake.set("UUID", xpc_value::of_uuid(identity));
        handshake.set("Properties", std::move(properties));
        handshake.set("Services", xpc_value::of_dictionary());
        if (status const sent = created->connection->send(handshake); !sent)
        {
            return std::unexpected(sent.error());
        }
        outcome<xpc_value> const answer = created->connection->receive(directory_patience);
        if (!answer)
        {
            return std::unexpected(answer.error());
        }
        xpc_value const *const services = answer->find("Services");
        if (services == nullptr || services->type != xpc_value::kind::dictionary)
        {
            return std::unexpected(make_failure("service directory", 0, "the phone's answer names no services"));
        }
        for (auto const &[name, entry] : services->entries)
        {
            xpc_value const *const port = entry.find("Port");
            if (port == nullptr)
            {
                continue;
            }
            // the port is written as text
            std::string_view const text = port->as_text().value_or("");
            std::uint16_t number = 0;
            auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
            if (error == std::errc{} && end == text.data() + text.size() && number != 0)
            {
                created->ports.emplace(name, number);
            }
        }
        return std::unique_ptr<service_directory>{new service_directory{std::move(created)}};
    }

    std::size_t service_directory::size() const
    {
        return impl->ports.size();
    }

    bool service_directory::offers(std::string_view service) const
    {
        return impl->ports.contains(service);
    }

    outcome<std::unique_ptr<remote_xpc_connection>> service_directory::connect(std::string_view service)
    {
        auto const found = impl->ports.find(service);
        if (found == impl->ports.end())
        {
            return std::unexpected(make_failure("service directory", 0,
                                                std::format("the phone does not offer the service {}", service)));
        }
        outcome<std::unique_ptr<byte_stream>> stream = impl->tunnel->connect(found->second);
        if (!stream)
        {
            return std::unexpected(stream.error());
        }
        return remote_xpc_connection::open(std::move(*stream));
    }

    outcome<xpc_value> core_device_invoke(remote_xpc_connection &connection, std::string_view feature,
                                          std::string_view action, xpc_value input, std::chrono::milliseconds patience)
    {
        xpc_value version = xpc_value::of_dictionary();
        std::vector<xpc_value> components;
        for (std::uint64_t const part : core_device_version_parts)
        {
            components.push_back(xpc_value::of_unsigned(part));
        }
        version.set("components", xpc_value::of_array(std::move(components)));
        version.set("originalComponentsCount",
                    xpc_value::of_signed(static_cast<std::int64_t>(core_device_version_parts.size())));
        version.set("stringValue", xpc_value::of_text(core_device_version));

        xpc_value request = xpc_value::of_dictionary();
        request.set("CoreDevice.CoreDeviceDDIProtocolVersion", xpc_value::of_signed(2));
        request.set("CoreDevice.coreDeviceVersion", std::move(version));
        request.set("CoreDevice.deviceIdentifier", xpc_value::of_text(uuid_text(new_uuid())));
        if (input.type != xpc_value::kind::dictionary)
        {
            input = xpc_value::of_dictionary();
        }
        request.set("CoreDevice.input", std::move(input));
        request.set("CoreDevice.invocationIdentifier", xpc_value::of_text(uuid_text(new_uuid())));
        request.set("CoreDevice.featureIdentifier", xpc_value::of_text(feature));
        request.set("CoreDevice.action", xpc_value::of_dictionary());
        if (!action.empty())
        {
            request.set("CoreDevice.actionIdentifier", xpc_value::of_text(action));
        }
        outcome<xpc_value> answer = connection.request(request, patience);
        if (!answer)
        {
            return std::unexpected(answer.error());
        }
        if (xpc_value const *const output = answer->find("CoreDevice.output"); output != nullptr)
        {
            return *output;
        }
        // the phone's own account of what went wrong
        std::int64_t code = 0;
        std::string words = "the phone gave no reason";
        if (xpc_value const *const error = answer->find("CoreDevice.error"); error != nullptr)
        {
            if (xpc_value const *const number = error->find("code"); number != nullptr)
            {
                code = static_cast<std::int64_t>(number->number);
            }
            if (xpc_value const *const user_info = error->find("userInfo"); user_info != nullptr)
            {
                if (xpc_value const *const text = user_info->find("NSLocalizedDescription"); text != nullptr)
                {
                    words = std::string{text->as_text().value_or(words)};
                }
            }
        }
        return std::unexpected(make_failure(feature, code, words));
    }

    outcome<xpc_value> video_stream_request(std::string_view receiver_address, std::uint16_t receiver_port,
                                            std::string_view phone_address)
    {
        std::random_device source;
        // the offer: a binary property list with the description (in zlib's container) inside
        std::vector<std::uint8_t> endpoint;
        put_number(endpoint, 1, 0);
        put_number(endpoint, 2, 1);
        put_text(endpoint, 3, "Mac15,9"); // the kind of host Apple's own tool is, by the phone's lights
        put_text(endpoint, 4, "2205.3.1");
        put_text(endpoint, 5, "25F80");
        property_list offer;
        offer.set_data("avcMediaStreamNegotiatorMediaBlob", zlib_stored(video_media_description(source())));
        offer.set_number("avcMediaStreamNegotiatorMode", 5); // video
        offer.set_text("avcMediaStreamOptionCallID", uuid_text(new_uuid()));
        offer.set_data("avcMediaStreamOptionRemoteEndpointInfo", endpoint);
        outcome<std::vector<std::uint8_t>> const offer_bytes = offer.to_binary();
        if (!offer_bytes)
        {
            return std::unexpected(offer_bytes.error());
        }

        xpc_value options = xpc_value::of_dictionary();
        options.set("AVCMediaStreamNegotiatorAccessNetworkType", tagged("int", xpc_value::of_signed(1)));
        options.set("AVCMediaStreamNegotiatorTransportProtocolType", tagged("int", xpc_value::of_signed(2)));
        options.set("CoreDeviceVideoDisplayMode", tagged("string", xpc_value::of_text("DisplayByID")));
        options.set("VideoStreamForDisplayID", tagged("int", xpc_value::of_signed(1)));
        options.set("avcMediaStreamOptionClientSessionID", tagged("uuid", xpc_value::of_uuid(new_uuid())));

        xpc_value input = xpc_value::of_dictionary();
        input.set("clientSupportedFeatures", xpc_value::of_unsigned(140));
        input.set("direction", xpc_value::of_text("output"));
        input.set("negotiatorOffer", xpc_value::of_data(*offer_bytes));
        input.set("options", std::move(options));
        input.set("receiverIP", xpc_value::of_text(receiver_address));
        input.set("receiverPort", xpc_value::of_unsigned(receiver_port));
        input.set("senderIP", xpc_value::of_text(phone_address));
        input.set("timeout", xpc_value::of_unsigned(20));
        input.set("type", xpc_value::of_text("video"));
        return input;
    }
}
