// SPDX-License-Identifier: GPL-3.0-or-later
// usbmuxd: the service of Apple's driver package (Apple Mobile Device
// Service, 127.0.0.1:27015) that knows the iPhones attached to this PC and
// opens TCP connections to ports on them, through the USB cable.
//
// Messages are property lists behind a 16-byte header.  After a successful
// "Connect" the socket carries the bytes of that connection and nothing else.
export module uxplay_usbmux;

import std;
import uxplay_status;
import uxplay_property_list;

export namespace uxplay
{
    struct usbmux_device
    {
        std::uint32_t identifier = 0; // usbmuxd's number for it, valid while it stays attached
        std::string serial;           // the phone's identifier (not to be logged)
        bool over_cable = false;      // else: over the network (Wi-Fi sync)
    };

    // A TCP connection to a port of the phone, made through usbmuxd.  Owns the socket.
    class device_socket
    {
    public:
        device_socket() = default;
        explicit device_socket(std::uintptr_t adopted);
        ~device_socket();
        device_socket(device_socket &&other) noexcept;
        device_socket &operator=(device_socket &&other) noexcept;
        device_socket(device_socket const &) = delete;
        device_socket &operator=(device_socket const &) = delete;

        [[nodiscard]] bool valid() const;
        // the SOCKET
        [[nodiscard]] std::uintptr_t native() const;

    private:
        std::uintptr_t handle = ~std::uintptr_t{0};
    };

    [[nodiscard]] outcome<std::vector<usbmux_device>> usbmux_devices();
    [[nodiscard]] outcome<device_socket> usbmux_connect(std::uint32_t device_identifier, std::uint16_t port);
    // The record of the pairing between this PC and the phone, as usbmuxd
    // keeps it.  It holds private keys: its content is never logged.
    [[nodiscard]] outcome<property_list> usbmux_pair_record(std::string_view serial);
}
