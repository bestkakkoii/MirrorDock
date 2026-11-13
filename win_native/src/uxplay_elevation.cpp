// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

module uxplay_elevation;

import std;
import uxplay_status;

namespace uxplay
{
    namespace
    {
        // ShellExecuteEx may hand the request to a shell extension: COM has to be there
        struct com_scope
        {
            bool initialized = false;

            com_scope()
                : initialized{SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))}
            {
            }

            com_scope(com_scope const &) = delete;
            com_scope &operator=(com_scope const &) = delete;

            ~com_scope()
            {
                if (initialized)
                {
                    CoUninitialize();
                }
            }
        };
    }

    std::wstring system_tool(std::wstring_view name)
    {
        std::array<wchar_t, MAX_PATH> directory{};
        UINT const length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
        if (length == 0 || length >= directory.size())
        {
            return std::wstring{name};
        }
        return std::format(L"{}\\{}", std::wstring_view{directory.data(), length}, name);
    }

    outcome<unsigned long> run_elevated(std::wstring const &file, std::wstring const &parameters, void *owner_window,
                                        std::chrono::milliseconds patience)
    {
        com_scope const com;
        SHELLEXECUTEINFOW request{};
        request.cbSize = sizeof(request);
        request.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        request.hwnd = static_cast<HWND>(owner_window);
        request.lpVerb = L"runas";
        request.lpFile = file.c_str();
        request.lpParameters = parameters.c_str();
        request.nShow = SW_HIDE;
        if (ShellExecuteExW(&request) == FALSE)
        {
            DWORD const error = GetLastError();
            if (error == ERROR_CANCELLED)
            {
                return std::unexpected(
                    make_failure("administrator consent", ERROR_CANCELLED, "administrator consent was declined"));
            }
            return std::unexpected(make_win32_failure("ShellExecuteExW(runas)", error));
        }
        if (request.hProcess == nullptr)
        {
            return std::unexpected(make_failure("ShellExecuteExW(runas)", 0, "no process handle returned"));
        }
        DWORD exit_code = 0;
        auto const wait_ms = static_cast<DWORD>(std::clamp<std::chrono::milliseconds::rep>(patience.count(), 1, 600'000));
        DWORD const waited = WaitForSingleObject(request.hProcess, wait_ms);
        bool const got_code = waited == WAIT_OBJECT_0 && GetExitCodeProcess(request.hProcess, &exit_code) != FALSE;
        DWORD const wait_error = got_code ? 0 : (waited == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError());
        CloseHandle(request.hProcess);
        if (!got_code)
        {
            return std::unexpected(make_failure("waiting for the elevated tool", wait_error,
                                                waited == WAIT_TIMEOUT ? "timed out" : "wait failed"));
        }
        return exit_code;
    }
}
