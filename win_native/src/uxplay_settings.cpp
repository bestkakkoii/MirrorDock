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
        // set in this program's key once the earlier key was looked at
        constexpr wchar_t legacy_adopted_value[] = L"legacy_settings_adopted";

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
        LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, settings_key, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr,
                                         &current, nullptr);
        if (result != ERROR_SUCCESS)
        {
            return std::unexpected(make_win32_failure("RegCreateKeyExW(settings)", static_cast<unsigned long>(result)));
        }
        DWORD adopted = 0;
        DWORD adopted_size = sizeof(adopted);
        bool const taken_over_before = RegGetValueW(current, nullptr, legacy_adopted_value, RRF_RT_REG_DWORD, nullptr,
                                                    &adopted, &adopted_size) == ERROR_SUCCESS;
        if (!taken_over_before)
        {
            // Every value the earlier key holds and this one lacks is copied;
            // a value the user has set since under the new name stays.  (Value
            // by value: RegCopyTreeW was refused with "access denied" for a
            // key opened with the rights this needs.)
            HKEY legacy = nullptr;
            result = RegOpenKeyExW(HKEY_CURRENT_USER, legacy_settings_key, 0, KEY_READ, &legacy);
            if (result == ERROR_SUCCESS)
            {
                int copied = 0;
                std::optional<failure> problem;
                for (DWORD index = 0; !problem; ++index)
                {
                    wchar_t name[256]{};
                    DWORD name_length = 256;
                    DWORD type = 0;
                    DWORD data = 0;
                    DWORD data_size = sizeof(data);
                    LSTATUS const enumerated = RegEnumValueW(legacy, index, name, &name_length, nullptr, &type,
                                                             reinterpret_cast<BYTE *>(&data), &data_size);
                    if (enumerated == ERROR_NO_MORE_ITEMS)
                    {
                        break;
                    }
                    if (enumerated == ERROR_MORE_DATA || (enumerated == ERROR_SUCCESS && type != REG_DWORD))
                    {
                        continue; // not one of this program's settings (all are numbers)
                    }
                    if (enumerated != ERROR_SUCCESS)
                    {
                        problem = make_win32_failure("RegEnumValueW(earlier settings)",
                                                     static_cast<unsigned long>(enumerated));
                        break;
                    }
                    DWORD present = 0;
                    DWORD present_size = sizeof(present);
                    if (RegGetValueW(current, nullptr, name, RRF_RT_REG_DWORD, nullptr, &present, &present_size) ==
                        ERROR_SUCCESS)
                    {
                        continue;
                    }
                    LSTATUS const stored = RegSetValueExW(current, name, 0, REG_DWORD,
                                                          reinterpret_cast<BYTE const *>(&data), sizeof(data));
                    if (stored != ERROR_SUCCESS)
                    {
                        problem = make_win32_failure("RegSetValueExW(setting taken over)",
                                                     static_cast<unsigned long>(stored));
                        break;
                    }
                    ++copied;
                }
                RegCloseKey(legacy);
                if (problem)
                {
                    RegCloseKey(current);
                    return std::unexpected(*problem); // not marked as done: the next start tries again
                }
                if (copied > 0)
                {
                    done = std::format("{} setting(s) taken over from the earlier program name (HKCU\\Software\\"
                                       UXPLAY_LEGACY_PRODUCT_NAME ", which is left as it is)",
                                       copied);
                }
            }
            else if (result != ERROR_FILE_NOT_FOUND)
            {
                RegCloseKey(current);
                return std::unexpected(
                    make_win32_failure("RegOpenKeyExW(earlier settings)", static_cast<unsigned long>(result)));
            }
            // done, or nothing to take over: not looked at again
            DWORD const flag = 1;
            result = RegSetValueExW(current, legacy_adopted_value, 0, REG_DWORD, reinterpret_cast<BYTE const *>(&flag),
                                    sizeof(flag));
            if (result != ERROR_SUCCESS)
            {
                RegCloseKey(current);
                return std::unexpected(
                    make_win32_failure("RegSetValueExW(settings taken over)", static_cast<unsigned long>(result)));
            }
        }
        RegCloseKey(current);

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
