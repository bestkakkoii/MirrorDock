// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

module uxplay_clock;

import std;

namespace uxplay::clock
{
    namespace
    {
        std::int64_t qpc_frequency()
        {
            static std::int64_t const frequency = []
            {
                LARGE_INTEGER value{};
                QueryPerformanceFrequency(&value);
                return static_cast<std::int64_t>(value.QuadPart);
            }();
            return frequency;
        }
    }

    std::int64_t qpc_ticks_to_ns(std::int64_t ticks)
    {
        std::int64_t const frequency = qpc_frequency();
        std::int64_t const whole_seconds = ticks / frequency;
        std::int64_t const remainder = ticks % frequency;
        return whole_seconds * ns_per_second + (remainder * ns_per_second) / frequency;
    }

    std::int64_t now_ns()
    {
        LARGE_INTEGER value{};
        QueryPerformanceCounter(&value);
        return qpc_ticks_to_ns(static_cast<std::int64_t>(value.QuadPart));
    }

    precise_waiter::precise_waiter()
        : timer_handle{CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS)}
    {
        if (timer_handle == nullptr)
        {
            // Older kernels lack the high-resolution flag; a normal timer still works.
            timer_handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
    }

    precise_waiter::~precise_waiter()
    {
        if (timer_handle != nullptr)
        {
            CloseHandle(timer_handle);
        }
    }

    void precise_waiter::arm(std::int64_t deadline_ns)
    {
        std::int64_t const remaining_ns = deadline_ns - now_ns();
        LARGE_INTEGER due{};
        // negative = relative time in 100 ns units
        due.QuadPart = -std::max<std::int64_t>(remaining_ns / 100, 1);
        SetWaitableTimerEx(timer_handle, &due, 0, nullptr, nullptr, nullptr, 0);
    }

    void precise_waiter::sleep_until(std::int64_t deadline_ns)
    {
        if (deadline_ns <= now_ns())
        {
            return;
        }
        if (timer_handle == nullptr)
        {
            std::this_thread::sleep_for(std::chrono::nanoseconds{deadline_ns - now_ns()});
            return;
        }
        arm(deadline_ns);
        WaitForSingleObject(timer_handle, INFINITE);
    }

    bool precise_waiter::wait_until(void *handle, std::int64_t deadline_ns)
    {
        if (deadline_ns <= now_ns())
        {
            return WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
        }
        if (timer_handle == nullptr)
        {
            auto const remaining_ms = static_cast<DWORD>((deadline_ns - now_ns()) / ns_per_ms + 1);
            return WaitForSingleObject(handle, remaining_ms) == WAIT_OBJECT_0;
        }
        arm(deadline_ns);
        HANDLE const handles[2] = {static_cast<HANDLE>(handle), timer_handle};
        DWORD const result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0)
        {
            CancelWaitableTimer(timer_handle);
            return true;
        }
        return false;
    }
}
