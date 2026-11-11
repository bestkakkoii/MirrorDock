// SPDX-License-Identifier: GPL-3.0-or-later
module uxplay_lockdown;

import std;
import uxplay_status;
import uxplay_byte_stream;
import uxplay_property_list;
import uxplay_usbmux;
import uxplay_device_stream;

namespace uxplay
{
    namespace
    {
        constexpr std::uint16_t lockdown_port = 62078;
        constexpr std::string_view client_label = "mirror_dock";
        // a message larger than this is not one of lockdown's
        constexpr std::uint32_t longest_message = 16 * 1024 * 1024;
        // lockdown and the services it starts answer at once; a phone that
        // has not for this long is gone or hangs
        constexpr std::chrono::milliseconds answer_patience = std::chrono::seconds{10};
        // the stages at which the phone's trust in this PC shows
        constexpr std::string_view pairing_record_stage = "pairing record";
        constexpr std::string_view start_session_stage = "lockdown StartSession";

        // the phone's own word for what went wrong, when the reply carries one
        [[nodiscard]] status reply_status(property_list const &reply, std::string_view stage)
        {
            if (std::optional<std::string> const error = reply.root().item("Error").text(); error)
            {
                return std::unexpected(make_failure(stage, 0, *error));
            }
            return {};
        }
    }

    status send_property_list(byte_stream &stream, property_list const &message)
    {
        outcome<std::string> const body = message.to_xml();
        if (!body)
        {
            return std::unexpected(body.error());
        }
        std::vector<std::uint8_t> framed(4 + body->size());
        auto const length = static_cast<std::uint32_t>(body->size());
        framed[0] = static_cast<std::uint8_t>(length >> 24);
        framed[1] = static_cast<std::uint8_t>(length >> 16);
        framed[2] = static_cast<std::uint8_t>(length >> 8);
        framed[3] = static_cast<std::uint8_t>(length);
        std::memcpy(framed.data() + 4, body->data(), body->size());
        return stream.write_all(framed);
    }

    outcome<property_list> receive_property_list(byte_stream &stream)
    {
        std::array<std::uint8_t, 4> header{};
        if (status const got = stream.read_exact(header); !got)
        {
            return std::unexpected(got.error());
        }
        std::uint32_t const length = (static_cast<std::uint32_t>(header[0]) << 24) |
                                     (static_cast<std::uint32_t>(header[1]) << 16) |
                                     (static_cast<std::uint32_t>(header[2]) << 8) | static_cast<std::uint32_t>(header[3]);
        if (length == 0 || length > longest_message)
        {
            return std::unexpected(make_failure("property list message", length, "implausible length"));
        }
        std::vector<std::uint8_t> body(length);
        if (status const got = stream.read_exact(body); !got)
        {
            return std::unexpected(got.error());
        }
        return property_list::parse(body);
    }

    struct lockdown_session::state
    {
        usbmux_device device;
        std::string host_identifier; // "HostID" of the pairing: a UUID as text
        std::unique_ptr<device_stream> stream;
        // of this PC's pairing with the phone (PEM); kept for the services' TLS
        std::vector<std::uint8_t> certificate;
        std::vector<std::uint8_t> private_key;
        std::mutex exchange_mutex; // one request and its reply at a time

        [[nodiscard]] outcome<property_list> exchange(property_list const &request)
        {
            std::scoped_lock lock{exchange_mutex};
            if (status const sent = send_property_list(*stream, request); !sent)
            {
                return std::unexpected(sent.error());
            }
            return receive_property_list(*stream);
        }
    };

    lockdown_session::lockdown_session(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    lockdown_session::~lockdown_session()
    {
        if (impl && impl->stream)
        {
            impl->stream->shut_down();
        }
    }

    outcome<std::unique_ptr<lockdown_session>> lockdown_session::open(usbmux_device const &device)
    {
        auto created = std::make_unique<state>();
        created->device = device;

        outcome<property_list> const record = usbmux_pair_record(device.serial);
        if (!record)
        {
            // (usbmuxd answered a moment ago, when it named the phone: it has no record of a pairing)
            return std::unexpected(
                make_failure(pairing_record_stage, record.error().code, record.error().describe()));
        }
        std::optional<std::string> const host_identifier = record->root().item("HostID").text();
        std::optional<std::string> const system_identifier = record->root().item("SystemBUID").text();
        std::optional<std::vector<std::uint8_t>> certificate = record->root().item("HostCertificate").data();
        std::optional<std::vector<std::uint8_t>> private_key = record->root().item("HostPrivateKey").data();
        if (!host_identifier || !system_identifier || !certificate || !private_key)
        {
            return std::unexpected(
                make_failure(pairing_record_stage, 0, "the record of this PC's pairing is incomplete"));
        }
        created->certificate = std::move(*certificate);
        created->private_key = std::move(*private_key);
        created->host_identifier = *host_identifier;

        outcome<device_socket> socket = usbmux_connect(device.identifier, lockdown_port);
        if (!socket)
        {
            return std::unexpected(socket.error());
        }
        created->stream = std::make_unique<device_stream>(std::move(*socket));
        created->stream->set_read_patience(answer_patience);

        property_list query;
        query.set_text("Label", client_label);
        query.set_text("Request", "QueryType");
        outcome<property_list> const type = created->exchange(query);
        if (!type)
        {
            return std::unexpected(type.error());
        }
        if (type->root().item("Type").text().value_or("") != "com.apple.mobile.lockdown")
        {
            return std::unexpected(make_failure("lockdown QueryType", 0, "the phone's lockdown service did not answer"));
        }

        property_list start;
        start.set_text("Label", client_label);
        start.set_text("Request", "StartSession");
        start.set_text("HostID", *host_identifier);
        start.set_text("SystemBUID", *system_identifier);
        outcome<property_list> const session = created->exchange(start);
        if (!session)
        {
            return std::unexpected(session.error());
        }
        if (status const accepted = reply_status(*session, start_session_stage); !accepted)
        {
            return std::unexpected(accepted.error());
        }
        if (session->root().item("EnableSessionSSL").flag().value_or(false))
        {
            if (status const encrypted = created->stream->start_tls(created->certificate, created->private_key);
                !encrypted)
            {
                return std::unexpected(encrypted.error());
            }
        }
        return std::unique_ptr<lockdown_session>{new lockdown_session{std::move(created)}};
    }

    bool pairing_refused(failure const &cause)
    {
        return cause.stage == pairing_record_stage || cause.stage == start_session_stage;
    }

    std::array<std::uint8_t, 16> lockdown_session::host_identity() const
    {
        // the hexadecimal digits of the pairing's identifier, in their order
        std::array<std::uint8_t, 16> identity{};
        std::size_t digits = 0;
        for (char const character : impl->host_identifier)
        {
            int const value = character >= '0' && character <= '9'   ? character - '0'
                              : character >= 'a' && character <= 'f' ? character - 'a' + 10
                              : character >= 'A' && character <= 'F' ? character - 'A' + 10
                                                                     : -1;
            if (value < 0 || digits >= 32)
            {
                continue;
            }
            identity[digits / 2] = static_cast<std::uint8_t>(identity[digits / 2] | (value << (digits % 2 == 0 ? 4 : 0)));
            ++digits;
        }
        return identity;
    }

    outcome<property_list> lockdown_session::value(std::string_view domain, std::string_view key)
    {
        property_list request;
        request.set_text("Label", client_label);
        request.set_text("Request", "GetValue");
        if (!domain.empty())
        {
            request.set_text("Domain", domain);
        }
        request.set_text("Key", key);
        outcome<property_list> reply = impl->exchange(request);
        if (!reply)
        {
            return std::unexpected(reply.error());
        }
        if (status const accepted = reply_status(*reply, "lockdown GetValue"); !accepted)
        {
            return std::unexpected(accepted.error());
        }
        return reply;
    }

    outcome<std::unique_ptr<byte_stream>> lockdown_session::start_service(std::string_view name)
    {
        property_list request;
        request.set_text("Label", client_label);
        request.set_text("Request", "StartService");
        request.set_text("Service", name);
        outcome<property_list> const reply = impl->exchange(request);
        if (!reply)
        {
            return std::unexpected(reply.error());
        }
        if (status const accepted = reply_status(*reply, std::format("lockdown StartService({})", name)); !accepted)
        {
            return std::unexpected(accepted.error());
        }
        std::optional<std::uint64_t> const port = reply->root().item("Port").number();
        if (!port || *port == 0 || *port > 65535)
        {
            return std::unexpected(make_failure("lockdown StartService", 0, "the phone named no port for the service"));
        }
        outcome<device_socket> socket = usbmux_connect(impl->device.identifier, static_cast<std::uint16_t>(*port));
        if (!socket)
        {
            return std::unexpected(socket.error());
        }
        auto stream = std::make_unique<device_stream>(std::move(*socket));
        stream->set_read_patience(answer_patience);
        if (reply->root().item("EnableServiceSSL").flag().value_or(false))
        {
            if (status const encrypted = stream->start_tls(impl->certificate, impl->private_key); !encrypted)
            {
                return std::unexpected(encrypted.error());
            }
        }
        return std::unique_ptr<byte_stream>{std::move(stream)};
    }
}
