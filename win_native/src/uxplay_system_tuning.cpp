module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <avrt.h>
#include <processthreadsapi.h>

module uxplay_system_tuning;

import std;

namespace uxplay::system_tuning
{
    process_tuning_report apply_process_tuning()
    {
        process_tuning_report report{};

        PROCESS_POWER_THROTTLING_STATE throttling{};
        throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        // ControlMask selects the policies we decide; StateMask = 0 turns them off.
        throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        throttling.StateMask = 0;
        report.power_throttling_disabled =
            SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling)) != 0;

        // Keep high-resolution timer requests honoured while the window is
        // occluded/minimized (Windows 11 otherwise ignores them).
        PROCESS_POWER_THROTTLING_STATE timer_state{};
        timer_state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        timer_state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        timer_state.StateMask = 0;
        report.timer_resolution_ignored_opt_out =
            SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &timer_state, sizeof(timer_state)) != 0;

        report.priority_raised = SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) != 0;
        // the executable manifest normally sets it already; either way counts
        report.dpi_awareness_set = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != 0 ||
                                   AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                                                DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != 0;
        return report;
    }

    mmcss_scope::mmcss_scope(wchar_t const *task_name)
        : task_handle{nullptr}
    {
        DWORD task_index = 0;
        task_handle = AvSetMmThreadCharacteristicsW(task_name, &task_index);
        if (task_handle != nullptr)
        {
            AvSetMmThreadPriority(task_handle, AVRT_PRIORITY_HIGH);
        }
    }

    mmcss_scope::~mmcss_scope()
    {
        if (task_handle != nullptr)
        {
            AvRevertMmThreadCharacteristics(task_handle);
        }
    }

    bool mmcss_scope::active() const
    {
        return task_handle != nullptr;
    }
}
