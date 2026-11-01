// Monotonic timeline of the native media engine.
//
// now_ns() is QueryPerformanceCounter expressed in nanoseconds using exactly
// the same conversion as raop_ntp_get_local_time() in lib/raop_ntp.c, so
// AirPlay local timestamps, DXGI frame statistics and WASAPI QPC positions
// all share one clock without any offset estimation.
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

        void sleep_until(std::int64_t deadline_ns);

        // Waits for `handle` or the deadline; true when the handle was signaled.
        [[nodiscard]] bool wait_until(void *handle, std::int64_t deadline_ns);

    private:
        void arm(std::int64_t deadline_ns);

        void *timer_handle;
    };
}
