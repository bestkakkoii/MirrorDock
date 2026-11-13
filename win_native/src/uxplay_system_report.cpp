// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include <objbase.h>
#include <oleauto.h>
#include <netfw.h>
#include <netlistmgr.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <dxgi1_6.h>
#include <shellscalingapi.h>
#include <wrl/client.h>

#include "uxplay_version.h"

module uxplay_system_report;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_ui_text;
import uxplay_gpu_device;
import uxplay_update_check;
import uxplay_usbmux;
import uxplay_lockdown;
import uxplay_property_list;

namespace uxplay
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr std::size_t group_count = static_cast<std::size_t>(report_group::count);

        // ---- how the rows are judged ------------------------------------------
        // weights: the share of a row in the score
        constexpr unsigned int weight_windows = 6;
        constexpr unsigned int weight_display = 10;
        constexpr unsigned int weight_power = 4;
        constexpr unsigned int weight_network = 10;
        constexpr unsigned int weight_firewall = 10;
        constexpr unsigned int weight_processor = 12;
        constexpr unsigned int weight_memory = 8;
        constexpr unsigned int weight_graphics = 10;
        constexpr unsigned int weight_decoding = 25;
        constexpr unsigned int weight_sound = 5;
        constexpr unsigned int weight_graphics_driver = 15;
        constexpr unsigned int weight_apple_support = 10;
        constexpr unsigned int weight_usb_link = 5;
        constexpr unsigned int weight_program = 8;
        // Windows 10 1809: the oldest version the program's window and
        // presentation paths are written for
        constexpr std::uint32_t oldest_good_windows_build = 17763;
        // the phone sends 60 pictures a second
        constexpr double slowest_good_display_hz = 59.0;
        // monitors that show the same desktop count as refreshing alike within this
        constexpr double uneven_refresh_hz = 0.5;
        // link rate below which a mirror stream has little headroom
        constexpr std::uint64_t slowest_good_network_bits = 50'000'000;
        constexpr unsigned int fewest_good_threads = 4;
        constexpr std::uint64_t gibibyte = 1024ULL * 1024 * 1024;
        constexpr double least_good_memory_gib = 7.5;
        constexpr double least_usable_memory_gib = 3.5;
        // remote control (uxplay_remote_control: first_ios_with_input)
        constexpr int first_ios_with_control = 27;
        constexpr char developer_mode_domain[] = "com.apple.security.mac.amfi";

        // device properties (devpkey.h; written out so that no GUID library is needed)
        constexpr DEVPROPKEY key_device_description = {
            {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};
        constexpr DEVPROPKEY key_driver_date = {
            {0xa8b865dd, 0x2e3d, 0x4094, {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}}, 2};
        constexpr DEVPROPKEY key_driver_version = {
            {0xa8b865dd, 0x2e3d, 0x4094, {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}}, 3};
        constexpr DEVPROPKEY key_driver_provider = {
            {0xa8b865dd, 0x2e3d, 0x4094, {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}}, 9};
        // PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h)
        constexpr PROPERTYKEY key_endpoint_name = {
            {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
        constexpr wchar_t display_class[] = L"{4d36e968-e325-11ce-bfc1-08002be10318}";

        struct com_scope
        {
            HRESULT entered = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

            com_scope() = default;
            com_scope(com_scope const &) = delete;
            com_scope &operator=(com_scope const &) = delete;

            ~com_scope()
            {
                if (SUCCEEDED(entered))
                {
                    CoUninitialize();
                }
            }
        };

        [[nodiscard]] std::wstring trimmed(std::wstring_view text)
        {
            std::size_t const first = text.find_first_not_of(L" \t");
            if (first == std::wstring_view::npos)
            {
                return {};
            }
            return std::wstring{text.substr(first, text.find_last_not_of(L" \t") - first + 1)};
        }

        [[nodiscard]] std::wstring upper_ascii(std::wstring_view text)
        {
            std::wstring result{text};
            for (wchar_t &character : result)
            {
                if (character >= L'a' && character <= L'z')
                {
                    character = static_cast<wchar_t>(character - L'a' + L'A');
                }
            }
            return result;
        }

        [[nodiscard]] std::wstring registry_text(HKEY root, wchar_t const *key, wchar_t const *name, DWORD view = 0)
        {
            wchar_t buffer[512]{};
            DWORD size = sizeof(buffer) - sizeof(wchar_t);
            if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | view, nullptr, buffer, &size) != ERROR_SUCCESS)
            {
                return {};
            }
            return trimmed(buffer);
        }

        [[nodiscard]] std::optional<std::uint32_t> registry_number(HKEY root, wchar_t const *key, wchar_t const *name)
        {
            DWORD value = 0;
            DWORD size = sizeof(value);
            if (RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
            {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]] std::wstring executable_path()
        {
            std::wstring path(1024, L'\0');
            DWORD const length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0 || length >= path.size())
            {
                return {};
            }
            path.resize(length);
            return path;
        }

        // ---- system -------------------------------------------------------------

        void measure_windows(system_facts &facts)
        {
            constexpr wchar_t key[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
            std::wstring name = registry_text(HKEY_LOCAL_MACHINE, key, L"ProductName");
            std::wstring const build = registry_text(HKEY_LOCAL_MACHINE, key, L"CurrentBuildNumber");
            for (wchar_t const digit : build)
            {
                if (digit < L'0' || digit > L'9' || facts.windows_build > 100'000'000)
                {
                    facts.windows_build = 0;
                    break;
                }
                facts.windows_build = facts.windows_build * 10 + static_cast<std::uint32_t>(digit - L'0');
            }
            facts.windows_revision = registry_number(HKEY_LOCAL_MACHINE, key, L"UBR").value_or(0);
            // Windows 11 still says "Windows 10" in this value
            if (constexpr std::wstring_view ten = L"Windows 10"; facts.windows_build >= 22000 && name.starts_with(ten))
            {
                name.replace(0, ten.size(), L"Windows 11");
            }
            if (std::wstring const version = registry_text(HKEY_LOCAL_MACHINE, key, L"DisplayVersion");
                !version.empty() && !name.empty())
            {
                name += L' ';
                name += version;
            }
            facts.windows_name = std::move(name);
        }

        void measure_display(system_facts &facts)
        {
            if (auto const mode = gpu_device::primary_display_mode(); mode)
            {
                facts.display_width = mode->width;
                facts.display_height = mode->height;
                for (display_target_info const &target : mode->targets)
                {
                    display_facts monitor;
                    if (target.name_known)
                    {
                        monitor.name = utf8_to_utf16(target.monitor_name);
                    }
                    monitor.built_in = target.internal_panel;
                    monitor.hz = target.refresh_hz > 0.0 ? target.refresh_hz : mode->refresh_hz();
                    facts.displays.push_back(std::move(monitor));
                }
                if (facts.displays.empty())
                {
                    display_facts monitor;
                    monitor.hz = mode->refresh_hz();
                    facts.displays.push_back(std::move(monitor));
                }
            }
            UINT dpi_x = 0;
            UINT dpi_y = 0;
            if (SUCCEEDED(GetDpiForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI,
                                           &dpi_x, &dpi_y)) &&
                dpi_x != 0)
            {
                facts.display_scale_percent = MulDiv(static_cast<int>(dpi_x), 100, 96);
            }
        }

        void measure_power(system_facts &facts)
        {
            SYSTEM_POWER_STATUS power{};
            if (GetSystemPowerStatus(&power) == FALSE)
            {
                return;
            }
            // BatteryFlag: 128 "no system battery", 255 "unknown"
            facts.battery_present = power.BatteryFlag != 128 && power.BatteryFlag != 255;
            facts.on_battery = facts.battery_present && power.ACLineStatus == 0;
            facts.energy_saver = power.SystemStatusFlag == 1;
            facts.battery_percent = power.BatteryLifePercent <= 100 ? power.BatteryLifePercent : -1;
        }

        void measure_network(system_facts &facts)
        {
            ULONG const flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                GAA_FLAG_SKIP_DNS_SERVER;
            std::vector<std::byte> buffer;
            ULONG size = 32 * 1024;
            ULONG result = ERROR_BUFFER_OVERFLOW;
            for (int attempt = 0; attempt < 4 && result == ERROR_BUFFER_OVERFLOW; ++attempt)
            {
                buffer.resize(size);
                result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                              reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()), &size);
            }
            facts.metered = metered_connection();
            if (result != NO_ERROR)
            {
                return;
            }
            // the adapter Windows routes through: up, with a gateway, wired or
            // Wi-Fi, and not the iPhone's own USB link
            IP_ADAPTER_ADDRESSES const *chosen = nullptr;
            for (IP_ADAPTER_ADDRESSES const *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES const *>(buffer.data());
                 adapter != nullptr; adapter = adapter->Next)
            {
                if (adapter->OperStatus != IfOperStatusUp || adapter->FirstGatewayAddress == nullptr ||
                    (adapter->IfType != IF_TYPE_ETHERNET_CSMACD && adapter->IfType != IF_TYPE_IEEE80211))
                {
                    continue;
                }
                if (adapter->Description != nullptr &&
                    std::wstring_view{adapter->Description}.contains(L"Apple Mobile Device"))
                {
                    continue;
                }
                if (chosen == nullptr || adapter->Ipv4Metric < chosen->Ipv4Metric)
                {
                    chosen = adapter;
                }
            }
            if (chosen == nullptr)
            {
                return;
            }
            facts.network_connected = true;
            facts.network_name = chosen->FriendlyName != nullptr ? chosen->FriendlyName : L"";
            if (chosen->Description != nullptr && chosen->Description[0] != L'\0')
            {
                if (!facts.network_name.empty())
                {
                    facts.network_name += L" · ";
                }
                facts.network_name += chosen->Description;
            }
            // (an adapter that does not know its rate reports the largest number)
            std::uint64_t const rate = std::min(chosen->TransmitLinkSpeed, chosen->ReceiveLinkSpeed);
            facts.network_bits_per_second = rate < 1'000'000'000'000ULL ? rate : 0;

            GUID adapter_id{};
            if (chosen->AdapterName == nullptr ||
                FAILED(CLSIDFromString(utf8_to_utf16(chosen->AdapterName).c_str(), &adapter_id)))
            {
                return;
            }
            ComPtr<INetworkListManager> manager;
            ComPtr<IEnumNetworkConnections> connections;
            if (FAILED(CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager))) ||
                FAILED(manager->GetNetworkConnections(&connections)))
            {
                return;
            }
            for (;;)
            {
                ComPtr<INetworkConnection> connection;
                ULONG fetched = 0;
                if (connections->Next(1, &connection, &fetched) != S_OK || fetched == 0)
                {
                    break;
                }
                GUID connected_adapter{};
                if (FAILED(connection->GetAdapterId(&connected_adapter)) ||
                    IsEqualGUID(connected_adapter, adapter_id) == FALSE)
                {
                    continue;
                }
                ComPtr<INetwork> network;
                NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PUBLIC;
                if (SUCCEEDED(connection->GetNetwork(&network)) && SUCCEEDED(network->GetCategory(&category)))
                {
                    facts.network = category == NLM_NETWORK_CATEGORY_PRIVATE                ? network_category::private_network
                                    : category == NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED ? network_category::domain_network
                                                                                             : network_category::public_network;
                }
                break;
            }
        }

        // Would Windows Firewall let a connection from the network reach this
        // program?  From its rules for this executable in the profiles that
        // are active now.  Another firewall product is not looked at.
        [[nodiscard]] firewall_verdict measure_firewall()
        {
            ComPtr<INetFwPolicy2> policy;
            long active = 0;
            if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy))) ||
                FAILED(policy->get_CurrentProfileTypes(&active)))
            {
                return firewall_verdict::unknown;
            }
            bool enabled = false;
            bool shut = false;
            for (NET_FW_PROFILE_TYPE2 const profile :
                 {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC})
            {
                VARIANT_BOOL on = VARIANT_FALSE;
                if ((active & profile) == 0 || FAILED(policy->get_FirewallEnabled(profile, &on)) || on == VARIANT_FALSE)
                {
                    continue;
                }
                enabled = true;
                VARIANT_BOOL block_all = VARIANT_FALSE;
                if (SUCCEEDED(policy->get_BlockAllInboundTraffic(profile, &block_all)) && block_all != VARIANT_FALSE)
                {
                    shut = true;
                }
            }
            if (!enabled)
            {
                return firewall_verdict::off;
            }
            if (shut)
            {
                return firewall_verdict::blocked;
            }
            std::wstring const program = executable_path();
            ComPtr<INetFwRules> rules;
            ComPtr<IUnknown> enumeration;
            ComPtr<IEnumVARIANT> enumerator;
            if (program.empty() || FAILED(policy->get_Rules(&rules)) || FAILED(rules->get__NewEnum(&enumeration)) ||
                FAILED(enumeration.As(&enumerator)))
            {
                return firewall_verdict::unknown;
            }
            bool allowed = false;
            bool blocked = false;
            for (;;)
            {
                VARIANT item;
                VariantInit(&item);
                ULONG fetched = 0;
                if (enumerator->Next(1, &item, &fetched) != S_OK || fetched == 0)
                {
                    break;
                }
                ComPtr<INetFwRule> rule;
                if (item.vt == VT_DISPATCH && item.pdispVal != nullptr)
                {
                    item.pdispVal->QueryInterface(IID_PPV_ARGS(&rule));
                }
                VariantClear(&item);
                VARIANT_BOOL rule_enabled = VARIANT_FALSE;
                NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_OUT;
                long profiles = 0;
                BSTR application = nullptr;
                if (!rule || FAILED(rule->get_Enabled(&rule_enabled)) || rule_enabled == VARIANT_FALSE ||
                    FAILED(rule->get_Direction(&direction)) || direction != NET_FW_RULE_DIR_IN ||
                    FAILED(rule->get_Profiles(&profiles)) || (profiles & active) == 0 ||
                    FAILED(rule->get_ApplicationName(&application)) || application == nullptr)
                {
                    continue;
                }
                // (a rule may name its program with %ProgramFiles% and the like)
                wchar_t expanded[1024]{};
                DWORD const length = ExpandEnvironmentStringsW(application, expanded, 1024);
                SysFreeString(application);
                if (length == 0 || length > 1024 ||
                    CompareStringOrdinal(expanded, -1, program.c_str(), -1, TRUE) != CSTR_EQUAL)
                {
                    continue;
                }
                NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
                if (SUCCEEDED(rule->get_Action(&action)))
                {
                    (action == NET_FW_ACTION_ALLOW ? allowed : blocked) = true;
                }
            }
            // a rule that blocks wins over one that allows
            return blocked ? firewall_verdict::blocked : allowed ? firewall_verdict::allowed : firewall_verdict::no_rule;
        }

        // ---- hardware -----------------------------------------------------------

        void measure_processor(system_facts &facts)
        {
            facts.processor_name = registry_text(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                                 L"ProcessorNameString");
            facts.processor_threads = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
            DWORD length = 0;
            GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
            std::vector<std::byte> buffer(length);
            if (length == 0 ||
                GetLogicalProcessorInformationEx(
                    RelationProcessorCore,
                    reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data()), &length) == FALSE)
            {
                return;
            }
            for (DWORD offset = 0; offset < length;)
            {
                auto const *const record =
                    reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX const *>(buffer.data() + offset);
                if (record->Size == 0)
                {
                    break;
                }
                ++facts.processor_cores;
                offset += record->Size;
            }
        }

        void measure_memory(system_facts &facts)
        {
            MEMORYSTATUSEX memory{};
            memory.dwLength = sizeof(memory);
            if (GlobalMemoryStatusEx(&memory) != FALSE)
            {
                facts.memory_total = memory.ullTotalPhys;
                facts.memory_available = memory.ullAvailPhys;
            }
            // what is installed (a little of it is reserved for the hardware)
            if (ULONGLONG installed_kilobytes = 0; GetPhysicallyInstalledSystemMemory(&installed_kilobytes) != FALSE)
            {
                facts.memory_total = installed_kilobytes * 1024;
            }
        }

        // device instance identifiers (they may contain serial numbers: never shown, never logged)
        [[nodiscard]] std::vector<std::wstring> device_identifiers(wchar_t const *filter, ULONG flags)
        {
            std::vector<std::wstring> identifiers;
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                ULONG length = 0;
                if (CM_Get_Device_ID_List_SizeW(&length, filter, flags) != CR_SUCCESS || length == 0)
                {
                    return identifiers;
                }
                std::vector<wchar_t> list(length);
                CONFIGRET const result = CM_Get_Device_ID_ListW(filter, list.data(), length, flags);
                if (result == CR_BUFFER_SMALL)
                {
                    continue; // a device arrived in between
                }
                if (result != CR_SUCCESS)
                {
                    return identifiers;
                }
                for (wchar_t const *entry = list.data(); *entry != L'\0'; entry += std::wcslen(entry) + 1)
                {
                    identifiers.emplace_back(entry);
                }
                return identifiers;
            }
            return identifiers;
        }

        [[nodiscard]] std::wstring device_text(DEVINST node, DEVPROPKEY const &key)
        {
            DEVPROPTYPE type = 0;
            wchar_t buffer[512]{};
            ULONG size = sizeof(buffer) - sizeof(wchar_t);
            if (CM_Get_DevNode_PropertyW(node, &key, &type, reinterpret_cast<PBYTE>(buffer), &size, 0) != CR_SUCCESS ||
                type != DEVPROP_TYPE_STRING)
            {
                return {};
            }
            return trimmed(buffer);
        }

        [[nodiscard]] std::wstring device_date(DEVINST node, DEVPROPKEY const &key)
        {
            DEVPROPTYPE type = 0;
            FILETIME time{};
            ULONG size = sizeof(time);
            SYSTEMTIME day{};
            if (CM_Get_DevNode_PropertyW(node, &key, &type, reinterpret_cast<PBYTE>(&time), &size, 0) != CR_SUCCESS ||
                type != DEVPROP_TYPE_FILETIME || FileTimeToSystemTime(&time, &day) == FALSE)
            {
                return {};
            }
            return std::format(L"{:04}-{:02}-{:02}", day.wYear, day.wMonth, day.wDay);
        }

        // "31.0.101.2125" as numbers, for comparing
        [[nodiscard]] std::vector<unsigned long> version_numbers(std::wstring_view version)
        {
            std::vector<unsigned long> numbers;
            unsigned long current = 0;
            bool digits = false;
            for (wchar_t const character : version)
            {
                if (character >= L'0' && character <= L'9')
                {
                    current = current * 10 + static_cast<unsigned long>(character - L'0');
                    digits = true;
                }
                else
                {
                    numbers.push_back(current);
                    current = 0;
                    digits = false;
                }
            }
            if (digits)
            {
                numbers.push_back(current);
            }
            return numbers;
        }

        void measure_graphics(system_facts &facts)
        {
            // the display adapters Windows has a driver for, by hardware identifier
            struct display_driver
            {
                std::wstring identifier; // upper case
                std::wstring version;
                std::wstring date;
            };
            std::vector<display_driver> drivers;
            for (std::wstring const &identifier :
                 device_identifiers(display_class, CM_GETIDLIST_FILTER_CLASS | CM_GETIDLIST_FILTER_PRESENT))
            {
                DEVINST node = 0;
                std::wstring writable = identifier;
                if (CM_Locate_DevNodeW(&node, writable.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
                {
                    continue;
                }
                drivers.push_back(display_driver{upper_ascii(identifier), device_text(node, key_driver_version),
                                                 device_date(node, key_driver_date)});
            }

            ComPtr<IDXGIFactory1> factory;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            {
                return;
            }
            bool in_use_found = false;
            for (UINT index = 0;; ++index)
            {
                ComPtr<IDXGIAdapter1> adapter;
                if (factory->EnumAdapters1(index, &adapter) != S_OK)
                {
                    break;
                }
                DXGI_ADAPTER_DESC1 description{};
                if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                {
                    continue; // Windows' software renderer is named below when it is the one in use
                }
                graphics_adapter_facts card;
                card.name = trimmed(description.Description);
                card.dedicated_memory = description.DedicatedVideoMemory;
                if (!facts.program.software_renderer && !in_use_found && card.name == facts.program.adapter_name)
                {
                    card.in_use = true;
                    in_use_found = true;
                }
                std::wstring const hardware =
                    std::format(L"VEN_{:04X}&DEV_{:04X}", description.VendorId, description.DeviceId);
                if (auto const driver = std::ranges::find_if(drivers,
                                                             [&hardware](display_driver const &candidate)
                                                             {
                                                                 return candidate.identifier.contains(hardware);
                                                             });
                    driver != drivers.end())
                {
                    card.driver_version = driver->version;
                    card.driver_date = driver->date;
                }
                // (hybrid graphics lists an adapter once per output path: once is enough here)
                if (std::ranges::none_of(facts.adapters, [&card](graphics_adapter_facts const &known)
                    {
                        return known.name == card.name;
                    }))
                {
                    facts.adapters.push_back(std::move(card));
                }
            }
            if (facts.program.software_renderer)
            {
                graphics_adapter_facts renderer;
                renderer.name = L"Microsoft Basic Render Driver";
                renderer.in_use = true;
                renderer.software = true;
                facts.adapters.insert(facts.adapters.begin(), std::move(renderer));
            }
            std::ranges::stable_partition(facts.adapters, &graphics_adapter_facts::in_use);
        }

        void measure_sound(system_facts &facts)
        {
            ComPtr<IMMDeviceEnumerator> enumerator;
            ComPtr<IMMDevice> device;
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
                FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
            {
                return;
            }
            facts.sound_device = true;
            if (ComPtr<IPropertyStore> properties; SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)))
            {
                PROPVARIANT name;
                PropVariantInit(&name);
                if (SUCCEEDED(properties->GetValue(key_endpoint_name, &name)) && name.vt == VT_LPWSTR &&
                    name.pwszVal != nullptr)
                {
                    facts.sound_name = name.pwszVal;
                }
                PropVariantClear(&name);
            }
            ComPtr<IAudioClient> client;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void **>(client.GetAddressOf()))))
            {
                WAVEFORMATEX *format = nullptr;
                if (SUCCEEDED(client->GetMixFormat(&format)) && format != nullptr)
                {
                    facts.sound_rate = static_cast<int>(format->nSamplesPerSec);
                    CoTaskMemFree(format);
                }
            }
        }

        // ---- Apple's USB support ------------------------------------------------

        void measure_apple_support(system_facts &facts)
        {
            constexpr wchar_t key[] = L"SOFTWARE\\Apple Inc.\\Apple Mobile Device Support";
            facts.apple_support_version = registry_text(HKEY_LOCAL_MACHINE, key, L"Version");
            if (facts.apple_support_version.empty())
            {
                facts.apple_support_version = registry_text(HKEY_LOCAL_MACHINE, key, L"Version", RRF_SUBKEY_WOW6432KEY);
            }
            facts.apple_support_installed = !facts.apple_support_version.empty();
            // the service of that package, or of the "Apple Devices" app
            facts.apple_service_reachable = usbmux_devices().has_value();

            // Apple's drivers of the devices that are or were attached (also
            // the ones that are not there now: their records stay)
            std::vector<unsigned long> usb_version;
            std::vector<unsigned long> network_version;
            for (std::wstring const &identifier : device_identifiers(L"USB", CM_GETIDLIST_FILTER_ENUMERATOR))
            {
                if (!upper_ascii(identifier).starts_with(L"USB\\VID_05AC"))
                {
                    continue;
                }
                DEVINST node = 0;
                std::wstring writable = identifier;
                if (CM_Locate_DevNodeW(&node, writable.data(), CM_LOCATE_DEVNODE_PHANTOM) != CR_SUCCESS ||
                    !device_text(node, key_driver_provider).contains(L"Apple"))
                {
                    continue;
                }
                std::wstring const description = device_text(node, key_device_description);
                std::wstring const version = device_text(node, key_driver_version);
                if (description.empty() || version.empty())
                {
                    continue;
                }
                bool const network = description.contains(L"Ethernet");
                std::vector<unsigned long> &newest = network ? network_version : usb_version;
                std::vector<unsigned long> numbers = version_numbers(version);
                if (!newest.empty() && !std::ranges::lexicographical_compare(newest, numbers))
                {
                    continue;
                }
                newest = std::move(numbers);
                std::wstring const date = device_date(node, key_driver_date);
                (network ? facts.apple_network_driver : facts.apple_usb_driver) =
                    date.empty() ? std::format(L"{} {}", description, version)
                                 : std::format(L"{} {} ({})", description, version, date);
            }
        }

        // ---- wording ------------------------------------------------------------

        struct wording
        {
            ui_language language = ui_language::english;

            [[nodiscard]] std::wstring text(ui_text id) const
            {
                return std::wstring{ui_string(id, language)};
            }

            [[nodiscard]] std::wstring format(ui_text id, std::initializer_list<std::wstring_view> arguments) const
            {
                return ui_format(id, language,
                                 std::span<std::wstring_view const>{arguments.begin(), arguments.size()});
            }
        };

        void join(std::wstring &line, std::wstring_view part)
        {
            if (part.empty())
            {
                return;
            }
            if (!line.empty())
            {
                line += L" · ";
            }
            line += part;
        }

        [[nodiscard]] std::wstring rate_text(std::uint64_t bits_per_second)
        {
            if (bits_per_second >= 1'000'000'000)
            {
                double const gigabits = static_cast<double>(bits_per_second) / 1e9;
                return gigabits == std::floor(gigabits) ? std::format(L"{:.0f} Gbps", gigabits)
                                                        : std::format(L"{:.1f} Gbps", gigabits);
            }
            return std::format(L"{:.0f} Mbps", static_cast<double>(bits_per_second) / 1e6);
        }

        [[nodiscard]] double gibibytes(std::uint64_t bytes)
        {
            return static_cast<double>(bytes) / static_cast<double>(gibibyte);
        }

        // "27.2.1" is 27
        [[nodiscard]] int leading_number(std::wstring_view text)
        {
            int number = 0;
            for (wchar_t const character : text)
            {
                if (character < L'0' || character > L'9' || number > 100'000)
                {
                    break;
                }
                number = number * 10 + (character - L'0');
            }
            return number;
        }

        struct report_builder
        {
            wording words;
            system_report report;

            void add(report_group group, ui_text label, std::wstring value, report_rating rating = report_rating::none,
                     unsigned int weight = 0, std::optional<ui_text> advice = std::nullopt)
            {
                report_entry entry;
                entry.group = group;
                entry.label = label;
                entry.value = std::move(value);
                entry.rating = rating;
                entry.weight = rating == report_rating::none ? 0 : weight;
                // what to do is said about a row that is not in order
                if (advice && (rating == report_rating::attention || rating == report_rating::problem))
                {
                    entry.advice = words.text(*advice);
                }
                report.entries.push_back(std::move(entry));
            }
        };

        void score(system_report &report)
        {
            // in half weights: good counts twice, attention once
            std::array<unsigned int, group_count> reached{};
            std::array<unsigned int, group_count> possible{};
            for (report_entry const &entry : report.entries)
            {
                if (entry.rating == report_rating::none || entry.weight == 0)
                {
                    continue;
                }
                auto const group = static_cast<std::size_t>(entry.group);
                possible[group] += 2 * entry.weight;
                reached[group] += (entry.rating == report_rating::good ? 2U : entry.rating == report_rating::attention ? 1U : 0U) *
                                  entry.weight;
            }
            unsigned int all_reached = 0;
            unsigned int all_possible = 0;
            for (std::size_t group = 0; group < group_count; ++group)
            {
                all_reached += reached[group];
                all_possible += possible[group];
                if (possible[group] != 0)
                {
                    report.group_scores[group] =
                        static_cast<int>(std::lround(100.0 * reached[group] / possible[group]));
                }
            }
            if (all_possible != 0)
            {
                report.score = static_cast<int>(std::lround(100.0 * all_reached / all_possible));
            }
        }

        // The iPhone is asked on a thread of its own: the end of the program
        // does not wait for a phone that does not answer (the collector stops
        // waiting and leaves that thread to end alone; it uses nothing but
        // this block and logs nothing).
        struct phone_question
        {
            std::mutex mutex;
            std::condition_variable_any finished;
            std::optional<system_facts> answer; // guarded by mutex
        };

        // nothing when the stop was requested first
        [[nodiscard]] std::optional<system_facts> ask_phone(system_facts const &facts, std::stop_token stop)
        {
            auto const shared = std::make_shared<phone_question>();
            try
            {
                std::thread{[shared, asked = facts]() mutable
                {
                    measure_phone(asked);
                    {
                        std::scoped_lock lock{shared->mutex};
                        shared->answer = std::move(asked);
                    }
                    shared->finished.notify_all();
                }}.detach();
            }
            catch (std::system_error const &cause)
            {
                // no thread to ask with: the iPhone stays unasked, the rest of the facts stands
                log::warning("this PC: the iPhone was not asked: {}",
                             make_failure("thread for the question", cause.code().value(), cause.what()).describe());
                system_facts unasked = facts;
                unasked.phone = phone_condition::absent;
                return unasked;
            }
            std::unique_lock lock{shared->mutex};
            if (!shared->finished.wait(lock, stop, [&shared]
                {
                    return shared->answer.has_value();
                }))
            {
                return std::nullopt;
            }
            return std::move(shared->answer);
        }
    }

    system_facts measure_system(program_facts program)
    {
        com_scope const com;
        system_facts facts;
        facts.program = std::move(program);
        measure_windows(facts);
        measure_display(facts);
        measure_power(facts);
        measure_network(facts);
        facts.firewall = measure_firewall();
        measure_processor(facts);
        measure_memory(facts);
        measure_graphics(facts);
        measure_sound(facts);
        measure_apple_support(facts);
        return facts;
    }

    void measure_phone(system_facts &facts)
    {
        facts.phone = phone_condition::absent;
        outcome<std::vector<usbmux_device>> const devices = usbmux_devices();
        facts.apple_service_reachable = devices.has_value();
        if (!devices)
        {
            return;
        }
        auto const on_cable = std::ranges::find_if(*devices, &usbmux_device::over_cable);
        if (on_cable == devices->end())
        {
            return;
        }
        outcome<std::unique_ptr<lockdown_session>> session = lockdown_session::open(*on_cable);
        if (!session)
        {
            facts.phone = pairing_refused(session.error()) ? phone_condition::not_trusted : phone_condition::unreachable;
            return;
        }
        facts.phone = phone_condition::present;
        auto const general = [&session](std::string_view key)
        {
            outcome<property_list> const reply = (*session)->value({}, key);
            return reply ? utf8_to_utf16(reply->root().item("Value").text().value_or(std::string{})) : std::wstring{};
        };
        facts.phone_model = general("ProductType");
        facts.phone_system = general("ProductVersion");
        if (outcome<property_list> const mode = (*session)->value(developer_mode_domain, "DeveloperModeStatus"); mode)
        {
            facts.developer_mode = mode->root().item("Value").flag();
        }
    }

    system_report build_report(system_facts const &facts, ui_language language)
    {
        report_builder builder;
        builder.words.language = language;
        wording const &words = builder.words;
        program_facts const &program = facts.program;
        using enum report_rating;

        // ---- system
        if (!facts.windows_name.empty())
        {
            std::wstring value = facts.windows_name;
            if (facts.windows_build != 0)
            {
                value += std::format(L" ({}.{})", facts.windows_build, facts.windows_revision);
            }
            bool const current = facts.windows_build >= oldest_good_windows_build;
            builder.add(report_group::system, ui_text::row_windows, std::move(value),
                        facts.windows_build == 0 ? none : current ? good : attention, weight_windows,
                        ui_text::advice_windows);
        }
        if (facts.display_width > 0 && facts.display_height > 0 && !facts.displays.empty())
        {
            std::wstring value =
                words.format(ui_text::value_display,
                             {std::to_wstring(facts.display_width), std::to_wstring(facts.display_height),
                              std::to_wstring(facts.display_scale_percent > 0 ? facts.display_scale_percent : 100)});
            double slowest = facts.displays.front().hz;
            double fastest = slowest;
            for (display_facts const &monitor : facts.displays)
            {
                std::wstring const name =
                    !monitor.name.empty() ? monitor.name
                                          : words.text(monitor.built_in ? ui_text::value_built_in_display
                                                                        : ui_text::value_external_display);
                join(value, std::format(L"{} {:.0f} Hz", name, monitor.hz));
                slowest = std::min(slowest, monitor.hz);
                fastest = std::max(fastest, monitor.hz);
            }
            // The desktop duplicated on monitors that refresh at different
            // rates: Windows paces it to one of them, the other one judders.
            if (fastest - slowest > uneven_refresh_hz)
            {
                builder.add(report_group::system, ui_text::row_display, std::move(value), attention, weight_display,
                            ui_text::advice_display_duplicate);
            }
            else
            {
                builder.add(report_group::system, ui_text::row_display, std::move(value),
                            slowest >= slowest_good_display_hz ? good : attention, weight_display,
                            ui_text::advice_display);
            }
        }
        {
            std::wstring const percent = facts.battery_percent >= 0 ? std::to_wstring(facts.battery_percent) : L"?";
            if (!facts.battery_present)
            {
                builder.add(report_group::system, ui_text::row_power, words.text(ui_text::value_no_battery), good,
                            weight_power);
            }
            else if (!facts.on_battery)
            {
                builder.add(report_group::system, ui_text::row_power, words.text(ui_text::value_plugged_in), good,
                            weight_power);
            }
            else if (facts.energy_saver)
            {
                builder.add(report_group::system, ui_text::row_power, words.format(ui_text::value_energy_saver, {percent}),
                            attention, weight_power, ui_text::advice_energy_saver);
            }
            else
            {
                builder.add(report_group::system, ui_text::row_power, words.format(ui_text::value_on_battery, {percent}),
                            good, weight_power);
            }
        }
        if (!facts.network_connected)
        {
            builder.add(report_group::system, ui_text::row_network, words.text(ui_text::value_not_connected), attention,
                        weight_network, ui_text::advice_no_network);
        }
        else
        {
            std::wstring value = facts.network_name;
            if (facts.network_bits_per_second != 0)
            {
                join(value, rate_text(facts.network_bits_per_second));
            }
            if (facts.network != network_category::unknown)
            {
                join(value, words.text(facts.network == network_category::private_network  ? ui_text::value_private_network
                                       : facts.network == network_category::domain_network ? ui_text::value_domain_network
                                                                                           : ui_text::value_public_network));
            }
            if (facts.metered.value_or(false))
            {
                join(value, words.text(ui_text::value_metered));
            }
            bool const slow = facts.network_bits_per_second != 0 &&
                              facts.network_bits_per_second < slowest_good_network_bits;
            if (facts.network == network_category::public_network)
            {
                builder.add(report_group::system, ui_text::row_network, std::move(value), attention, weight_network,
                            ui_text::advice_public_network);
            }
            else
            {
                builder.add(report_group::system, ui_text::row_network, std::move(value), slow ? attention : good,
                            weight_network, ui_text::advice_slow_network);
            }
        }
        switch (facts.firewall)
        {
        case firewall_verdict::off:
            builder.add(report_group::system, ui_text::row_firewall, words.text(ui_text::value_firewall_off), good,
                        weight_firewall);
            break;
        case firewall_verdict::allowed:
            builder.add(report_group::system, ui_text::row_firewall, words.text(ui_text::value_firewall_allows), good,
                        weight_firewall);
            break;
        case firewall_verdict::blocked:
            builder.add(report_group::system, ui_text::row_firewall, words.text(ui_text::value_firewall_blocks), problem,
                        weight_firewall, ui_text::advice_firewall);
            break;
        case firewall_verdict::no_rule:
            builder.add(report_group::system, ui_text::row_firewall, words.text(ui_text::value_firewall_no_rule),
                        attention, weight_firewall, ui_text::advice_firewall);
            break;
        case firewall_verdict::unknown:
            break;
        }

        // ---- hardware
        if (facts.processor_threads != 0)
        {
            std::wstring value = facts.processor_name;
            join(value, words.format(ui_text::value_processor,
                                     {std::to_wstring(std::max(facts.processor_cores, 1U)),
                                      std::to_wstring(facts.processor_threads)}));
            builder.add(report_group::hardware, ui_text::row_processor, std::move(value),
                        facts.processor_threads >= fewest_good_threads ? good
                        : facts.processor_threads >= 2                 ? attention
                                                                       : problem,
                        weight_processor, ui_text::advice_processor);
        }
        if (facts.memory_total != 0)
        {
            double const total = gibibytes(facts.memory_total);
            builder.add(report_group::hardware, ui_text::row_memory,
                        words.format(ui_text::value_memory, {std::format(L"{:.0f}", total),
                                                             std::format(L"{:.1f}", gibibytes(facts.memory_available))}),
                        total >= least_good_memory_gib ? good : total >= least_usable_memory_gib ? attention : problem,
                        weight_memory, ui_text::advice_memory);
        }
        for (graphics_adapter_facts const &card : facts.adapters)
        {
            std::wstring value = card.name;
            if (card.dedicated_memory >= gibibyte / 2)
            {
                join(value, std::format(L"{:.0f} GB", std::max(1.0, std::round(gibibytes(card.dedicated_memory)))));
            }
            if (card.in_use)
            {
                join(value, words.text(ui_text::value_in_use));
                builder.add(report_group::hardware, ui_text::row_graphics, std::move(value),
                            card.software ? problem : good, weight_graphics, ui_text::advice_software_renderer);
            }
            else
            {
                builder.add(report_group::hardware, ui_text::row_other_graphics, std::move(value));
            }
        }
        {
            std::wstring const hardware = words.text(ui_text::value_hardware);
            std::wstring const software = words.text(ui_text::value_software);
            builder.add(report_group::hardware, ui_text::row_decoding,
                        words.format(ui_text::value_decoding, {program.hevc_hardware ? hardware : software,
                                                               program.h264_hardware ? hardware : software}),
                        program.hevc_hardware ? good : program.h264_hardware ? attention : problem, weight_decoding,
                        program.h264_hardware ? ui_text::advice_h264_only : ui_text::advice_no_hardware_decoding);
        }
        if (facts.sound_device)
        {
            std::wstring value = facts.sound_name;
            if (facts.sound_rate > 0)
            {
                join(value, facts.sound_rate % 1000 == 0 ? std::format(L"{} kHz", facts.sound_rate / 1000)
                                                         : std::format(L"{:.1f} kHz", facts.sound_rate / 1000.0));
            }
            builder.add(report_group::hardware, ui_text::row_sound, std::move(value), good, weight_sound);
        }
        else
        {
            builder.add(report_group::hardware, ui_text::row_sound, words.text(ui_text::value_none), problem,
                        weight_sound, ui_text::advice_no_sound);
        }

        // ---- drivers
        for (graphics_adapter_facts const &card : facts.adapters)
        {
            if (card.software || card.driver_version.empty())
            {
                continue;
            }
            std::wstring value = card.name;
            join(value, card.driver_version);
            join(value, card.driver_date);
            builder.add(report_group::drivers, ui_text::row_graphics_driver, std::move(value),
                        card.in_use ? good : none, weight_graphics_driver);
        }
        if (facts.apple_support_installed || facts.apple_service_reachable)
        {
            std::wstring value = facts.apple_support_version;
            join(value, words.text(facts.apple_service_reachable ? ui_text::value_service_running
                                                                 : ui_text::value_service_stopped));
            builder.add(report_group::drivers, ui_text::row_apple_support, std::move(value),
                        facts.apple_service_reachable ? good : attention, weight_apple_support,
                        ui_text::advice_apple_service);
        }
        else
        {
            builder.add(report_group::drivers, ui_text::row_apple_support, words.text(ui_text::value_not_installed),
                        attention, weight_apple_support, ui_text::advice_apple_support);
        }
        if (!facts.apple_usb_driver.empty())
        {
            builder.add(report_group::drivers, ui_text::row_apple_usb_driver, facts.apple_usb_driver);
        }
        if (!facts.apple_network_driver.empty())
        {
            builder.add(report_group::drivers, ui_text::row_apple_network_driver, facts.apple_network_driver);
        }
        if (program.usb_link)
        {
            std::wstring value = words.text(*program.usb_link);
            if (!program.usb_link_address.empty())
            {
                value += L' ';
                value += program.usb_link_address;
            }
            builder.add(report_group::drivers, ui_text::row_usb_link, std::move(value),
                        program.usb_adapter_disabled ? attention : none, weight_usb_link, ui_text::advice_usb_adapter);
        }

        // ---- software
        {
            std::wstring value = std::format(L"" UXPLAY_PRODUCT_NAME L" {}", program.version);
            switch (program.update)
            {
            case update_knowledge::up_to_date:
                join(value, words.text(ui_text::value_up_to_date));
                builder.add(report_group::software, ui_text::row_program, std::move(value), good, weight_program);
                break;
            case update_knowledge::newer_available:
                join(value, words.format(ui_text::value_newer, {program.newest_version}));
                builder.add(report_group::software, ui_text::row_program, std::move(value), attention, weight_program,
                            ui_text::advice_update);
                break;
            case update_knowledge::unknown:
                builder.add(report_group::software, ui_text::row_program, std::move(value));
                break;
            }
        }
        if (program.stream_active)
        {
            std::wstring value = program.stream_link;
            join(value, program.stream_codec);
            if (program.stream_width > 0 && program.stream_height > 0)
            {
                join(value, std::format(L"{} × {}", program.stream_width, program.stream_height));
            }
            join(value, words.text(program.stream_hardware_decoding ? ui_text::value_hardware_decoding
                                                                    : ui_text::value_software_decoding));
            builder.add(report_group::software, ui_text::row_stream, std::move(value));
        }
        // The iPhone does not count in the score (it is no part of this PC);
        // what control still needs of it is said beside it.
        switch (facts.phone)
        {
        case phone_condition::not_asked:
            builder.add(report_group::software, ui_text::row_iphone, words.text(ui_text::value_checking));
            break;
        case phone_condition::absent:
        case phone_condition::unreachable:
            builder.add(report_group::software, ui_text::row_iphone, words.text(ui_text::value_no_iphone));
            break;
        case phone_condition::not_trusted:
        {
            builder.add(report_group::software, ui_text::row_iphone, words.text(ui_text::value_not_trusted));
            builder.report.entries.back().advice = words.text(ui_text::control_needs_cable);
            break;
        }
        case phone_condition::present:
        {
            std::wstring value = facts.phone_model;
            if (!facts.phone_system.empty())
            {
                join(value, std::format(L"iOS {}", facts.phone_system));
            }
            if (facts.developer_mode)
            {
                join(value, words.text(*facts.developer_mode ? ui_text::value_developer_mode_on
                                                             : ui_text::value_developer_mode_off));
            }
            builder.add(report_group::software, ui_text::row_iphone, std::move(value));
            if (int const major = leading_number(facts.phone_system); major != 0 && major < first_ios_with_control)
            {
                builder.report.entries.back().advice = words.text(ui_text::control_needs_ios);
            }
            else if (facts.developer_mode && !*facts.developer_mode)
            {
                builder.report.entries.back().advice = words.text(ui_text::control_needs_developer_mode);
            }
            break;
        }
        }

        score(builder.report);
        return std::move(builder.report);
    }

    std::wstring report_text(system_facts const &facts)
    {
        constexpr ui_language language = ui_language::english;
        system_report const report = build_report(facts, language);
        std::wstring text = std::format(L"" UXPLAY_PRODUCT_NAME L" {} - this PC\r\n", facts.program.version);
        if (report.score)
        {
            text += std::format(L"Score: {} of 100\r\n", *report.score);
        }
        std::optional<report_group> shown;
        for (report_entry const &entry : report.entries)
        {
            if (shown != entry.group)
            {
                shown = entry.group;
                constexpr std::array group_names{ui_text::group_system, ui_text::group_hardware, ui_text::group_drivers,
                                                 ui_text::group_software};
                auto const group = static_cast<std::size_t>(entry.group);
                text += std::format(L"\r\n{}", ui_string(group_names[group], language));
                if (report.group_scores[group])
                {
                    text += std::format(L" ({} of 100)", *report.group_scores[group]);
                }
                text += L"\r\n";
            }
            wchar_t const *const mark = entry.rating == report_rating::good        ? L"[good]     "
                                        : entry.rating == report_rating::attention ? L"[attention]"
                                        : entry.rating == report_rating::problem   ? L"[problem]  "
                                                                                   : L"[info]     ";
            text += std::format(L"  {} {}: {}\r\n", mark, ui_string(entry.label, language), entry.value);
            if (!entry.advice.empty())
            {
                text += std::format(L"              -> {}\r\n", entry.advice);
            }
        }
        return text;
    }

    struct report_collector::state
    {
        std::function<void(system_facts const &facts)> on_facts;
        std::mutex mutex;
        std::condition_variable_any wake;
        std::optional<program_facts> wanted; // guarded by mutex: the request not yet taken
        std::jthread worker;                 // last member: stopped and joined before the others go

        [[nodiscard]] bool superseded()
        {
            std::scoped_lock lock{mutex};
            return wanted.has_value();
        }

        void run(std::stop_token stop)
        {
            for (;;)
            {
                program_facts program;
                {
                    std::unique_lock lock{mutex};
                    if (!wake.wait(lock, stop, [this]
                        {
                            return wanted.has_value();
                        }))
                    {
                        return;
                    }
                    program = std::move(*wanted);
                    wanted.reset();
                }
                system_facts const facts = measure_system(std::move(program));
                if (stop.stop_requested())
                {
                    return;
                }
                if (superseded())
                {
                    continue;
                }
                on_facts(facts);
                std::optional<system_facts> const with_phone = ask_phone(facts, stop);
                if (!with_phone)
                {
                    return; // the program ends
                }
                if (!superseded())
                {
                    on_facts(*with_phone);
                }
            }
        }
    };

    report_collector::report_collector(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    report_collector::~report_collector() = default;

    std::unique_ptr<report_collector> report_collector::create(std::function<void(system_facts const &facts)> on_facts)
    {
        auto created = std::make_unique<state>();
        created->on_facts = std::move(on_facts);
        state *const shared = created.get();
        created->worker = std::jthread{[shared](std::stop_token stop)
        {
            shared->run(stop);
        }};
        return std::unique_ptr<report_collector>{new report_collector{std::move(created)}};
    }

    void report_collector::request(program_facts program)
    {
        {
            std::scoped_lock lock{impl->mutex};
            impl->wanted = std::move(program);
        }
        impl->wake.notify_all();
    }
}
