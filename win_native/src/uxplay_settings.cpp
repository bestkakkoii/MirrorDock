// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "uxplay_version.h"

module uxplay_settings;

import std;
import uxplay_status;
import uxplay_log;

namespace uxplay::settings
{
    namespace
    {
        constexpr wchar_t settings_key[] = L"Software\\" UXPLAY_PRODUCT_NAME;
        constexpr wchar_t run_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        constexpr wchar_t run_value[] = L"" UXPLAY_PRODUCT_NAME;
        // the same under the name the program had up to 1.6
        constexpr wchar_t legacy_settings_key[] = L"Software\\" UXPLAY_LEGACY_PRODUCT_NAME;
        constexpr wchar_t legacy_run_value[] = L"" UXPLAY_LEGACY_PRODUCT_NAME;

        outcome<std::wstring> executable_path()
        {
            std::wstring path(512, L'\0');
            for (;;)
            {
                DWORD const length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
                if (length == 0)
                {
                    return std::unexpected(make_win32_failure("GetModuleFileNameW", GetLastError()));
                }
                if (length < path.size())
                {
                    path.resize(length);
                    return path;
                }
                if (path.size() >= 32768)
                {
                    return std::unexpected(make_win32_failure("GetModuleFileNameW", ERROR_INSUFFICIENT_BUFFER));
                }
                path.resize(path.size() * 2);
            }
        }
    }

    outcome<std::optional<std::uint32_t>> read_number(std::wstring_view name)
    {
        std::wstring const value_name{name};
        DWORD value = 0;
        DWORD size = sizeof(value);
        LSTATUS const result = RegGetValueW(HKEY_CURRENT_USER, settings_key, value_name.c_str(), RRF_RT_REG_DWORD,
                                            nullptr, &value, &size);
        if (result == ERROR_FILE_NOT_FOUND)
        {
            return std::optional<std::uint32_t>{};
        }
        if (result != ERROR_SUCCESS)
        {
            return std::unexpected(make_win32_failure(std::format("RegGetValueW({})", utf16_to_utf8(value_name)),
                                                      static_cast<unsigned long>(result)));
        }
        return std::optional<std::uint32_t>{value};
    }

    outcome<void> write_number(std::wstring_view name, std::uint32_t value)
    {
        std::wstring const value_name{name};
        DWORD const stored = value;
        LSTATUS const result = RegSetKeyValueW(HKEY_CURRENT_USER, settings_key, value_name.c_str(), REG_DWORD,
                                               &stored, sizeof(stored));
        if (result != ERROR_SUCCESS)
        {
            return std::unexpected(make_win32_failure(std::format("RegSetKeyValueW({})", utf16_to_utf8(value_name)),
                                                      static_cast<unsigned long>(result)));
        }
        return {};
    }

    std::optional<std::uint32_t> stored_number(std::wstring_view name)
    {
        auto value = read_number(name);
        if (!value)
        {
            log::warning("settings: {} (using the default)", value.error().describe());
            return std::nullopt;
        }
        return *value;
    }

    void store_number(std::wstring_view name, std::uint32_t value)
    {
        if (auto stored = write_number(name, value); !stored)
        {
            log::warning("settings: not saved: {}", stored.error().describe());
        }
    }

    outcome<std::string> adopt_legacy_settings()
    {
        std::string done;
        HKEY current = nullptr;
        LSTATUS result = RegOpenKeyExW(HKEY_CURRENT_USER, settings_key, 0, KEY_READ, &current);
        if (result == ERROR_SUCCESS)
        {
            RegCloseKey(current); // this program has settings of its own already
        }
        else if (result != ERROR_FILE_NOT_FOUND)
        {
            return std::unexpected(make_win32_failure("RegOpenKeyExW(settings)", static_cast<unsigned long>(result)));
        }
        else
        {
            HKEY legacy = nullptr;
            result = RegOpenKeyExW(HKEY_CURRENT_USER, legacy_settings_key, 0, KEY_READ, &legacy);
            if (result == ERROR_SUCCESS)
            {
                HKEY created = nullptr;
                result = RegCreateKeyExW(HKEY_CURRENT_USER, settings_key, 0, nullptr, 0, KEY_WRITE, nullptr, &created,
                                         nullptr);
                if (result == ERROR_SUCCESS)
                {
                    result = RegCopyTreeW(legacy, nullptr, created);
                    RegCloseKey(created);
                    if (result != ERROR_SUCCESS)
                    {
                        // no half-copied settings: the next start tries again
                        RegDeleteTreeW(HKEY_CURRENT_USER, settings_key);
                    }
                }
                RegCloseKey(legacy);
                if (result != ERROR_SUCCESS)
                {
                    return std::unexpected(make_win32_failure("copying the settings of the earlier program name",
                                                              static_cast<unsigned long>(result)));
                }
                done = "settings taken over from the earlier program name (HKCU\\Software\\" UXPLAY_LEGACY_PRODUCT_NAME
                       ", which is left as it is)";
            }
            else if (result != ERROR_FILE_NOT_FOUND)
            {
                return std::unexpected(
                    make_win32_failure("RegOpenKeyExW(earlier settings)", static_cast<unsigned long>(result)));
            }
        }

        // "start with Windows" registered under the earlier name: it would
        // start the earlier executable, so it becomes an entry for this one
        DWORD size = 0;
        if (RegGetValueW(HKEY_CURRENT_USER, run_key, legacy_run_value, RRF_RT_REG_SZ, nullptr, nullptr, &size) ==
            ERROR_SUCCESS)
        {
            if (auto registered = set_autostart(true); !registered)
            {
                return std::unexpected(registered.error());
            }
            result = RegDeleteKeyValueW(HKEY_CURRENT_USER, run_key, legacy_run_value);
            if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND)
            {
                return std::unexpected(
                    make_win32_failure("RegDeleteKeyValueW(earlier Run entry)", static_cast<unsigned long>(result)));
            }
            done += done.empty() ? "" : "; ";
            done += "\"start with Windows\" now starts this program instead of the earlier one";
        }
        return done;
    }

    outcome<std::optional<std::wstring>> autostart_command()
    {
        DWORD size = 0;
        LSTATUS result =
            RegGetValueW(HKEY_CURRENT_USER, run_key, run_value, RRF_RT_REG_SZ, nullptr, nullptr, &size);
        if (result == ERROR_FILE_NOT_FOUND)
        {
            return std::optional<std::wstring>{};
        }
        if (result != ERROR_SUCCESS)
        {
            return std::unexpected(make_win32_failure("RegGetValueW(Run, size)", static_cast<unsigned long>(result)));
        }
        std::wstring command(size / sizeof(wchar_t) + 1, L'\0');
        size = static_cast<DWORD>(command.size() * sizeof(wchar_t));
        result = RegGetValueW(HKEY_CURRENT_USER, run_key, run_value, RRF_RT_REG_SZ, nullptr, command.data(), &size);
        if (result != ERROR_SUCCESS)
        {
            return std::unexpected(make_win32_failure("RegGetValueW(Run)", static_cast<unsigned long>(result)));
        }
        command.resize(std::wcslen(command.c_str()));
        return std::optional<std::wstring>{std::move(command)};
    }

    outcome<void> set_autostart(bool enabled)
    {
        if (!enabled)
        {
            LSTATUS const result = RegDeleteKeyValueW(HKEY_CURRENT_USER, run_key, run_value);
            if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND)
            {
                return std::unexpected(
                    make_win32_failure("RegDeleteKeyValueW(Run)", static_cast<unsigned long>(result)));
            }
            return {};
        }
        auto path = executable_path();
        if (!path)
        {
            return std::unexpected(path.error());
        }
        std::wstring const command = L"\"" + *path + L"\"";
        LSTATUS const result =
            RegSetKeyValueW(HKEY_CURRENT_USER, run_key, run_value, REG_SZ, command.c_str(),
                            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        if (result != ERROR_SUCCESS)
        {
            return std::unexpected(make_win32_failure("RegSetKeyValueW(Run)", static_cast<unsigned long>(result)));
        }
        return {};
    }
}
