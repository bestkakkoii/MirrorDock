// SPDX-License-Identifier: MIT
// Explicit failure outcomes shared by the Windows-native media engine.
// A failure keeps the stage that failed, the original error code and the
// original system/library message so diagnostics never lose the root cause.
export module uxplay_status;

import std;

export namespace uxplay
{
    struct failure
    {
        std::string stage;
        std::int64_t code = 0;
        std::string message;

        [[nodiscard]] std::string describe() const;
    };

    template <class value_type>
    using outcome = std::expected<value_type, failure>;

    using status = std::expected<void, failure>;

    [[nodiscard]] failure make_failure(std::string_view stage, std::int64_t code, std::string_view message);

    // HRESULT text comes from FormatMessageW (system message table).
    [[nodiscard]] failure make_hresult_failure(std::string_view stage, long hresult);

    // Win32 error code text (GetLastError() values).
    [[nodiscard]] failure make_win32_failure(std::string_view stage, unsigned long error_code);

    [[nodiscard]] std::string utf16_to_utf8(std::wstring_view text);
    [[nodiscard]] std::wstring utf8_to_utf16(std::string_view text);
}
