// SPDX-License-Identifier: MIT
// The iPhone's wired network link: Personal Hotspot over the USB cable
// ("Apple Mobile Device Ethernet", always 172.20.10.0/28).  AirPlay mirroring
// rides this link when it is up (USB first, Wi-Fi second); the mirror itself
// is local traffic and uses no cellular data.
//
// The monitor watches for the iPhone being attached and for its link coming
// up, so the notification area can tell the user what to do ("turn on
// Personal Hotspot") and which link is in use.  It also detects when Windows
// routes the PC's own internet traffic through the iPhone (cellular data) and
// offers a persistent data guard: the link keeps carrying the mirror but is
// never used as an internet route.  The iPhone hands out global IPv6 even
// when the home Wi-Fi has none, so without the guard every IPv6-capable
// connection of the PC would silently use the phone's mobile data.
export module uxplay_usb_link;

import std;
import uxplay_status;

export namespace uxplay
{
    struct usb_link_state
    {
        bool iphone_attached = false;    // an iPhone/iPad is attached by USB
        bool link_up = false;            // its network link (Personal Hotspot) is up
        bool link_missing = false;       // attached for a while, still no link: hotspot off
        bool link_adapter_disabled = false; // its "Apple Mobile Device Ethernet" is disabled in Windows
        std::string local_address;       // PC side of the link, e.g. "172.20.10.9"
        bool other_internet = false;     // another adapter (Wi-Fi/LAN) has a default gateway
        bool data_guard_applied = false; // internet routing over the link is disabled
        bool ipv4_internet_via_link = false;
        bool ipv6_internet_via_link = false;

        [[nodiscard]] bool data_leak() const;
        bool operator==(usb_link_state const &other) const = default;
    };

    // administrator actions the monitor can run (each behind a UAC prompt)
    enum class usb_link_action
    {
        data_guard,    // keep PC internet off the iPhone link
        enable_adapter // re-enable a disabled "Apple Mobile Device Ethernet"
    };

    struct usb_link_events
    {
        // the state changed (monitor thread; must not block)
        std::function<void(usb_link_state const &state)> on_change;
        // an administrator action finished: done, or why not
        std::function<void(usb_link_action action, bool done, std::string const &detail)> on_admin_result;
    };

    // True when local_address (dotted IPv4, IPv4-mapped IPv6, or an IPv6
    // address, link-local ones as "fe80::...%<interface index>") is the PC side
    // of the iPhone USB link rather than a Wi-Fi/LAN address.
    [[nodiscard]] bool is_usb_link_address(std::string_view local_address);

    class usb_link_monitor
    {
    public:
        static outcome<std::unique_ptr<usb_link_monitor>> create(usb_link_events events);

        ~usb_link_monitor();
        usb_link_monitor(usb_link_monitor const &) = delete;
        usb_link_monitor &operator=(usb_link_monitor const &) = delete;

        [[nodiscard]] usb_link_state current() const;

        // re-check now (a USB device arrived or was removed)
        void refresh();

        // Persistently stops Windows from routing internet traffic over the
        // iPhone link (IPv4 and IPv6), keeping the link for the mirror.
        // Needs administrator consent (UAC, owned by owner_window); runs on
        // the monitor thread.
        void apply_data_guard(void *owner_window);

        // Re-enables the iPhone's network adapter after it was disabled in
        // Windows (needed for AirPlay over the cable).  Needs administrator
        // consent (UAC); runs on the monitor thread.
        void enable_link_adapter(void *owner_window);

    private:
        struct state;

        explicit usb_link_monitor(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
