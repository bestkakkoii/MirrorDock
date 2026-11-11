// SPDX-License-Identifier: MIT
module uxplay_log;

import std;

namespace uxplay::log
{
    namespace
    {
        struct sink_state
        {
            std::atomic<sink_function> sink{nullptr};
            std::atomic<int> threshold{static_cast<int>(level::info)};
            std::mutex write_mutex;
        };

        sink_state &state()
        {
            static sink_state instance;
            return instance;
        }
    }

    void install_sink(sink_function sink, level threshold)
    {
        state().threshold.store(static_cast<int>(threshold), std::memory_order_relaxed);
        state().sink.store(sink, std::memory_order_release);
    }

    bool enabled(level message_level)
    {
        return static_cast<int>(message_level) <= state().threshold.load(std::memory_order_relaxed);
    }

    void write(level message_level, std::string_view message)
    {
        sink_state &current = state();
        std::string const line = std::format("[native] {}", message);
        std::scoped_lock lock{current.write_mutex};
        if (sink_function const sink = current.sink.load(std::memory_order_acquire); sink != nullptr)
        {
            sink(static_cast<int>(message_level), line.c_str());
            return;
        }
        std::println(std::cerr, "{}", line);
    }
}
