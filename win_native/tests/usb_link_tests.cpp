// SPDX-License-Identifier: MIT
// Tests for the iPhone USB link helpers that need no iPhone: local-address
// classification (window "USB" vs "Wi-Fi" label) and the link monitor's
// lifecycle (notification registration, first evaluation, prompt shutdown,
// which matters because UxPlay must exit fast).
import std;
import uxplay_usb_link;

namespace
{
    int failures = 0;

    void check(bool condition, std::string_view what)
    {
        std::println("{} {}", condition ? "ok:  " : "FAIL:", what);
        if (!condition)
        {
            ++failures;
        }
    }

    std::string_view yes_no(bool value)
    {
        return value ? "yes" : "no";
    }
}

int main()
{
    using uxplay::is_usb_link_address;
    check(!is_usb_link_address("192.168.31.248"), "a home LAN address is not the USB link");
    check(!is_usb_link_address("172.20.11.5"), "an address outside 172.20.10.0/24 is not the USB link");
    check(is_usb_link_address("172.20.10.9"), "172.20.10.x not owned by a Wi-Fi adapter is the USB link");
    check(is_usb_link_address("::FFFF:172.20.10.9"), "the IPv4-mapped IPv6 form is recognized");
    check(!is_usb_link_address(""), "an empty address is rejected");
    check(!is_usb_link_address("not-an-address"), "a malformed address is rejected");
    check(!is_usb_link_address("fe80::1"), "an IPv6 address is not the USB link");

    std::mutex mutex;
    std::condition_variable changed;
    int notifications = 0;
    uxplay::usb_link_events events;
    events.on_change = [&](uxplay::usb_link_state const &)
    {
        {
            std::scoped_lock lock{mutex};
            ++notifications;
        }
        changed.notify_all();
    };
    auto monitor = uxplay::usb_link_monitor::create(std::move(events));
    check(monitor.has_value(), "the monitor registers for interface, address and route notifications");
    if (!monitor)
    {
        std::println("  {}", monitor.error().describe());
        return 1;
    }
    {
        std::unique_lock lock{mutex};
        changed.wait_for(lock, std::chrono::seconds{3}, [&]
        {
            return notifications > 0;
        });
    }
    uxplay::usb_link_state const state = (*monitor)->current();
    // environment-dependent facts: printed, not asserted
    std::println("  iPhone attached {}, network adapter disabled {}, link up {} {}, other internet {}, guard {}, "
                 "internet via iPhone: IPv4 {} IPv6 {}",
                 yes_no(state.iphone_attached), yes_no(state.link_adapter_disabled), yes_no(state.link_up),
                 state.local_address, yes_no(state.other_internet), yes_no(state.data_guard_applied),
                 yes_no(state.ipv4_internet_via_link), yes_no(state.ipv6_internet_via_link));
    check(!state.link_up || !state.local_address.empty(), "a link that is up reports its PC address");
    check(!state.link_adapter_disabled || state.iphone_attached, "a disabled adapter is only reported for an attached iPhone");
    check(!(state.link_adapter_disabled && state.link_up), "a disabled adapter cannot carry the link");

    auto const started = std::chrono::steady_clock::now();
    monitor->reset();
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                               started);
    std::println("  monitor shutdown took {} ms", elapsed.count());
    check(elapsed < std::chrono::milliseconds{1000}, "the monitor shuts down in under a second");

    std::println("{} failure(s)", failures);
    return failures == 0 ? 0 : 1;
}
