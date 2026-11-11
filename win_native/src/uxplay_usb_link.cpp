// SPDX-License-Identifier: MIT
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
        // Address/interface notifications and WM_DEVICECHANGE drive the
        // monitor.  Only the end of the grace period after an attach arrives
        // without any notification, so the state is polled while that is
        // awaited; otherwise the poll is a rarely needed safety net.
        constexpr auto waiting_poll_interval = std::chrono::seconds{5};
        constexpr auto idle_poll_interval = std::chrono::seconds{60};
        constexpr auto settle_delay = std::chrono::milliseconds{300};
        constexpr DWORD guard_process_timeout_ms = 60'000;
        // the link needs a few seconds after attach (enumeration + DHCP)
        constexpr auto link_grace = std::chrono::seconds{8};
        // USB\VID_05AC&PID_12xx: Apple iOS devices (iPhone, iPad, iPod touch)
        constexpr std::wstring_view ios_device_prefix = L"USB\\VID_05AC&PID_12";
        // device setup class "Net" (network adapters)
        constexpr std::wstring_view net_class_guid = L"{4d36e972-e325-11ce-bfc1-08002be10318}";
        // pnputil success, or success that needs a restart
        constexpr DWORD pnputil_reboot_required = 3010;

        bool equal_ignoring_case(std::wstring_view left, std::wstring_view right)
        {
            return left.size() == right.size() &&
                   std::ranges::equal(left, right,
                                      [](wchar_t a, wchar_t b)
                                      {
                                          return std::towupper(a) == std::towupper(b);
                                      });
        }

        // Device instance IDs are passed to an elevated pnputil: accept only
        // the characters Windows itself uses in them (no quotes or spaces).
        bool safe_instance_id(std::wstring_view id)
        {
            return !id.empty() && id.size() <= 200 && std::ranges::all_of(id, [](wchar_t c)
            {
                return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') ||
                       c == L'&' || c == L'_' || c == L'\\' || c == L'.' || c == L'-';
            });
        }

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

        // A device function that the user disabled (Device Manager or
        // "Disable" on the network adapter): problem CM_PROB_DISABLED.
        bool disabled_network_function(std::wstring_view instance_id)
        {
            std::wstring id{instance_id};
            DEVINST node = 0;
            if (CM_Locate_DevNodeW(&node, id.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
            {
                return false;
            }
            ULONG status = 0;
            ULONG problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, node, 0) != CR_SUCCESS || (status & DN_HAS_PROBLEM) == 0 ||
                problem != CM_PROB_DISABLED)
            {
                return false;
            }
            std::array<wchar_t, 64> class_guid{};
            ULONG size = static_cast<ULONG>(class_guid.size() * sizeof(wchar_t));
            if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_CLASSGUID, nullptr, class_guid.data(), &size, 0) !=
                CR_SUCCESS)
            {
                return false;
            }
            return equal_ignoring_case(std::wstring_view{class_guid.data()}, net_class_guid);
        }

        struct ios_device_scan
        {
            bool attached = false;
            std::wstring disabled_network_function; // instance ID, empty when none
        };

        ios_device_scan scan_ios_devices()
        {
            ios_device_scan found;
            ULONG const flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                ULONG length = 0;
                if (CM_Get_Device_ID_List_SizeW(&length, L"USB", flags) != CR_SUCCESS || length == 0)
                {
                    return found;
                }
                std::vector<wchar_t> list(length);
                CONFIGRET const result = CM_Get_Device_ID_ListW(L"USB", list.data(), length, flags);
                if (result == CR_BUFFER_SMALL)
                {
                    continue; // a device arrived between the two calls
                }
                if (result != CR_SUCCESS)
                {
                    return found;
                }
                for (wchar_t const *entry = list.data(); *entry != L'\0'; entry += std::wcslen(entry) + 1)
                {
                    std::wstring_view const id{entry};
                    if (id.size() < ios_device_prefix.size() ||
                        !equal_ignoring_case(id.substr(0, ios_device_prefix.size()), ios_device_prefix))
                    {
                        continue;
                    }
                    found.attached = true;
                    if (found.disabled_network_function.empty() && disabled_network_function(id))
                    {
                        found.disabled_network_function = std::wstring{id};
                    }
                }
                return found;
            }
            return found;
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

        // Runs a Microsoft-signed system tool elevated (UAC prompt owned by
        // `owner`) and returns its exit code.  A declined prompt is a failure
        // with code ERROR_CANCELLED.
        outcome<DWORD> run_elevated(std::wstring const &file, std::wstring const &parameters, HWND owner)
        {
            com_scope const com;
            SHELLEXECUTEINFOW request{};
            request.cbSize = sizeof(request);
            request.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            request.hwnd = owner;
            request.lpVerb = L"runas";
            request.lpFile = file.c_str();
            request.lpParameters = parameters.c_str();
            request.nShow = SW_HIDE;
            if (ShellExecuteExW(&request) == FALSE)
            {
                DWORD const error = GetLastError();
                if (error == ERROR_CANCELLED)
                {
                    return std::unexpected(make_failure("administrator consent", ERROR_CANCELLED,
                                                        "administrator consent was declined"));
                }
                return std::unexpected(make_win32_failure("ShellExecuteExW(runas)", error));
            }
            if (request.hProcess == nullptr)
            {
                return std::unexpected(make_failure("ShellExecuteExW(runas)", 0, "no process handle returned"));
            }
            DWORD exit_code = 0;
            DWORD const waited = WaitForSingleObject(request.hProcess, guard_process_timeout_ms);
            bool const got_code = waited == WAIT_OBJECT_0 && GetExitCodeProcess(request.hProcess, &exit_code) != FALSE;
            DWORD const wait_error = got_code ? 0 : (waited == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError());
            CloseHandle(request.hProcess);
            if (!got_code)
            {
                return std::unexpected(make_failure("waiting for the elevated tool", wait_error,
                                                    waited == WAIT_TIMEOUT ? "timed out" : "wait failed"));
            }
            return exit_code;
        }
    }

    bool usb_link_state::data_leak() const
    {
        return ipv4_internet_via_link || ipv6_internet_via_link;
    }

    bool is_usb_link_address(std::string_view local_address)
    {
        if (std::optional<std::uint32_t> const parsed = parse_ipv4(local_address); parsed)
        {
            if (!in_usb_subnet(*parsed))
            {
                return false;
            }
            // A PC joined to the iPhone hotspot over Wi-Fi also gets
            // 172.20.10.x: that is Wi-Fi, not the cable.
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
        // IPv6 (the link also carries link-local and carrier addresses): the
        // address is on the USB link when its adapter holds the 172.20.10.x
        // address and is not a Wi-Fi adapter.  "fe80::...%<zone>" names the
        // adapter by its IPv6 interface index.
        std::string text{local_address};
        std::optional<unsigned long> zone;
        if (std::size_t const percent = text.find('%'); percent != std::string::npos)
        {
            unsigned long parsed_zone = 0;
            char const *const first = text.data() + percent + 1;
            char const *const last = text.data() + text.size();
            if (auto const [end, error] = std::from_chars(first, last, parsed_zone);
                error != std::errc{} || end != last || parsed_zone == 0)
            {
                return false;
            }
            zone = parsed_zone;
            text.resize(percent);
        }
        in6_addr wanted{};
        if (inet_pton(AF_INET6, text.c_str(), &wanted) != 1)
        {
            return false;
        }
        auto adapters = adapter_list(AF_UNSPEC);
        if (!adapters)
        {
            // cannot tell: not claiming USB keeps "Wi-Fi only" from refusing a LAN client
            log::warning("USB link: adapter list unavailable while classifying {}: {}", local_address,
                         adapters.error().describe());
            return false;
        }
        for (IP_ADAPTER_ADDRESSES const *adapter = first_adapter(*adapters); adapter != nullptr;
             adapter = adapter->Next)
        {
            bool owns_address = false;
            bool holds_usb_subnet = false;
            for (IP_ADAPTER_UNICAST_ADDRESS_LH const *unicast = adapter->FirstUnicastAddress; unicast != nullptr;
                 unicast = unicast->Next)
            {
                if (std::optional<std::uint32_t> const ipv4 = ipv4_of(*unicast); ipv4)
                {
                    holds_usb_subnet = holds_usb_subnet || in_usb_subnet(ntohl(*ipv4));
                }
                else if (unicast->Address.lpSockaddr != nullptr &&
                         unicast->Address.lpSockaddr->sa_family == AF_INET6)
                {
                    sockaddr_in6 ipv6{};
                    std::memcpy(&ipv6, unicast->Address.lpSockaddr, sizeof(ipv6));
                    owns_address = owns_address || std::memcmp(&ipv6.sin6_addr, &wanted, sizeof(wanted)) == 0;
                }
            }
            if (owns_address && (!zone || adapter->Ipv6IfIndex == *zone))
            {
                return holds_usb_subnet && adapter->IfType != IF_TYPE_IEEE80211;
            }
        }
        return false;
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
        std::wstring disabled_adapter_id; // last scan: disabled network function of the iPhone
        bool dirty = true;
        bool guard_requested = false;
        bool enable_requested = false;
        HWND action_owner = nullptr;
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
        void report(usb_link_action action, bool done, std::string const &detail);
        void run_guard();
        void run_enable_adapter();

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
        ios_device_scan const devices = scan_ios_devices();
        next.iphone_attached = devices.attached;
        next.link_adapter_disabled = !devices.disabled_network_function.empty();
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

        if (next.iphone_attached && !next.link_up && !next.link_adapter_disabled)
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
        disabled_adapter_id = devices.disabled_network_function;
        return next;
    }

    void usb_link_monitor::state::report(usb_link_action action, bool done, std::string const &detail)
    {
        if (events.on_admin_result)
        {
            events.on_admin_result(action, done, detail);
        }
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
            owner = action_owner;
        }
        if (ipv4_index == 0)
        {
            report(usb_link_action::data_guard, false, "the iPhone USB link is not up");
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

        log::info("USB link: asking for administrator consent to keep PC internet off the iPhone's cellular data "
                  "(interface {})",
                  ipv4_index);
        outcome<DWORD> const ran = run_elevated(system_tool(L"cmd.exe"), command, owner);
        if (!ran)
        {
            std::string const detail = ran.error().describe();
            log::warning("USB link: data guard not applied: {}", detail);
            report(usb_link_action::data_guard, false, detail);
            return;
        }
        DWORD const exit_code = *ran;

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
        report(usb_link_action::data_guard, applied, detail);
    }

    void usb_link_monitor::state::run_enable_adapter()
    {
        std::wstring id;
        HWND owner = nullptr;
        {
            std::scoped_lock lock{mutex};
            id = disabled_adapter_id;
            owner = action_owner;
        }
        if (id.empty())
        {
            report(usb_link_action::enable_adapter, true, "the iPhone network adapter is not disabled");
            return;
        }
        if (!safe_instance_id(id))
        {
            std::string const detail = std::format("refusing unexpected device instance ID \"{}\"", utf16_to_utf8(id));
            log::warning("USB link: {}", detail);
            report(usb_link_action::enable_adapter, false, detail);
            return;
        }

        // pnputil (Windows 10 2004+) enables exactly that device function; it
        // touches neither the rest of the iPhone's USB device nor charging.
        log::info("USB link: asking for administrator consent to re-enable {}", utf16_to_utf8(id));
        outcome<DWORD> const ran =
            run_elevated(system_tool(L"pnputil.exe"), std::format(L"/enable-device \"{}\"", id), owner);
        if (!ran)
        {
            std::string const detail = ran.error().describe();
            log::warning("USB link: adapter not re-enabled: {}", detail);
            report(usb_link_action::enable_adapter, false, detail);
            return;
        }

        usb_link_state after = evaluate();
        for (int check = 0; check < 10 && after.link_adapter_disabled; ++check)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{300});
            after = evaluate();
        }
        publish(after);
        bool const enabled = !after.link_adapter_disabled && (*ran == 0 || *ran == pnputil_reboot_required);
        std::string const detail = std::format("pnputil exit code {}; adapter {}", *ran,
                                               after.link_adapter_disabled ? "still disabled" : "enabled");
        if (enabled)
        {
            log::info("USB link: iPhone network adapter re-enabled ({})", detail);
        }
        else
        {
            log::warning("USB link: re-enabling the iPhone network adapter failed ({})", detail);
        }
        report(usb_link_action::enable_adapter, enabled, detail);
    }

    void usb_link_monitor::state::worker_loop()
    {
        bool awaiting_link = true; // first evaluation follows at once (dirty)
        for (;;)
        {
            bool guard_now = false;
            bool enable_now = false;
            {
                std::unique_lock lock{mutex};
                wake.wait_for(lock, awaiting_link ? waiting_poll_interval : idle_poll_interval, [this]
                {
                    return stopping || dirty || guard_requested || enable_requested;
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
                enable_now = std::exchange(enable_requested, false);
            }
            usb_link_state const evaluated = evaluate();
            awaiting_link = waiting_for_link_since.has_value() && !evaluated.link_missing;
            publish(evaluated);
            if (enable_now)
            {
                run_enable_adapter();
            }
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
            impl->action_owner = static_cast<HWND>(owner_window);
        }
        impl->wake.notify_all();
    }

    void usb_link_monitor::enable_link_adapter(void *owner_window)
    {
        {
            std::scoped_lock lock{impl->mutex};
            impl->enable_requested = true;
            impl->action_owner = static_cast<HWND>(owner_window);
        }
        impl->wake.notify_all();
    }
}
