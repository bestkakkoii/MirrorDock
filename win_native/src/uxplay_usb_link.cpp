module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <cfgmgr32.h>
#include <objbase.h>
#include <shellapi.h>

module uxplay_usb_link;

import std;
import uxplay_status;
import uxplay_log;

namespace uxplay
{
    namespace
    {
        // 172.20.10.0/24 (host byte order): the iPhone Personal Hotspot subnet
        constexpr std::uint32_t usb_link_network = 0xAC140A00U;
        constexpr std::uint32_t usb_link_mask = 0xFFFFFF00U;
        // interface metric that keeps any other link preferred
        constexpr ULONG guarded_metric = 9999;
        constexpr auto poll_interval = std::chrono::seconds{5};
        constexpr auto settle_delay = std::chrono::milliseconds{300};
        constexpr DWORD guard_process_timeout_ms = 60'000;
        // the link needs a few seconds after attach (enumeration + DHCP)
        constexpr auto link_grace = std::chrono::seconds{8};
        // USB\VID_05AC&PID_12xx: Apple iOS devices (iPhone, iPad, iPod touch)
        constexpr std::wstring_view ios_device_prefix = L"USB\\VID_05AC&PID_12";

        bool in_usb_subnet(std::uint32_t host_order_address)
        {
            return (host_order_address & usb_link_mask) == usb_link_network;
        }

        std::optional<std::uint32_t> parse_ipv4(std::string_view text)
        {
            constexpr std::string_view mapped_prefix = "::ffff:";
            if (text.size() > mapped_prefix.size() &&
                std::ranges::equal(text.substr(0, mapped_prefix.size()), mapped_prefix,
                                   [](char left, char right)
                                   {
                                       return std::tolower(static_cast<unsigned char>(left)) == right;
                                   }))
            {
                text.remove_prefix(mapped_prefix.size());
            }
            std::string const copy{text};
            in_addr parsed{};
            if (inet_pton(AF_INET, copy.c_str(), &parsed) != 1)
            {
                return std::nullopt;
            }
            return ntohl(parsed.s_addr);
        }

        std::string format_ipv4(std::uint32_t network_order_address)
        {
            in_addr address{};
            address.s_addr = network_order_address;
            std::array<char, INET_ADDRSTRLEN> text{};
            if (inet_ntop(AF_INET, &address, text.data(), text.size()) == nullptr)
            {
                return {};
            }
            return std::string{text.data()};
        }

        bool virtual_adapter(IP_ADAPTER_ADDRESSES const &adapter)
        {
            std::wstring_view const description = adapter.Description != nullptr ? adapter.Description : L"";
            return description.contains(L"Hyper-V") || description.contains(L"Virtual") ||
                   description.contains(L"WSL") || description.contains(L"vEthernet");
        }

        bool lan_adapter(IP_ADAPTER_ADDRESSES const &adapter)
        {
            return adapter.OperStatus == IfOperStatusUp &&
                   (adapter.IfType == IF_TYPE_ETHERNET_CSMACD || adapter.IfType == IF_TYPE_IEEE80211) &&
                   !virtual_adapter(adapter);
        }

        bool has_ipv4_gateway(IP_ADAPTER_ADDRESSES const &adapter)
        {
            for (IP_ADAPTER_GATEWAY_ADDRESS_LH const *gateway = adapter.FirstGatewayAddress; gateway != nullptr;
                 gateway = gateway->Next)
            {
                if (gateway->Address.lpSockaddr != nullptr && gateway->Address.lpSockaddr->sa_family == AF_INET)
                {
                    return true;
                }
            }
            return false;
        }

        std::optional<std::uint32_t> ipv4_of(IP_ADAPTER_UNICAST_ADDRESS_LH const &unicast)
        {
            if (unicast.Address.lpSockaddr == nullptr || unicast.Address.lpSockaddr->sa_family != AF_INET)
            {
                return std::nullopt;
            }
            sockaddr_in ipv4{};
            std::memcpy(&ipv4, unicast.Address.lpSockaddr, sizeof(ipv4));
            return ipv4.sin_addr.s_addr; // network byte order
        }

        // GetAdaptersAddresses into an 8-byte aligned buffer (the adapter
        // list can grow between the size query and the fetch: retry).
        outcome<std::vector<std::uint64_t>> adapter_list(ULONG family)
        {
            ULONG const flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;
            ULONG size = 16 * 1024;
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                std::vector<std::uint64_t> buffer((size + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
                size = static_cast<ULONG>(buffer.size() * sizeof(std::uint64_t));
                ULONG const result = GetAdaptersAddresses(
                    family, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()), &size);
                if (result == NO_ERROR)
                {
                    return buffer;
                }
                if (result == ERROR_NO_DATA)
                {
                    return std::vector<std::uint64_t>{};
                }
                if (result != ERROR_BUFFER_OVERFLOW)
                {
                    return std::unexpected(make_win32_failure("GetAdaptersAddresses", result));
                }
            }
            return std::unexpected(make_win32_failure("GetAdaptersAddresses", ERROR_BUFFER_OVERFLOW));
        }

        IP_ADAPTER_ADDRESSES const *first_adapter(std::vector<std::uint64_t> const &buffer)
        {
            return buffer.empty() ? nullptr : reinterpret_cast<IP_ADAPTER_ADDRESSES const *>(buffer.data());
        }

        bool ios_device_attached()
        {
            ULONG const flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                ULONG length = 0;
                if (CM_Get_Device_ID_List_SizeW(&length, L"USB", flags) != CR_SUCCESS || length == 0)
                {
                    return false;
                }
                std::vector<wchar_t> list(length);
                CONFIGRET const result = CM_Get_Device_ID_ListW(L"USB", list.data(), length, flags);
                if (result == CR_BUFFER_SMALL)
                {
                    continue; // a device arrived between the two calls
                }
                if (result != CR_SUCCESS)
                {
                    return false;
                }
                for (wchar_t const *entry = list.data(); *entry != L'\0'; entry += std::wcslen(entry) + 1)
                {
                    std::wstring_view const id{entry};
                    if (id.size() >= ios_device_prefix.size() &&
                        std::ranges::equal(id.substr(0, ios_device_prefix.size()), ios_device_prefix,
                                           [](wchar_t left, wchar_t right)
                                           {
                                               return std::towupper(left) == right;
                                           }))
                    {
                        return true;
                    }
                }
                return false;
            }
            return false;
        }

        std::optional<MIB_IPINTERFACE_ROW> interface_row(ADDRESS_FAMILY family, NET_LUID luid)
        {
            MIB_IPINTERFACE_ROW row{};
            InitializeIpInterfaceEntry(&row);
            row.Family = family;
            row.InterfaceLuid = luid;
            if (GetIpInterfaceEntry(&row) != NO_ERROR)
            {
                return std::nullopt;
            }
            return row;
        }

        // Whether Windows' best route to a public address leaves through `luid`.
        bool best_route_uses(ADDRESS_FAMILY family, NET_LUID luid)
        {
            SOCKADDR_INET destination{};
            if (family == AF_INET)
            {
                destination.Ipv4.sin_family = AF_INET;
                inet_pton(AF_INET, "1.1.1.1", &destination.Ipv4.sin_addr);
            }
            else
            {
                destination.Ipv6.sin6_family = AF_INET6;
                inet_pton(AF_INET6, "2606:4700:4700::1111", &destination.Ipv6.sin6_addr);
            }
            MIB_IPFORWARD_ROW2 route{};
            SOCKADDR_INET source{};
            if (GetBestRoute2(nullptr, 0, nullptr, &destination, 0, &route, &source) != NO_ERROR)
            {
                return false; // no route at all for this family
            }
            return route.InterfaceLuid.Value == luid.Value;
        }

        struct notification_handle
        {
            HANDLE handle = nullptr;

            notification_handle() = default;
            notification_handle(notification_handle const &) = delete;
            notification_handle &operator=(notification_handle const &) = delete;

            ~notification_handle()
            {
                reset();
            }

            void reset()
            {
                if (handle != nullptr)
                {
                    CancelMibChangeNotify2(handle); // waits for a running callback
                    handle = nullptr;
                }
            }
        };

        struct com_scope
        {
            bool initialized = false;

            com_scope()
                : initialized{SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))}
            {
            }

            com_scope(com_scope const &) = delete;
            com_scope &operator=(com_scope const &) = delete;

            ~com_scope()
            {
                if (initialized)
                {
                    CoUninitialize();
                }
            }
        };

        std::wstring system_tool(std::wstring_view name)
        {
            std::array<wchar_t, MAX_PATH> directory{};
            UINT const length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
            if (length == 0 || length >= directory.size())
            {
                return std::wstring{name};
            }
            return std::format(L"{}\\{}", std::wstring_view{directory.data(), length}, name);
        }
    }

    bool usb_link_state::data_leak() const
    {
        return ipv4_internet_via_link || ipv6_internet_via_link;
    }

    bool is_usb_link_address(std::string_view local_address)
    {
        std::optional<std::uint32_t> const parsed = parse_ipv4(local_address);
        if (!parsed || !in_usb_subnet(*parsed))
        {
            return false;
        }
        // A PC joined to the iPhone hotspot over Wi-Fi also gets 172.20.10.x:
        // that is Wi-Fi, not the cable.
        auto adapters = adapter_list(AF_INET);
        if (!adapters)
        {
            return true;
        }
        std::uint32_t const wanted = htonl(*parsed);
        for (IP_ADAPTER_ADDRESSES const *adapter = first_adapter(*adapters); adapter != nullptr;
             adapter = adapter->Next)
        {
            for (IP_ADAPTER_UNICAST_ADDRESS_LH const *unicast = adapter->FirstUnicastAddress; unicast != nullptr;
                 unicast = unicast->Next)
            {
                if (ipv4_of(*unicast) == wanted)
                {
                    return adapter->IfType != IF_TYPE_IEEE80211;
                }
            }
        }
        return true;
    }

    struct usb_link_monitor::state
    {
        usb_link_events events;
        mutable std::mutex mutex;
        std::condition_variable wake;
        usb_link_state published;
        NET_LUID link_luid{};
        NET_IFINDEX link_ipv4_index = 0;
        NET_IFINDEX link_ipv6_index = 0;
        bool dirty = true;
        bool guard_requested = false;
        HWND guard_owner = nullptr;
        std::optional<std::chrono::steady_clock::time_point> waiting_for_link_since; // worker only
        bool stopping = false;
        notification_handle interface_changes;
        notification_handle address_changes;
        notification_handle route_changes;
        std::thread worker;

        void mark_dirty();
        void worker_loop();
        usb_link_state evaluate();
        void publish(usb_link_state const &next);
        void run_guard();

        static void WINAPI on_interface_change(void *context, MIB_IPINTERFACE_ROW *row, MIB_NOTIFICATION_TYPE type);
        static void WINAPI on_address_change(void *context, MIB_UNICASTIPADDRESS_ROW *row,
                                             MIB_NOTIFICATION_TYPE type);
        static void WINAPI on_route_change(void *context, MIB_IPFORWARD_ROW2 *row, MIB_NOTIFICATION_TYPE type);
    };

    void usb_link_monitor::state::mark_dirty()
    {
        {
            std::scoped_lock lock{mutex};
            dirty = true;
        }
        wake.notify_all();
    }

    void WINAPI usb_link_monitor::state::on_interface_change(void *context, MIB_IPINTERFACE_ROW *,
                                                             MIB_NOTIFICATION_TYPE)
    {
        static_cast<state *>(context)->mark_dirty();
    }

    void WINAPI usb_link_monitor::state::on_address_change(void *context, MIB_UNICASTIPADDRESS_ROW *,
                                                           MIB_NOTIFICATION_TYPE)
    {
        static_cast<state *>(context)->mark_dirty();
    }

    void WINAPI usb_link_monitor::state::on_route_change(void *context, MIB_IPFORWARD_ROW2 *, MIB_NOTIFICATION_TYPE)
    {
        static_cast<state *>(context)->mark_dirty();
    }

    usb_link_state usb_link_monitor::state::evaluate()
    {
        usb_link_state next;
        next.iphone_attached = ios_device_attached();
        NET_LUID luid{};
        NET_IFINDEX ipv4_index = 0;
        NET_IFINDEX ipv6_index = 0;

        auto adapters = adapter_list(AF_UNSPEC);
        if (!adapters)
        {
            log::warning("USB link: adapter scan: {}", adapters.error().describe());
        }
        else
        {
            for (IP_ADAPTER_ADDRESSES const *adapter = first_adapter(*adapters); adapter != nullptr;
                 adapter = adapter->Next)
            {
                if (!lan_adapter(*adapter))
                {
                    continue;
                }
                bool is_link = false;
                if (adapter->IfType != IF_TYPE_IEEE80211 && !next.link_up)
                {
                    for (IP_ADAPTER_UNICAST_ADDRESS_LH const *unicast = adapter->FirstUnicastAddress;
                         unicast != nullptr; unicast = unicast->Next)
                    {
                        std::optional<std::uint32_t> const address = ipv4_of(*unicast);
                        if (address && unicast->DadState == IpDadStatePreferred && in_usb_subnet(ntohl(*address)))
                        {
                            is_link = true;
                            next.link_up = true;
                            next.local_address = format_ipv4(*address);
                            luid = adapter->Luid;
                            ipv4_index = adapter->IfIndex;
                            ipv6_index = adapter->Ipv6IfIndex;
                            break;
                        }
                    }
                }
                if (!is_link && has_ipv4_gateway(*adapter))
                {
                    next.other_internet = true;
                }
            }
        }

        if (next.iphone_attached && !next.link_up)
        {
            auto const now = std::chrono::steady_clock::now();
            if (!waiting_for_link_since)
            {
                waiting_for_link_since = now;
            }
            next.link_missing = now - *waiting_for_link_since >= link_grace;
        }
        else
        {
            waiting_for_link_since.reset();
        }

        if (next.link_up)
        {
            std::optional<MIB_IPINTERFACE_ROW> const ipv4 = interface_row(AF_INET, luid);
            std::optional<MIB_IPINTERFACE_ROW> const ipv6 = interface_row(AF_INET6, luid);
            bool const ipv4_guarded = ipv4 && !ipv4->UseAutomaticMetric && ipv4->Metric >= guarded_metric;
            bool const ipv6_guarded = !ipv6 || (ipv6->RouterDiscoveryBehavior == RouterDiscoveryDisabled &&
                                                ipv6->Metric >= guarded_metric);
            next.data_guard_applied = ipv4_guarded && ipv6_guarded;
            next.ipv4_internet_via_link = best_route_uses(AF_INET, luid);
            next.ipv6_internet_via_link = best_route_uses(AF_INET6, luid);
        }

        std::scoped_lock lock{mutex};
        link_luid = luid;
        link_ipv4_index = ipv4_index;
        link_ipv6_index = ipv6_index;
        return next;
    }

    void usb_link_monitor::state::publish(usb_link_state const &next)
    {
        {
            std::scoped_lock lock{mutex};
            if (next == published)
            {
                return;
            }
            published = next;
        }
        if (events.on_change)
        {
            events.on_change(next);
        }
    }

    void usb_link_monitor::state::run_guard()
    {
        NET_IFINDEX ipv4_index = 0;
        NET_IFINDEX ipv6_index = 0;
        HWND owner = nullptr;
        {
            std::scoped_lock lock{mutex};
            ipv4_index = link_ipv4_index;
            ipv6_index = link_ipv6_index;
            owner = guard_owner;
        }
        if (ipv4_index == 0)
        {
            if (events.on_guard_result)
            {
                events.on_guard_result(false, "the iPhone USB link is not up");
            }
            return;
        }

        // One elevated, Microsoft-signed process applies both families,
        // persistently (survives replug and reboot): the link stays usable
        // for the mirror, but neither IPv4 nor IPv6 internet traffic is ever
        // routed over it.  Arguments are fixed numbers: no injection surface.
        std::wstring const netsh = system_tool(L"netsh.exe");
        std::wstring command = std::format(L"/d /c \"\"{}\" interface ipv4 set interface interface={} metric={} "
                                           L"ignoredefaultroutes=enabled store=persistent",
                                           netsh, ipv4_index, guarded_metric);
        if (ipv6_index != 0)
        {
            command += std::format(L" & \"{}\" interface ipv6 set interface interface={} metric={} "
                                   L"routerdiscovery=disabled ignoredefaultroutes=enabled store=persistent",
                                   netsh, ipv6_index, guarded_metric);
        }
        command += L"\"";
        std::wstring const shell = system_tool(L"cmd.exe");

        com_scope const com;
        SHELLEXECUTEINFOW request{};
        request.cbSize = sizeof(request);
        request.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        request.hwnd = owner;
        request.lpVerb = L"runas";
        request.lpFile = shell.c_str();
        request.lpParameters = command.c_str();
        request.nShow = SW_HIDE;
        log::info("USB link: asking for administrator consent to keep PC internet off the iPhone's cellular data "
                  "(interface {})",
                  ipv4_index);
        if (ShellExecuteExW(&request) == FALSE)
        {
            DWORD const error = GetLastError();
            std::string const detail = error == ERROR_CANCELLED
                                           ? std::string{"administrator consent was declined"}
                                           : make_win32_failure("ShellExecuteExW(runas netsh)", error).describe();
            log::warning("USB link: data guard not applied: {}", detail);
            if (events.on_guard_result)
            {
                events.on_guard_result(false, detail);
            }
            return;
        }
        DWORD exit_code = 0;
        if (request.hProcess != nullptr)
        {
            DWORD const waited = WaitForSingleObject(request.hProcess, guard_process_timeout_ms);
            if (waited != WAIT_OBJECT_0 || GetExitCodeProcess(request.hProcess, &exit_code) == FALSE)
            {
                exit_code = waited == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError();
            }
            CloseHandle(request.hProcess);
        }

        // the route table settles a moment after netsh returns
        usb_link_state after = evaluate();
        for (int check = 0; check < 10 && !(after.data_guard_applied && !after.data_leak()); ++check)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{300});
            after = evaluate();
        }
        publish(after);
        bool const applied = after.data_guard_applied && !after.data_leak();
        std::string const detail =
            std::format("netsh exit code {}; guard markers {}; internet via iPhone: IPv4 {}, IPv6 {}", exit_code,
                        after.data_guard_applied ? "set" : "missing", after.ipv4_internet_via_link,
                        after.ipv6_internet_via_link);
        if (applied)
        {
            log::info("USB link: data guard applied ({})", detail);
        }
        else
        {
            log::warning("USB link: data guard incomplete ({})", detail);
        }
        if (events.on_guard_result)
        {
            events.on_guard_result(applied, detail);
        }
    }

    void usb_link_monitor::state::worker_loop()
    {
        for (;;)
        {
            bool guard_now = false;
            {
                std::unique_lock lock{mutex};
                wake.wait_for(lock, poll_interval, [this]
                {
                    return stopping || dirty || guard_requested;
                });
                if (stopping)
                {
                    return;
                }
                if (dirty)
                {
                    // let a burst of address/route notifications settle
                    wake.wait_for(lock, settle_delay, [this]
                    {
                        return stopping;
                    });
                    if (stopping)
                    {
                        return;
                    }
                }
                dirty = false;
                guard_now = std::exchange(guard_requested, false);
            }
            publish(evaluate());
            if (guard_now)
            {
                run_guard();
            }
        }
    }

    usb_link_monitor::usb_link_monitor(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    usb_link_monitor::~usb_link_monitor()
    {
        // stop callbacks first: they reference the state
        impl->interface_changes.reset();
        impl->address_changes.reset();
        impl->route_changes.reset();
        {
            std::scoped_lock lock{impl->mutex};
            impl->stopping = true;
        }
        impl->wake.notify_all();
        if (impl->worker.joinable())
        {
            impl->worker.join();
        }
    }

    outcome<std::unique_ptr<usb_link_monitor>> usb_link_monitor::create(usb_link_events events)
    {
        auto created = std::make_unique<state>();
        created->events = std::move(events);
        state *const shared = created.get();
        if (DWORD const result = NotifyIpInterfaceChange(AF_UNSPEC, &state::on_interface_change, shared, FALSE,
                                                         &created->interface_changes.handle);
            result != NO_ERROR)
        {
            return std::unexpected(make_win32_failure("NotifyIpInterfaceChange", result));
        }
        if (DWORD const result = NotifyUnicastIpAddressChange(AF_UNSPEC, &state::on_address_change, shared, FALSE,
                                                              &created->address_changes.handle);
            result != NO_ERROR)
        {
            return std::unexpected(make_win32_failure("NotifyUnicastIpAddressChange", result));
        }
        if (DWORD const result =
                NotifyRouteChange2(AF_UNSPEC, &state::on_route_change, shared, FALSE, &created->route_changes.handle);
            result != NO_ERROR)
        {
            return std::unexpected(make_win32_failure("NotifyRouteChange2", result));
        }
        created->worker = std::thread{[shared]
        {
            shared->worker_loop();
        }};
        return std::unique_ptr<usb_link_monitor>{new usb_link_monitor{std::move(created)}};
    }

    usb_link_state usb_link_monitor::current() const
    {
        std::scoped_lock lock{impl->mutex};
        return impl->published;
    }

    void usb_link_monitor::refresh()
    {
        impl->mark_dirty();
    }

    void usb_link_monitor::apply_data_guard(void *owner_window)
    {
        {
            std::scoped_lock lock{impl->mutex};
            impl->guard_requested = true;
            impl->guard_owner = static_cast<HWND>(owner_window);
        }
        impl->wake.notify_all();
    }
}
