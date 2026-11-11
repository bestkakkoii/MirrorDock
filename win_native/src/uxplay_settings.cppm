// SPDX-License-Identifier: MIT
// Per-user settings kept between runs (HKCU\Software\UxPlay): small numbers
// such as the link preference, the performance mode and the window size, and
// the "start with Windows" entry (HKCU\...\CurrentVersion\Run).
// Every access reports its outcome; a value that was never stored is not an
// error.
export module uxplay_settings;

import std;
import uxplay_status;

export namespace uxplay::settings
{
    // the number stored under `name`, or std::nullopt when none is stored
    [[nodiscard]] outcome<std::optional<std::uint32_t>> read_number(std::wstring_view name);

    [[nodiscard]] outcome<void> write_number(std::wstring_view name, std::uint32_t value);

    // Convenience for callers whose only reaction to a failed access is the
    // default value: the failure is logged as a warning with its Win32 code.
    [[nodiscard]] std::optional<std::uint32_t> stored_number(std::wstring_view name);
    void store_number(std::wstring_view name, std::uint32_t value);

    // "Start with Windows": the program path registered for the current user
    // (std::nullopt when it is not registered).
    // Once after the program changed its name (UxPlay up to 1.6): every setting
    // stored under the earlier name that this program's key lacks is copied
    // (the earlier key stays, for an earlier version that may still be used;
    // the value "legacy_settings_adopted" marks that it was done), and a
    // "start with Windows" entry of the earlier name becomes one for this
    // program.  The value says what was done, for the log; it is empty when
    // there was nothing to do.
    [[nodiscard]] outcome<std::string> adopt_legacy_settings();

    [[nodiscard]] outcome<std::optional<std::wstring>> autostart_command();

    // Registers this executable (quoted path) to start with Windows, or
    // removes the registration.
    [[nodiscard]] outcome<void> set_autostart(bool enabled);
}
