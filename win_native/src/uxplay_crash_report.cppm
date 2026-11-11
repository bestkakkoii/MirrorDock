// SPDX-License-Identifier: MIT
// Crash record for a program without a console: when an unhandled exception
// (access violation, ...) or std::terminate ends the process, a text record
// (exception code, faulting address as module + offset, thread) and a minidump
// are written to the given directory first, so the cause can be traced after
// the user simply saw the program disappear.
//
// Everything the handlers need is prepared at install time; at crash time
// they use only preallocated buffers and Win32 file calls.
module;

#include "uxplay_version.h"

export module uxplay_crash_report;

import std;
import uxplay_status;

export namespace uxplay::crash_report
{
    inline constexpr std::wstring_view record_file_name = L"" UXPLAY_PRODUCT_FILE_NAME "_crash.txt";
    inline constexpr std::wstring_view dump_file_name = L"" UXPLAY_PRODUCT_FILE_NAME "_crash.dmp";

    inline constexpr std::wstring_view kept_record_file_name = L"" UXPLAY_PRODUCT_FILE_NAME "_crash_previous.txt";
    inline constexpr std::wstring_view kept_dump_file_name = L"" UXPLAY_PRODUCT_FILE_NAME "_crash_previous.dmp";

    // Installs the handlers for the whole process.  `directory` must exist.
    // The outcome reports whether the directory path fits and the handlers
    // are in place; its value is a note for the log when only the text record
    // can be written (dbghelp.dll missing: no minidump), else empty.  Nothing
    // is logged here: this runs before the program's log exists.
    [[nodiscard]] outcome<std::string> install(std::wstring_view directory);

    // The record a previous run left behind, if any.  It is moved aside
    // (kept_record_file_name / kept_dump_file_name, replacing an older one) so that it is
    // reported once; the returned path is where the text record is now.
    [[nodiscard]] std::optional<std::wstring> take_previous_record(std::wstring_view directory);
}
