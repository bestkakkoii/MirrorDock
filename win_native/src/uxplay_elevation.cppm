// SPDX-License-Identifier: MIT
// Running one of Windows' own tools with administrator rights.
//
// The few things the program changes that Windows keeps for administrators
// (a route's metric, a disabled adapter, a firewall rule, a stopped service)
// are done by the tool Windows has for it, started elevated: the consent
// prompt names that Microsoft-signed tool, and no code of this program ever
// runs elevated.
export module uxplay_elevation;

import std;
import uxplay_status;

export namespace uxplay
{
    // "<System32>\<name>": the tool of that name that belongs to Windows
    [[nodiscard]] std::wstring system_tool(std::wstring_view name);

    // Runs `file` elevated and hidden with `parameters`, waits for it and
    // returns its exit code.  owner_window: the HWND the consent prompt
    // belongs to (may be null).  A declined prompt is a failure with the code
    // ERROR_CANCELLED (1223); a tool that has not ended after `patience` is a
    // failure too (it is left to end alone).
    [[nodiscard]] outcome<unsigned long> run_elevated(std::wstring const &file, std::wstring const &parameters,
                                                      void *owner_window, std::chrono::milliseconds patience);

    // the code a declined consent prompt fails with
    inline constexpr unsigned long consent_declined = 1223; // ERROR_CANCELLED
}
