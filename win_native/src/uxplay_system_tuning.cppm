// SPDX-License-Identifier: MIT
// Host tuning for this laptop (i7-6700HQ, Intel HD 530 + GTX 960M Optimus,
// battery-capable).  Everything here is process- or thread-scoped and is
// reverted automatically by Windows when the process exits; no system
// settings are modified.
export module uxplay_system_tuning;

import std;

export namespace uxplay::system_tuning
{
    struct process_tuning_report
    {
        bool power_throttling_disabled = false;
        bool timer_resolution_ignored_opt_out = false;
        bool priority_raised = false;
        bool dpi_awareness_set = false;
    };

    // Opts the process out of EcoQoS/power throttling (keeps P-states high
    // enough for steady 60 fps decode on battery), raises the priority class
    // to ABOVE_NORMAL and enables per-monitor DPI awareness v2.
    process_tuning_report apply_process_tuning();

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
