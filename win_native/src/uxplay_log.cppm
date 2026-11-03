// SPDX-License-Identifier: MIT
// Process-wide log sink for the native media engine.  The sink is installed
// once by the C bridge and forwards to UxPlay's console logger, so engine and
// AirPlay-core messages share one ordered stream.
export module uxplay_log;

import std;

export namespace uxplay::log
{
    enum class level : int
    {
        error = 3,
        warning = 4,
        info = 6,
        debug = 7
    };

    using sink_function = void (*)(int message_level, char const *message);

    void install_sink(sink_function sink, level threshold);

    [[nodiscard]] bool enabled(level message_level);

    void write(level message_level, std::string_view message);

    template <class... argument_types>
    void error(std::format_string<argument_types...> format, argument_types &&...arguments)
    {
        if (enabled(level::error))
        {
            write(level::error, std::format(format, std::forward<argument_types>(arguments)...));
        }
    }

    template <class... argument_types>
    void warning(std::format_string<argument_types...> format, argument_types &&...arguments)
    {
        if (enabled(level::warning))
        {
            write(level::warning, std::format(format, std::forward<argument_types>(arguments)...));
        }
    }

    template <class... argument_types>
    void info(std::format_string<argument_types...> format, argument_types &&...arguments)
    {
        if (enabled(level::info))
        {
            write(level::info, std::format(format, std::forward<argument_types>(arguments)...));
        }
    }

    template <class... argument_types>
    void debug(std::format_string<argument_types...> format, argument_types &&...arguments)
    {
        if (enabled(level::debug))
        {
            write(level::debug, std::format(format, std::forward<argument_types>(arguments)...));
        }
    }
}
