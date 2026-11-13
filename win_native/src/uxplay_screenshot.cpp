// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "uxplay_version.h"

module uxplay_screenshot;

import std;
import uxplay_status;

namespace uxplay
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        // how often the clipboard is asked for while another program has it open
        constexpr int clipboard_attempts = 8;
        constexpr DWORD clipboard_retry_ms = 25;
        // further files of the same second get " (2)" and so on
        constexpr int most_same_second = 9;

        struct com_scope
        {
            HRESULT entered = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

            com_scope() = default;
            com_scope(com_scope const &) = delete;
            com_scope &operator=(com_scope const &) = delete;

            ~com_scope()
            {
                if (SUCCEEDED(entered))
                {
                    CoUninitialize();
                }
            }
        };

        // rows of 3 bytes a pixel (blue, green, red), padded to a multiple of 4
        [[nodiscard]] std::size_t padded_row(int width)
        {
            return (static_cast<std::size_t>(width) * 3 + 3) & ~std::size_t{3};
        }

        // The picture as a PNG file in memory.  rows: from the top, `stride` bytes each.
        [[nodiscard]] outcome<std::vector<std::uint8_t>> encode_png(int width, int height,
                                                                    std::span<std::uint8_t const> rows,
                                                                    std::size_t stride)
        {
            ComPtr<IWICImagingFactory> factory;
            if (HRESULT const result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                                        IID_PPV_ARGS(&factory));
                FAILED(result))
            {
                return std::unexpected(make_hresult_failure("the Windows imaging component", result));
            }
            ComPtr<IStream> stream;
            if (HRESULT const result = CreateStreamOnHGlobal(nullptr, TRUE, &stream); FAILED(result))
            {
                return std::unexpected(make_hresult_failure("CreateStreamOnHGlobal", result));
            }
            ComPtr<IWICBitmapEncoder> encoder;
            ComPtr<IWICBitmapFrameEncode> frame;
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            HRESULT result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
            char const *step = "IWICImagingFactory::CreateEncoder";
            if (SUCCEEDED(result))
            {
                step = "IWICBitmapEncoder::Initialize";
                result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
            }
            if (SUCCEEDED(result))
            {
                step = "IWICBitmapEncoder::CreateNewFrame";
                result = encoder->CreateNewFrame(&frame, nullptr);
            }
            if (SUCCEEDED(result))
            {
                step = "IWICBitmapFrameEncode::Initialize";
                result = frame->Initialize(nullptr);
            }
            if (SUCCEEDED(result))
            {
                step = "IWICBitmapFrameEncode::SetSize";
                result = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
            }
            if (SUCCEEDED(result))
            {
                step = "IWICBitmapFrameEncode::SetPixelFormat";
                result = frame->SetPixelFormat(&format);
            }
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure(step, result));
            }
            if (IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) == FALSE)
            {
                return std::unexpected(make_failure("IWICBitmapFrameEncode::SetPixelFormat", 0,
                                                    "the PNG encoder does not take 24-bit pixels"));
            }
            // (the pixels are not changed by the call; the interface has no const)
            result = frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(stride),
                                        static_cast<UINT>(rows.size()), const_cast<BYTE *>(rows.data()));
            step = "IWICBitmapFrameEncode::WritePixels";
            if (SUCCEEDED(result))
            {
                step = "IWICBitmapFrameEncode::Commit";
                result = frame->Commit();
            }
            if (SUCCEEDED(result))
            {
                step = "IWICBitmapEncoder::Commit";
                result = encoder->Commit();
            }
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure(step, result));
            }
            STATSTG facts{};
            HGLOBAL memory = nullptr;
            if (FAILED(result = stream->Stat(&facts, STATFLAG_NONAME)) ||
                FAILED(result = GetHGlobalFromStream(stream.Get(), &memory)))
            {
                return std::unexpected(make_hresult_failure("the encoded picture", result));
            }
            void const *const place = GlobalLock(memory);
            if (place == nullptr)
            {
                return std::unexpected(make_win32_failure("GlobalLock (encoded picture)", GetLastError()));
            }
            auto const *const bytes = static_cast<std::uint8_t const *>(place);
            std::vector<std::uint8_t> file(bytes, bytes + static_cast<std::size_t>(facts.cbSize.QuadPart));
            GlobalUnlock(memory);
            return file;
        }

        // "<Pictures>\MirrorDock", made when it is not there yet
        [[nodiscard]] outcome<std::filesystem::path> screenshot_folder()
        {
            PWSTR raw = nullptr;
            HRESULT const result = SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_CREATE, nullptr, &raw);
            if (FAILED(result) || raw == nullptr)
            {
                CoTaskMemFree(raw);
                return std::unexpected(make_hresult_failure("the Pictures folder", result));
            }
            std::filesystem::path folder = std::filesystem::path{raw} / L"" UXPLAY_PRODUCT_NAME;
            CoTaskMemFree(raw);
            std::error_code problem;
            std::filesystem::create_directories(folder, problem);
            if (problem)
            {
                return std::unexpected(make_failure("the folder for screenshots", problem.value(), problem.message()));
            }
            return folder;
        }

        // Writes a new file; one that exists is left alone (ERROR_FILE_EXISTS).
        [[nodiscard]] DWORD write_new_file(std::filesystem::path const &path, std::span<std::uint8_t const> bytes)
        {
            HANDLE const file =
                CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return GetLastError();
            }
            DWORD problem = ERROR_SUCCESS;
            std::size_t written = 0;
            while (written < bytes.size() && problem == ERROR_SUCCESS)
            {
                auto const piece = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - written, 1U << 24));
                DWORD done = 0;
                if (WriteFile(file, bytes.data() + written, piece, &done, nullptr) == FALSE)
                {
                    problem = GetLastError();
                }
                else if (done == 0)
                {
                    problem = ERROR_WRITE_FAULT;
                }
                written += done;
            }
            if (CloseHandle(file) == FALSE && problem == ERROR_SUCCESS)
            {
                problem = GetLastError();
            }
            if (problem != ERROR_SUCCESS)
            {
                DeleteFileW(path.c_str()); // no half-written picture stays
            }
            return problem;
        }

        // One form of the picture for the clipboard, which owns the memory
        // once it took it.  Returns a Windows error code (0: taken).
        [[nodiscard]] DWORD give_to_clipboard(UINT format, std::span<std::uint8_t const> bytes)
        {
            HGLOBAL const memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
            if (memory == nullptr)
            {
                return GetLastError();
            }
            void *const place = GlobalLock(memory);
            if (place == nullptr)
            {
                DWORD const problem = GetLastError();
                GlobalFree(memory);
                return problem;
            }
            std::memcpy(place, bytes.data(), bytes.size());
            GlobalUnlock(memory);
            if (SetClipboardData(format, memory) == nullptr)
            {
                DWORD const problem = GetLastError();
                GlobalFree(memory);
                return problem;
            }
            return ERROR_SUCCESS;
        }

        // dib: a BITMAPINFOHEADER followed by its pixels
        [[nodiscard]] status put_on_clipboard(HWND owner, std::span<std::uint8_t const> dib,
                                              std::span<std::uint8_t const> png)
        {
            bool opened = false;
            DWORD problem = ERROR_SUCCESS;
            for (int attempt = 0; attempt < clipboard_attempts && !opened; ++attempt)
            {
                opened = OpenClipboard(owner) != FALSE;
                if (!opened)
                {
                    problem = GetLastError();
                    Sleep(clipboard_retry_ms); // another program is looking at it just now
                }
            }
            if (!opened)
            {
                return std::unexpected(make_win32_failure("OpenClipboard", problem));
            }
            if (EmptyClipboard() == FALSE)
            {
                problem = GetLastError();
                CloseClipboard();
                return std::unexpected(make_win32_failure("EmptyClipboard", problem));
            }
            problem = give_to_clipboard(CF_DIB, dib);
            if (problem == ERROR_SUCCESS)
            {
                // a second form for the programs that know it; without it the bitmap is still there
                if (UINT const png_format = RegisterClipboardFormatW(L"PNG"); png_format != 0)
                {
                    (void) give_to_clipboard(png_format, png);
                }
            }
            CloseClipboard();
            if (problem != ERROR_SUCCESS)
            {
                return std::unexpected(make_win32_failure("SetClipboardData", problem));
            }
            return {};
        }
    }

    outcome<saved_screenshot> save_screenshot(int width, int height, std::span<std::uint8_t const> pixels,
                                              void *clipboard_owner)
    {
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
            pixels.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4)
        {
            return std::unexpected(make_failure("screenshot", 0,
                                                std::format("{} bytes are no picture of {} x {} pixels",
                                                            pixels.size(), width, height)));
        }
        // 3 bytes a pixel, twice: from the top for the PNG, and behind a
        // header from the bottom for the clipboard's bitmap
        std::size_t const stride = padded_row(width);
        std::size_t const picture_bytes = stride * static_cast<std::size_t>(height);
        std::vector<std::uint8_t> rows(picture_bytes);
        std::vector<std::uint8_t> dib(sizeof(BITMAPINFOHEADER) + picture_bytes);
        BITMAPINFOHEADER header{};
        header.biSize = sizeof(header);
        header.biWidth = width;
        header.biHeight = height; // bottom-up: the form every program reads
        header.biPlanes = 1;
        header.biBitCount = 24;
        header.biCompression = BI_RGB;
        header.biSizeImage = static_cast<DWORD>(picture_bytes);
        std::memcpy(dib.data(), &header, sizeof(header));
        for (int row = 0; row < height; ++row)
        {
            std::uint8_t const *source = pixels.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 4;
            std::uint8_t *const target = rows.data() + static_cast<std::size_t>(row) * stride;
            for (int column = 0; column < width; ++column)
            {
                target[column * 3] = source[column * 4];
                target[column * 3 + 1] = source[column * 4 + 1];
                target[column * 3 + 2] = source[column * 4 + 2];
            }
            std::memcpy(dib.data() + sizeof(header) + static_cast<std::size_t>(height - 1 - row) * stride, target,
                        stride);
        }

        com_scope const com;
        auto png = encode_png(width, height, rows, stride);
        if (!png)
        {
            return std::unexpected(png.error());
        }
        rows = {};
        auto folder = screenshot_folder();
        if (!folder)
        {
            return std::unexpected(folder.error());
        }
        SYSTEMTIME now{};
        GetLocalTime(&now);
        std::wstring const stem = std::format(L"" UXPLAY_PRODUCT_NAME L" {:04}-{:02}-{:02} {:02}{:02}{:02}", now.wYear,
                                              now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        saved_screenshot saved;
        DWORD problem = ERROR_FILE_EXISTS;
        for (int number = 1; number <= most_same_second && problem == ERROR_FILE_EXISTS; ++number)
        {
            saved.file = *folder / (number == 1 ? stem + L".png" : std::format(L"{} ({}).png", stem, number));
            problem = write_new_file(saved.file, *png);
        }
        if (problem != ERROR_SUCCESS)
        {
            return std::unexpected(make_win32_failure("writing the screenshot", problem));
        }
        if (status const copied = put_on_clipboard(static_cast<HWND>(clipboard_owner), dib, *png); copied)
        {
            saved.on_clipboard = true;
        }
        else
        {
            saved.clipboard_problem = copied.error().describe();
        }
        return saved;
    }
}
