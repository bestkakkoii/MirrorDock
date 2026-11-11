// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <avrt.h>
#include <processthreadsapi.h>

module uxplay_system_tuning;

import std;
import uxplay_status;
import uxplay_log;

namespace uxplay::system_tuning
{
    namespace
    {
        struct tuning_state
        {
            std::mutex guard;
            // Windows accepted the timer-resolution opt-out (Windows 11)
            bool timer_opt_out = false;
            // priority class the program was started with
            std::optional<DWORD> launch_priority;
            std::optional<bool> session_active;
        };

        tuning_state &tuning()
        {
            static tuning_state instance;
            return instance;
        }

        // Replaces the power-throttling policy of the process.  Windows manages
        // every mechanism that is missing from the control mask itself, so each
        // call names all the mechanisms this process decides; all of them are
        // switched off (StateMask 0).
        status set_power_throttling(ULONG control_mask)
        {
            PROCESS_POWER_THROTTLING_STATE throttling{};
            throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            throttling.ControlMask = control_mask;
            throttling.StateMask = 0;
            BOOL const applied =
                SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
            if (applied == 0)
            {
                return std::unexpected(
                    make_win32_failure("SetProcessInformation(ProcessPowerThrottling)", GetLastError()));
            }
            return {};
        }

        status apply_power_policy(tuning_state &state, bool session)
        {
            ULONG const session_mask = session ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0UL;
            if (state.timer_opt_out &&
                set_power_throttling(session_mask | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION))
            {
                return {};
            }
            return set_power_throttling(session_mask);
        }

        void remember_launch_priority(tuning_state &state)
        {
            if (state.launch_priority)
            {
                return;
            }
            DWORD const priority = GetPriorityClass(GetCurrentProcess());
            if (priority == 0)
            {
                log::warning("host tuning: {}", make_win32_failure("GetPriorityClass", GetLastError()).describe());
                return;
            }
            state.launch_priority = priority;
        }

        int priority_rank(DWORD priority_class)
        {
            switch (priority_class)
            {
            case IDLE_PRIORITY_CLASS:
                return 0;
            case BELOW_NORMAL_PRIORITY_CLASS:
                return 1;
            case ABOVE_NORMAL_PRIORITY_CLASS:
                return 3;
            case HIGH_PRIORITY_CLASS:
                return 4;
            case REALTIME_PRIORITY_CLASS:
                return 5;
            default:
                return 2; // NORMAL_PRIORITY_CLASS
            }
        }
    }

    process_tuning_report apply_process_tuning()
    {
        process_tuning_report report{};
        {
            tuning_state &state = tuning();
            std::scoped_lock lock{state.guard};
            remember_launch_priority(state);
            // Keep high-resolution timer requests honoured while the window is
            // occluded/minimized (Windows 11 otherwise ignores them; older
            // versions do not know the mechanism and reject the call).
            bool const session = state.session_active.value_or(false);
            ULONG const session_mask = session ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0UL;
            state.timer_opt_out =
                set_power_throttling(session_mask | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION).has_value();
            report.timer_resolution_ignored_opt_out = state.timer_opt_out;
        }
        // the executable manifest normally sets it already; either way counts
        report.dpi_awareness_set = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != 0 ||
                                   AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                                                DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != 0;
        return report;
    }

    void set_session_active(bool active)
    {
        tuning_state &state = tuning();
        std::scoped_lock lock{state.guard};
        if (state.session_active == active)
        {
            return;
        }
        state.session_active = active;
        remember_launch_priority(state);
        char const *const moment = active ? "session start" : "session end";

        if (status const applied = apply_power_policy(state, active); !applied)
        {
            log::warning("host tuning at {}: {}", moment, applied.error().describe());
        }

        DWORD const launch_priority = state.launch_priority.value_or(NORMAL_PRIORITY_CLASS);
        // a program started above ABOVE_NORMAL keeps what its user chose
        DWORD const wanted = active && priority_rank(launch_priority) < priority_rank(ABOVE_NORMAL_PRIORITY_CLASS)
                                 ? ABOVE_NORMAL_PRIORITY_CLASS
                                 : launch_priority;
        if (SetPriorityClass(GetCurrentProcess(), wanted) == 0)
        {
            log::warning("host tuning at {}: {}", moment,
                         make_win32_failure("SetPriorityClass", GetLastError()).describe());
        }
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
