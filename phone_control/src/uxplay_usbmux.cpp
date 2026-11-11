// SPDX-License-Identifier: GPL-3.0-or-later
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

module uxplay_usbmux;

import std;
import uxplay_status;
import uxplay_property_list;

namespace uxplay
{
    namespace
    {
        constexpr std::uint16_t usbmuxd_port = 27015;
        constexpr std::uint32_t protocol_version = 1;
        constexpr std::uint32_t message_property_list = 8;
        constexpr std::size_t header_bytes = 16;
        // a reply larger than this is not one of usbmuxd's
        constexpr std::uint32_t longest_reply = 4 * 1024 * 1024;
        constexpr DWORD socket_timeout_ms = 8000;

        [[nodiscard]] failure socket_failure(std::string_view stage)
        {
            return make_win32_failure(stage, static_cast<unsigned long>(WSAGetLastError()));
        }

        void put_le32(std::uint8_t *target, std::uint32_t value)
        {
            target[0] = static_cast<std::uint8_t>(value);
            target[1] = static_cast<std::uint8_t>(value >> 8);
            target[2] = static_cast<std::uint8_t>(value >> 16);
            target[3] = static_cast<std::uint8_t>(value >> 24);
        }

        [[nodiscard]] std::uint32_t get_le32(std::uint8_t const *source)
        {
            return static_cast<std::uint32_t>(source[0]) | (static_cast<std::uint32_t>(source[1]) << 8) |
                   (static_cast<std::uint32_t>(source[2]) << 16) | (static_cast<std::uint32_t>(source[3]) << 24);
        }

        [[nodiscard]] status send_all(SOCKET socket, std::span<std::uint8_t const> data)
        {
            std::size_t sent = 0;
            while (sent < data.size())
            {
                int const count = send(socket, reinterpret_cast<char const *>(data.data() + sent),
                                       static_cast<int>(std::min<std::size_t>(data.size() - sent, 1 << 20)), 0);
                if (count <= 0)
                {
                    return std::unexpected(socket_failure("send(usbmuxd)"));
                }
                sent += static_cast<std::size_t>(count);
            }
            return {};
        }

        [[nodiscard]] status receive_all(SOCKET socket, std::span<std::uint8_t> buffer)
        {
            std::size_t filled = 0;
            while (filled < buffer.size())
            {
                int const count = recv(socket, reinterpret_cast<char *>(buffer.data() + filled),
                                       static_cast<int>(std::min<std::size_t>(buffer.size() - filled, 1 << 20)), 0);
                if (count == 0)
                {
                    return std::unexpected(make_failure("recv(usbmuxd)", 0, "usbmuxd closed the connection"));
                }
                if (count < 0)
                {
                    return std::unexpected(socket_failure("recv(usbmuxd)"));
                }
                filled += static_cast<std::size_t>(count);
            }
            return {};
        }

        // a connection to usbmuxd itself
        [[nodiscard]] outcome<device_socket> open_usbmuxd()
        {
            SOCKET const raw = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (raw == INVALID_SOCKET)
            {
                return std::unexpected(socket_failure("socket(usbmuxd)"));
            }
            device_socket owned{raw};
            // replies to the control messages come at once; a hung service must not hang the caller
            setsockopt(raw, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const *>(&socket_timeout_ms),
                       sizeof(socket_timeout_ms));
            setsockopt(raw, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char const *>(&socket_timeout_ms),
                       sizeof(socket_timeout_ms));
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(usbmuxd_port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (connect(raw, reinterpret_cast<sockaddr const *>(&address), sizeof(address)) != 0)
            {
                failure cause = socket_failure("connect(usbmuxd)");
                cause.message += " (is Apple's driver package installed and its service running?)";
                return std::unexpected(std::move(cause));
            }
            return owned;
        }

        // one request and its reply
        [[nodiscard]] outcome<property_list> exchange(SOCKET socket, property_list const &request)
        {
            outcome<std::string> const body = request.to_xml();
            if (!body)
            {
                return std::unexpected(body.error());
            }
            std::vector<std::uint8_t> message(header_bytes + body->size());
            put_le32(message.data(), static_cast<std::uint32_t>(message.size()));
            put_le32(message.data() + 4, protocol_version);
            put_le32(message.data() + 8, message_property_list);
            put_le32(message.data() + 12, 1); // tag: one exchange at a time
            std::memcpy(message.data() + header_bytes, body->data(), body->size());
            if (status const sent = send_all(socket, message); !sent)
            {
                return std::unexpected(sent.error());
            }
            std::array<std::uint8_t, header_bytes> header{};
            if (status const got = receive_all(socket, header); !got)
            {
                return std::unexpected(got.error());
            }
            std::uint32_t const length = get_le32(header.data());
            if (length < header_bytes || length > longest_reply)
            {
                return std::unexpected(make_failure("usbmuxd reply", length, "implausible length"));
            }
            std::vector<std::uint8_t> reply(length - header_bytes);
            if (status const got = receive_all(socket, reply); !got)
            {
                return std::unexpected(got.error());
            }
            return property_list::parse(reply);
        }

        [[nodiscard]] property_list new_request(std::string_view type)
        {
            property_list request;
            request.set_text("MessageType", type);
            request.set_text("ClientVersionString", "mirror_dock");
            request.set_text("ProgName", "mirror_dock");
            request.set_number("kLibUSBMuxVersion", 3);
            return request;
        }
    }

    device_socket::device_socket(std::uintptr_t adopted)
        : handle{adopted}
    {
    }

    device_socket::~device_socket()
    {
        if (valid())
        {
            closesocket(static_cast<SOCKET>(handle));
        }
    }

    device_socket::device_socket(device_socket &&other) noexcept
        : handle{std::exchange(other.handle, ~std::uintptr_t{0})}
    {
    }

    device_socket &device_socket::operator=(device_socket &&other) noexcept
    {
        if (this != &other)
        {
            if (valid())
            {
                closesocket(static_cast<SOCKET>(handle));
            }
            handle = std::exchange(other.handle, ~std::uintptr_t{0});
        }
        return *this;
    }

    bool device_socket::valid() const
    {
        return handle != ~std::uintptr_t{0};
    }

    std::uintptr_t device_socket::native() const
    {
        return handle;
    }

    outcome<std::vector<usbmux_device>> usbmux_devices()
    {
        outcome<device_socket> service = open_usbmuxd();
        if (!service)
        {
            return std::unexpected(service.error());
        }
        outcome<property_list> const reply = exchange(static_cast<SOCKET>(service->native()), new_request("ListDevices"));
        if (!reply)
        {
            return std::unexpected(reply.error());
        }
        std::vector<usbmux_device> devices;
        property_node const list = reply->root().item("DeviceList");
        for (std::size_t index = 0; index < list.size(); ++index)
        {
            property_node const entry = list.element(index);
            property_node const properties = entry.item("Properties");
            usbmux_device device;
            device.identifier = static_cast<std::uint32_t>(
                entry.item("DeviceID").number().value_or(properties.item("DeviceID").number().value_or(0)));
            device.serial = properties.item("SerialNumber").text().value_or("");
            device.over_cable = properties.item("ConnectionType").text().value_or("") == "USB";
            if (device.identifier != 0 && !device.serial.empty())
            {
                devices.push_back(std::move(device));
            }
        }
        return devices;
    }

    outcome<device_socket> usbmux_connect(std::uint32_t device_identifier, std::uint16_t port)
    {
        outcome<device_socket> service = open_usbmuxd();
        if (!service)
        {
            return std::unexpected(service.error());
        }
        property_list request = new_request("Connect");
        request.set_number("DeviceID", device_identifier);
        // the port travels in network byte order inside a plain number
        request.set_number("PortNumber", htons(port));
        outcome<property_list> const reply = exchange(static_cast<SOCKET>(service->native()), request);
        if (!reply)
        {
            return std::unexpected(reply.error());
        }
        std::uint64_t const result = reply->root().item("Number").number().value_or(~std::uint64_t{0});
        if (result != 0)
        {
            // 2: no such device, 3: the phone refused the connection (port closed), 6: bad version
            return std::unexpected(make_failure("usbmuxd Connect", static_cast<std::int64_t>(result),
                                                std::format("the connection to port {} of the phone was refused", port)));
        }
        // from here on the socket is the connection: no timeouts of the control exchange
        DWORD const no_timeout = 0;
        SOCKET const raw = static_cast<SOCKET>(service->native());
        setsockopt(raw, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const *>(&no_timeout), sizeof(no_timeout));
        setsockopt(raw, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char const *>(&no_timeout), sizeof(no_timeout));
        BOOL const no_delay = TRUE;
        setsockopt(raw, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const *>(&no_delay), sizeof(no_delay));
        return std::move(*service);
    }

    outcome<property_list> usbmux_pair_record(std::string_view serial)
    {
        outcome<device_socket> service = open_usbmuxd();
        if (!service)
        {
            return std::unexpected(service.error());
        }
        property_list request = new_request("ReadPairRecord");
        request.set_text("PairRecordID", serial);
        outcome<property_list> const reply = exchange(static_cast<SOCKET>(service->native()), request);
        if (!reply)
        {
            return std::unexpected(reply.error());
        }
        std::optional<std::vector<std::uint8_t>> const record = reply->root().item("PairRecordData").data();
        if (!record)
        {
            return std::unexpected(make_failure(
                "usbmuxd ReadPairRecord", static_cast<std::int64_t>(reply->root().item("Number").number().value_or(0)),
                "this PC is not paired with the phone (unlock it and tap \"Trust\")"));
        }
        return property_list::parse(*record);
    }
}
