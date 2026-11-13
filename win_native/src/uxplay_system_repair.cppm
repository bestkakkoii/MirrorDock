// SPDX-License-Identifier: MIT
// One-click repairs for the rows of "This PC" (uxplay_system_report).
//
// Two kinds.  A real repair changes something only an administrator may
// change, with the tool Windows has for it and behind Windows' own consent
// prompt (uxplay_elevation): a firewall rule for this program, Apple's
// service started.  The other kind opens the page of Windows' Settings on
// which the user makes a choice that is theirs to make (the refresh rate,
// the kind of network), or starts an installer the user then goes through.
// Whether a repair helped is never assumed: the report is made anew after it.
export module uxplay_system_repair;

import std;
import uxplay_status;
import uxplay_system_report;

export namespace uxplay
{
    enum class repair_result
    {
        done,     // the change was made (for the firewall: and a look at its rules confirms it)
        opened,   // a page of Settings or an installer was opened: the user goes on there
        declined, // the user said no to the administrator prompt
        failed
    };

    struct repair_outcome
    {
        repair_result result = repair_result::failed;
        std::string detail; // what was done and how it went: English, for the log
    };

    // Whether `action` is one this module makes (the iPhone's network adapter
    // and the update belong to other parts of the program).
    [[nodiscard]] bool repairs(report_action action);

    // Blocks until it is done: for a real repair that includes the consent
    // prompt (owner_window: the HWND it belongs to; may be null) and the tool
    // behind it.  Not for a thread that has to stay responsive.
    [[nodiscard]] repair_outcome repair(report_action action, void *owner_window);
}
