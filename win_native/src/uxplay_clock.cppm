// SPDX-License-Identifier: MIT
// Monotonic timeline of the native media engine.
//
// now_ns() is QueryPerformanceCounter expressed in nanoseconds using exactly
// the same conversion as raop_ntp_get_local_time() in lib/raop_ntp.c, so
// AirPlay local timestamps, DXGI frame statistics and WASAPI QPC positions
// all share one clock without any offset estimation.
//
// precise_waiter paces the render thread: it wakes at a deadline on this
// timeline with sub-millisecond accuracy and needs no process-wide timer
// resolution request.  interval_gate rate-limits repeating log messages.
export module uxplay_clock;

import std;

export namespace uxplay::clock
{
    [[nodiscard]] std::int64_t now_ns();

    [[nodiscard]] std::int64_t qpc_ticks_to_ns(std::int64_t ticks);

    // WASAPI and DXGI report QPC-derived positions in 100 ns units.
    [[nodiscard]] constexpr std::int64_t hundred_ns_to_ns(std::int64_t value)
    {
        return value * 100;
    }

    inline constexpr std::int64_t ns_per_ms = 1'000'000;
    inline constexpr std::int64_t ns_per_second = 1'000'000'000;

    // High-resolution waitable timer (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION):
    // sub-millisecond sleeps without raising the global timer resolution.
    class precise_waiter
    {
    public:
        precise_waiter();
        ~precise_waiter();
        precise_waiter(precise_waiter const &) = delete;
        precise_waiter &operator=(precise_waiter const &) = delete;

        // false: this Windows version has no high-resolution timers, so waits
        // end on the system timer tick (15.6 ms unless something raised it).
        [[nodiscard]] bool high_resolution() const;

        void sleep_until(std::int64_t deadline_ns);

        // Waits for `handle` or the deadline; true when the handle was signaled.
        [[nodiscard]] bool wait_until(void *handle, std::int64_t deadline_ns);

    private:
        void arm(std::int64_t deadline_ns);

        void *timer_handle;
        bool precise;
    };

    // Lets a repeating event through once per interval and counts the ones it
    // holds back, so a failure that repeats every frame is logged once and
    // then as a count.  Not synchronized: one thread per gate.
    class interval_gate
    {
    public:
        explicit interval_gate(std::int64_t interval_ns);

        // An event at time_ns.  Returns the number of events held back since
        // the last one that passed (0 for the very first) when this one passes,
        // or nothing when it is held back too.
        [[nodiscard]] std::optional<std::uint64_t> pass(std::int64_t time_ns);

        // Held-back events that are old enough to be reported at time_ns (and
        // forgets them); 0 when there are none or the interval has not elapsed.
        [[nodiscard]] std::uint64_t take_overdue(std::int64_t time_ns);

    private:
        std::int64_t interval;
        std::int64_t passed_at_ns;
        std::uint64_t held_back;
        bool used;
    };
}
