// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

module uxplay_status;

import std;

namespace uxplay
{
    std::string failure::describe() const
    {
        if (message.empty())
        {
            return std::format("{} failed (code {:#x})", stage, static_cast<std::uint64_t>(code));
        }
        return std::format("{} failed (code {:#x}): {}", stage, static_cast<std::uint64_t>(code), message);
    }

    failure make_failure(std::string_view stage, std::int64_t code, std::string_view message)
    {
        return failure{std::string{stage}, code, std::string{message}};
    }

    namespace
    {
        std::string system_message(unsigned long message_id)
        {
            wchar_t *buffer = nullptr;
            DWORD const length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                                    FORMAT_MESSAGE_IGNORE_INSERTS,
                                                nullptr, message_id, 0, reinterpret_cast<wchar_t *>(&buffer), 0,
                                                nullptr);
            if (length == 0 || buffer == nullptr)
            {
                return {};
            }
            std::wstring_view text{buffer, length};
            while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
            {
                text.remove_suffix(1);
            }
            std::string result = utf16_to_utf8(text);
            LocalFree(buffer);
            return result;
        }
    }

    failure make_hresult_failure(std::string_view stage, long hresult)
    {
        return failure{std::string{stage}, static_cast<std::int64_t>(static_cast<std::uint32_t>(hresult)),
                       system_message(static_cast<unsigned long>(hresult))};
    }

    failure make_win32_failure(std::string_view stage, unsigned long error_code)
    {
        return failure{std::string{stage}, static_cast<std::int64_t>(error_code), system_message(error_code)};
    }

    std::string utf16_to_utf8(std::wstring_view text)
    {
        if (text.empty())
        {
            return {};
        }
        int const required = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                                 nullptr, nullptr);
        if (required <= 0)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(required), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), required, nullptr,
                            nullptr);
        return result;
    }

    std::wstring utf8_to_utf16(std::string_view text)
    {
        if (text.empty())
        {
            return {};
        }
        int const required = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (required <= 0)
        {
            return {};
        }
        std::wstring result(static_cast<std::size_t>(required), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), required);
        return result;
    }
}
