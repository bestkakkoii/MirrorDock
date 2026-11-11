// SPDX-License-Identifier: MIT
// Host tuning.  Everything here is process- or thread-scoped and is reverted
// automatically by Windows when the process exits; no system settings are
// modified.
//
// Two scopes:
// * whole process (apply_process_tuning, once at start): per-monitor DPI
//   awareness and the opt-out that keeps timer-resolution requests honoured
//   while the window is occluded;
// * one mirroring session (set_session_active): ABOVE_NORMAL priority class
//   and no power throttling (EcoQoS) while a stream is on screen, the
//   priority class the program was started with and Windows' own throttling
//   decisions while it only waits in the notification area.
// Frame pacing needs no timer-resolution request: the render thread uses a
// high-resolution waitable timer (uxplay_clock).
export module uxplay_system_tuning;

import std;

export namespace uxplay::system_tuning
{
    struct process_tuning_report
    {
        // set per session since set_session_active exists: always false here
        bool power_throttling_disabled = false;
        bool timer_resolution_ignored_opt_out = false;
        // set per session since set_session_active exists: always false here
        bool priority_raised = false;
        bool dpi_awareness_set = false;
    };

    // What must hold for the whole process: per-monitor DPI awareness v2 and
    // the timer-resolution opt-out (Windows 11; older versions report false).
    process_tuning_report apply_process_tuning();

    // A mirroring session starts (true) or ends (false); any thread, repeated
    // calls with the same value do nothing.  Active: power throttling off and
    // ABOVE_NORMAL priority class (left alone when the program was started
    // with a higher one).  Inactive: throttling decided by Windows again and
    // the priority class the program was started with (NORMAL by default).
    // A failing call is logged with its Win32 code; nothing is thrown.
    void set_session_active(bool active);

    // MMCSS registration of the calling thread ("Pro Audio", "Playback", ...).
    // Raises scheduling priority of latency-critical threads while alive.
    class mmcss_scope
    {
    public:
        explicit mmcss_scope(wchar_t const *task_name);
        ~mmcss_scope();
        mmcss_scope(mmcss_scope const &) = delete;
        mmcss_scope &operator=(mmcss_scope const &) = delete;

        [[nodiscard]] bool active() const;

    private:
        void *task_handle;
    };
}
