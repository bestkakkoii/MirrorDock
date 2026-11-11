// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include "uxplay_version.h"

module uxplay_crash_report;

import std;
import uxplay_status;

namespace uxplay::crash_report
{
    namespace
    {
        using minidump_writer = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                               PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

        constexpr std::size_t path_capacity = 1024;

        // written once by install(), read by the handlers
        wchar_t record_path[path_capacity]{};
        wchar_t dump_path[path_capacity]{};
        minidump_writer write_minidump = nullptr;
        std::atomic<bool> handling{false};

        // Appends text to a fixed buffer (no allocation).
        struct record_text
        {
            char bytes[2048]{};
            std::size_t length = 0;

            void add(std::string_view text)
            {
                std::size_t const count = std::min(text.size(), sizeof(bytes) - length);
                std::memcpy(bytes + length, text.data(), count);
                length += count;
            }

            void add_hex(std::uint64_t value)
            {
                char digits[19]{};
                auto const [end, error] = std::to_chars(std::begin(digits), std::end(digits), value, 16);
                add("0x");
                add(std::string_view{digits, static_cast<std::size_t>(end - digits)});
            }

            void add_number(std::uint64_t value)
            {
                char digits[21]{};
                auto const [end, error] = std::to_chars(std::begin(digits), std::end(digits), value, 10);
                add(std::string_view{digits, static_cast<std::size_t>(end - digits)});
            }

            void add_wide(wchar_t const *text)
            {
                char narrow[512]{};
                int const count = WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow, sizeof(narrow), nullptr, nullptr);
                if (count > 1)
                {
                    add(std::string_view{narrow, static_cast<std::size_t>(count - 1)});
                }
            }
        };

        void add_time(record_text &text)
        {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            text.add_number(now.wYear);
            text.add("-");
            text.add_number(now.wMonth);
            text.add("-");
            text.add_number(now.wDay);
            text.add(" ");
            text.add_number(now.wHour);
            text.add(":");
            text.add_number(now.wMinute);
            text.add(":");
            text.add_number(now.wSecond);
        }

        // "module.exe + 0x1234" for an address inside a loaded module
        void add_location(record_text &text, void const *address)
        {
            text.add_hex(reinterpret_cast<std::uintptr_t>(address));
            HMODULE module = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<wchar_t const *>(address), &module) == 0 ||
                module == nullptr)
            {
                text.add(" (no module)");
                return;
            }
            wchar_t module_path[512]{};
            DWORD const length = GetModuleFileNameW(module, module_path, 512);
            wchar_t const *name = module_path;
            for (DWORD index = 0; index < length; ++index)
            {
                if (module_path[index] == L'\\' || module_path[index] == L'/')
                {
                    name = module_path + index + 1;
                }
            }
            text.add(" (");
            text.add_wide(name);
            text.add(" + ");
            text.add_hex(reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module));
            text.add(")");
        }

        void write_record(record_text const &text)
        {
            HANDLE const file = CreateFileW(record_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return;
            }
            DWORD written = 0;
            WriteFile(file, text.bytes, static_cast<DWORD>(text.length), &written, nullptr);
            CloseHandle(file);
        }

        void write_dump(EXCEPTION_POINTERS *exception)
        {
            if (write_minidump == nullptr)
            {
                return;
            }
            HANDLE const file = CreateFileW(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                            nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return;
            }
            MINIDUMP_EXCEPTION_INFORMATION information{};
            information.ThreadId = GetCurrentThreadId();
            information.ExceptionPointers = exception;
            information.ClientPointers = FALSE;
            auto const type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo);
            write_minidump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                           exception != nullptr ? &information : nullptr, nullptr, nullptr);
            CloseHandle(file);
        }

        LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS *exception)
        {
            if (handling.exchange(true))
            {
                return EXCEPTION_EXECUTE_HANDLER; // a second fault while recording: just end
            }
            record_text text;
            text.add(UXPLAY_PRODUCT_NAME " ended with an unhandled exception\r\ntime: ");
            add_time(text);
            text.add("\r\nexception code: ");
            text.add_hex(exception->ExceptionRecord->ExceptionCode);
            text.add("\r\nat: ");
            add_location(text, exception->ExceptionRecord->ExceptionAddress);
            if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                exception->ExceptionRecord->NumberParameters >= 2)
            {
                text.add(exception->ExceptionRecord->ExceptionInformation[0] == 1 ? "\r\nwriting address: "
                                                                                  : "\r\nreading address: ");
                text.add_hex(exception->ExceptionRecord->ExceptionInformation[1]);
            }
            text.add("\r\nthread: ");
            text.add_number(GetCurrentThreadId());
            text.add("\r\nminidump: ");
            text.add_wide(dump_path);
            text.add("\r\n");
            write_record(text);
            write_dump(exception);
            return EXCEPTION_EXECUTE_HANDLER;
        }

        [[noreturn]] void on_terminate()
        {
            if (!handling.exchange(true))
            {
                record_text text;
                text.add(UXPLAY_PRODUCT_NAME " ended in std::terminate\r\ntime: ");
                add_time(text);
                if (std::exception_ptr const active = std::current_exception(); active)
                {
                    try
                    {
                        std::rethrow_exception(active);
                    }
                    catch (std::exception const &error)
                    {
                        text.add("\r\nuncaught C++ exception: ");
                        text.add(error.what());
                    }
                    catch (...)
                    {
                        text.add("\r\nuncaught C++ exception of an unknown type");
                    }
                }
                else
                {
                    text.add("\r\nno active exception (noexcept violation, joinable thread destroyed, or abort)");
                }
                text.add("\r\nthread: ");
                text.add_number(GetCurrentThreadId());
                text.add("\r\nminidump: ");
                text.add_wide(dump_path);
                text.add("\r\n");
                write_record(text);
                write_dump(nullptr);
            }
            TerminateProcess(GetCurrentProcess(), 3);
            std::abort(); // not reached
        }

        outcome<void> join_path(wchar_t (&destination)[path_capacity], std::wstring_view directory,
                                std::wstring_view file_name)
        {
            if (directory.size() + 1 + file_name.size() + 1 > path_capacity)
            {
                return std::unexpected(make_win32_failure("crash report path", ERROR_FILENAME_EXCED_RANGE));
            }
            std::size_t position = 0;
            for (wchar_t const character : directory)
            {
                destination[position++] = character;
            }
            destination[position++] = L'\\';
            for (wchar_t const character : file_name)
            {
                destination[position++] = character;
            }
            destination[position] = L'\0';
            return {};
        }
    }

    outcome<std::string> install(std::wstring_view directory)
    {
        if (auto joined = join_path(record_path, directory, record_file_name); !joined)
        {
            return std::unexpected(joined.error());
        }
        if (auto joined = join_path(dump_path, directory, dump_file_name); !joined)
        {
            return std::unexpected(joined.error());
        }
        std::string note;
        // loaded now: the loader must not run inside a crashing process
        if (HMODULE const dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            dbghelp != nullptr)
        {
            write_minidump = reinterpret_cast<minidump_writer>(
                reinterpret_cast<void *>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
        }
        if (write_minidump == nullptr)
        {
            note = std::format("crash report: no minidump will be written: {}",
                               make_win32_failure("LoadLibraryExW(dbghelp.dll)", GetLastError()).describe());
        }
        SetUnhandledExceptionFilter(on_unhandled_exception);
        std::set_terminate(on_terminate);
        return note;
    }

    std::optional<std::wstring> take_previous_record(std::wstring_view directory)
    {
        auto const in_directory = [directory](std::wstring_view file_name)
        {
            std::wstring path{directory};
            path += L'\\';
            path += file_name;
            return path;
        };
        std::wstring const record = in_directory(record_file_name);
        DWORD const attributes = GetFileAttributesW(record.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return std::nullopt;
        }
        // the dump first: it belongs to the record that is moved next (a dump
        // may be missing, then an older kept one would not match and goes)
        std::wstring const kept_dump = in_directory(kept_dump_file_name);
        if (!MoveFileExW(in_directory(dump_file_name).c_str(), kept_dump.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            DeleteFileW(kept_dump.c_str());
        }
        std::wstring const kept_record = in_directory(kept_record_file_name);
        if (!MoveFileExW(record.c_str(), kept_record.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            return record; // it stays where it is and is reported again next time
        }
        return kept_record;
    }
}
