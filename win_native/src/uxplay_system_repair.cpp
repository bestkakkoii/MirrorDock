// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <netfw.h>

#include "uxplay_version.h"

module uxplay_system_repair;

import std;
import uxplay_status;
import uxplay_system_report;
import uxplay_elevation;

namespace uxplay
{
    namespace
    {
        // for the tool behind the consent prompt (the prompt itself is not timed)
        constexpr std::chrono::milliseconds tool_patience{30'000};
        // the firewall shows a new rule at once; a few looks in case it does not
        constexpr int rule_checks = 6;
        constexpr DWORD rule_check_ms = 200;
        constexpr DWORD service_already_running = 1056; // ERROR_SERVICE_ALREADY_RUNNING
        // comes with the program (win_native/package)
        constexpr wchar_t apple_installer_name[] = L"install_apple_usb_support.cmd";
        // "Apple Devices" in the Microsoft Store: it brings the same drivers
        constexpr wchar_t apple_store_page[] = L"ms-windows-store://pdp/?ProductId=9NP83LWLPZ9K";

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

        [[nodiscard]] std::wstring executable_path()
        {
            std::wstring path(32768, L'\0');
            DWORD const length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            path.resize(length < path.size() ? length : 0);
            return path;
        }

        // Opens a page of Windows (an address Windows itself handles) or a file.
        [[nodiscard]] repair_outcome open_target(HWND owner, std::wstring const &target, std::string_view name,
                                                 wchar_t const *directory = nullptr)
        {
            auto const opened = reinterpret_cast<std::intptr_t>(
                ShellExecuteW(owner, L"open", target.c_str(), nullptr, directory, SW_SHOWNORMAL));
            if (opened <= 32) // ShellExecute reports success with a value above 32
            {
                return repair_outcome{repair_result::failed,
                                      std::format("{} could not be opened (ShellExecute code {})", name, opened)};
            }
            return repair_outcome{repair_result::opened, std::format("{} opened", name)};
        }

        [[nodiscard]] repair_outcome after_tool(outcome<unsigned long> const &ran, std::string_view tool)
        {
            if (ran.error().code == static_cast<std::int64_t>(consent_declined))
            {
                return repair_outcome{repair_result::declined,
                                      std::format("{} was not run: administrator consent was declined", tool)};
            }
            return repair_outcome{repair_result::failed, std::format("{} was not run: {}", tool, ran.error().describe())};
        }

        // "domain,private,public" as netsh takes it
        [[nodiscard]] std::wstring profile_names(long profiles)
        {
            std::wstring names;
            auto const add = [&names, profiles](long bit, std::wstring_view name)
            {
                if ((profiles & bit) != 0)
                {
                    if (!names.empty())
                    {
                        names += L',';
                    }
                    names += name;
                }
            };
            add(NET_FW_PROFILE2_DOMAIN, L"domain");
            add(NET_FW_PROFILE2_PRIVATE, L"private");
            add(NET_FW_PROFILE2_PUBLIC, L"public");
            return names;
        }

        // Windows Firewall lets connections from the network reach this
        // program: in the kinds of network the PC is on now when it had no
        // word on the program yet, and wherever a rule kept it out before.
        [[nodiscard]] repair_outcome allow_through_firewall(HWND owner)
        {
            com_scope const com;
            firewall_facts const before = measure_firewall_rules();
            std::wstring const program = executable_path();
            if (program.empty() || program.contains(L'"'))
            {
                return repair_outcome{repair_result::failed, "the path of the program is not usable in a firewall rule"};
            }
            std::wstring parameters;
            switch (before.verdict)
            {
            case firewall_verdict::off:
            case firewall_verdict::allowed:
                return repair_outcome{repair_result::done, "the firewall keeps nothing from this program: nothing to do"};
            case firewall_verdict::shut:
                return repair_outcome{repair_result::failed,
                                      "the firewall blocks every incoming connection: a rule does not help"};
            case firewall_verdict::unknown:
                return repair_outcome{repair_result::failed, "the firewall's rules could not be read"};
            case firewall_verdict::blocked:
                // the rules that keep it out become rules that let it in
                parameters = std::format(
                    L"advfirewall firewall set rule name=all dir=in program=\"{}\" new action=allow enable=yes", program);
                break;
            case firewall_verdict::no_rule:
            {
                std::wstring const profiles = profile_names(before.active_profiles);
                if (profiles.empty())
                {
                    return repair_outcome{repair_result::failed, "Windows names no kind of network the PC is on"};
                }
                parameters = std::format(L"advfirewall firewall add rule name=\"" UXPLAY_PRODUCT_NAME
                                         L"\" dir=in action=allow program=\"{}\" enable=yes profile={}",
                                         program, profiles);
                break;
            }
            }
            if (parameters.empty())
            {
                return repair_outcome{repair_result::failed, "the state of the firewall is not one of those known"};
            }
            outcome<unsigned long> const ran = run_elevated(system_tool(L"netsh.exe"), parameters, owner, tool_patience);
            if (!ran)
            {
                return after_tool(ran, "netsh");
            }
            firewall_facts after = measure_firewall_rules();
            for (int check = 1; check < rule_checks && after.verdict != firewall_verdict::allowed &&
                                after.verdict != firewall_verdict::off;
                 ++check)
            {
                Sleep(rule_check_ms);
                after = measure_firewall_rules();
            }
            bool const lets_in = after.verdict == firewall_verdict::allowed || after.verdict == firewall_verdict::off;
            return repair_outcome{lets_in ? repair_result::done : repair_result::failed,
                                  std::format("netsh advfirewall ({} a rule) ended with exit code {}; the firewall {} "
                                              "connections to this program now",
                                              before.verdict == firewall_verdict::blocked ? "changing" : "adding", *ran,
                                              lets_in ? "lets in" : "still keeps out")};
        }

        [[nodiscard]] repair_outcome start_apple_service(HWND owner)
        {
            outcome<unsigned long> const ran =
                run_elevated(system_tool(L"sc.exe"), L"start \"Apple Mobile Device Service\"", owner, tool_patience);
            if (!ran)
            {
                return after_tool(ran, "sc");
            }
            bool const started = *ran == 0 || *ran == service_already_running;
            return repair_outcome{started ? repair_result::done : repair_result::failed,
                                  std::format("sc start \"Apple Mobile Device Service\" ended with exit code {}", *ran)};
        }

        // The installer that comes with the program (it asks winget for
        // Apple's driver package); without it, the page of "Apple Devices" in
        // the Microsoft Store.
        [[nodiscard]] repair_outcome install_apple_support(HWND owner)
        {
            std::filesystem::path const folder = std::filesystem::path{executable_path()}.parent_path();
            std::filesystem::path const installer = folder / apple_installer_name;
            std::error_code ignored;
            if (!folder.empty() && std::filesystem::is_regular_file(installer, ignored))
            {
                return open_target(owner, installer.wstring(), "the installer of Apple's USB support", folder.c_str());
            }
            return open_target(owner, apple_store_page, "the page of \"Apple Devices\" in the Microsoft Store");
        }
    }

    bool repairs(report_action action)
    {
        switch (action)
        {
        case report_action::allow_through_firewall:
        case report_action::open_firewall_settings:
        case report_action::open_network_settings:
        case report_action::open_display_settings:
        case report_action::open_power_settings:
        case report_action::open_sound_settings:
        case report_action::open_windows_update:
        case report_action::install_apple_support:
        case report_action::start_apple_service:
            return true;
        case report_action::none:
        case report_action::enable_usb_adapter:
        case report_action::install_update:
            break;
        }
        return false;
    }

    repair_outcome repair(report_action action, void *owner_window)
    {
        HWND const owner = static_cast<HWND>(owner_window);
        switch (action)
        {
        case report_action::allow_through_firewall:
            return allow_through_firewall(owner);
        case report_action::open_firewall_settings:
            return open_target(owner, L"windowsdefender://network/", "Windows Security (firewall and network protection)");
        case report_action::open_network_settings:
            return open_target(owner, L"ms-settings:network", "Settings (network)");
        case report_action::open_display_settings:
            return open_target(owner, L"ms-settings:display-advanced", "Settings (advanced display)");
        case report_action::open_power_settings:
            return open_target(owner, L"ms-settings:batterysaver", "Settings (power and battery)");
        case report_action::open_sound_settings:
            return open_target(owner, L"ms-settings:sound", "Settings (sound)");
        case report_action::open_windows_update:
            return open_target(owner, L"ms-settings:windowsupdate", "Settings (Windows Update)");
        case report_action::install_apple_support:
            return install_apple_support(owner);
        case report_action::start_apple_service:
            return start_apple_service(owner);
        case report_action::none:
        case report_action::enable_usb_adapter:
        case report_action::install_update:
            break;
        }
        return repair_outcome{repair_result::failed, "this repair belongs to another part of the program"};
    }
}
