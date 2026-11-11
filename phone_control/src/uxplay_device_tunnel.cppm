// SPDX-License-Identifier: GPL-3.0-or-later
// The developer tunnel of an iPhone (iOS 17.4 and later): the lockdown service
// "CoreDeviceProxy" turns its connection into a point-to-point IPv6 link.
// After a short handshake (the phone names both addresses and the port of its
// service directory) whole IPv6 packets travel on that stream, and the
// phone's developer services listen on its end of the link.
//
// Apple's tools give the link to the operating system as a network adapter,
// which needs administrator rights.  Here the other end of the link is this
// class: a small IPv6 + TCP implementation that opens connections to the
// phone inside the tunnel, with no adapter and no special rights.
//
// What the TCP part relies on: the link is a reliable, ordered byte stream
// (TLS over the USB connection), so no packet is ever lost, duplicated or
// reordered between the two ends.  It therefore keeps sequence numbers,
// acknowledgements and both flow-control windows, and has no retransmission
// or congestion control.  It answers the phone's keep-alive probes (the
// phone resets a silent connection that does not).  UDP towards a port
// opened with discard_port() is accepted and dropped; other packets are
// ignored.
//
// All members may be called from any thread.
export module uxplay_device_tunnel;

import std;
import uxplay_status;
import uxplay_byte_stream;
import uxplay_lockdown;

export namespace uxplay
{
    class device_tunnel
    {
    public:
        // Starts the phone's tunnel service on this session and shakes hands.
        [[nodiscard]] static outcome<std::unique_ptr<device_tunnel>> open(lockdown_session &session);

        ~device_tunnel();
        device_tunnel(device_tunnel const &) = delete;
        device_tunnel &operator=(device_tunnel const &) = delete;

        // the port of the phone's service directory (RemoteServiceDiscovery)
        [[nodiscard]] std::uint16_t discovery_port() const;
        // both ends of the link, as text
        [[nodiscard]] std::string const &local_address() const;
        [[nodiscard]] std::string const &phone_address() const;

        // A TCP connection to a port of the phone, inside the tunnel.
        [[nodiscard]] outcome<std::unique_ptr<byte_stream>> connect(std::uint16_t port);

        // A UDP port of this end whose datagrams are counted and dropped: for
        // a media stream that only has to run.
        [[nodiscard]] std::uint16_t discard_port();
        [[nodiscard]] std::uint64_t discarded_datagrams() const;

        // false once the link broke (cable pulled, phone gone)
        [[nodiscard]] bool alive() const;

        struct state;

    private:
        explicit device_tunnel(std::shared_ptr<state> created_state);

        std::shared_ptr<state> impl;
    };
}
