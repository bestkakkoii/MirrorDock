// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

module uxplay_gpu_device;

import std;
import uxplay_status;
import uxplay_log;

using Microsoft::WRL::ComPtr;

namespace uxplay
{
    namespace
    {
        // DXVA decoder profiles (values from dxva2api.h / d3d11.h); defined
        // locally so no GUID import library is needed.
        constexpr GUID profile_h264_vld_nofgt = {0x1b81be68, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
        constexpr GUID profile_hevc_vld_main = {0x5b11d51b, 0x2f4c, 0x4452, {0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0}};
        constexpr GUID profile_hevc_vld_main10 = {0x107af0e0, 0xef1a, 0x4d19, {0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13}};

        bool decoder_supports(ID3D11VideoDevice *video_device, GUID const &profile, DXGI_FORMAT format)
        {
            UINT const count = video_device->GetVideoDecoderProfileCount();
            for (UINT index = 0; index < count; ++index)
            {
                GUID candidate{};
                if (SUCCEEDED(video_device->GetVideoDecoderProfile(index, &candidate)) &&
                    IsEqualGUID(candidate, profile))
                {
                    BOOL supported = FALSE;
                    return SUCCEEDED(video_device->CheckVideoDecoderFormat(&profile, format, &supported)) &&
                           supported == TRUE;
                }
            }
            return false;
        }

        std::wstring primary_monitor_device_name()
        {
            HMONITOR const monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            GetMonitorInfoW(monitor, &info);
            return info.szDevice;
        }

        // Adapter LUID of the GPU that owns the display path of the primary
        // monitor, from the physical display configuration (not DXGI's
        // per-process view, which hybrid GPU routing rewrites).
        std::optional<LUID> display_owner_luid()
        {
            std::wstring const device_name = primary_monitor_device_name();
            UINT32 path_count = 0;
            UINT32 mode_count = 0;
            if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS)
            {
                return std::nullopt;
            }
            std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(),
                                   nullptr) != ERROR_SUCCESS)
            {
                return std::nullopt;
            }
            for (UINT32 index = 0; index < path_count; ++index)
            {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name{};
                source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                source_name.header.size = sizeof(source_name);
                source_name.header.adapterId = paths[index].sourceInfo.adapterId;
                source_name.header.id = paths[index].sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&source_name.header) == ERROR_SUCCESS &&
                    std::wstring_view{source_name.viewGdiDeviceName} == device_name)
                {
                    return paths[index].sourceInfo.adapterId;
                }
            }
            return std::nullopt;
        }

        ComPtr<IDXGIAdapter1> select_adapter(IDXGIFactory6 *factory, gpu_preference preference)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (preference == gpu_preference::automatic)
            {
                if (auto const owner = display_owner_luid(); owner)
                {
                    if (SUCCEEDED(factory->EnumAdapterByLuid(*owner, IID_PPV_ARGS(&adapter))))
                    {
                        return adapter;
                    }
                }
                preference = gpu_preference::integrated;
            }
            DXGI_GPU_PREFERENCE const dxgi_preference = preference == gpu_preference::discrete
                                                            ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
                                                            : DXGI_GPU_PREFERENCE_MINIMUM_POWER;
            if (SUCCEEDED(factory->EnumAdapterByGpuPreference(0, dxgi_preference, IID_PPV_ARGS(&adapter))))
            {
                return adapter;
            }
            factory->EnumAdapters1(0, &adapter);
            return adapter;
        }

        char const *preference_name(gpu_preference preference)
        {
            switch (preference)
            {
            case gpu_preference::integrated:
                return "integrated (forced)";
            case gpu_preference::discrete:
                return "discrete (forced)";
            case gpu_preference::software:
                return "software (WARP, forced)";
            case gpu_preference::automatic:
                break;
            }
            return "display owner";
        }
    }

    double display_mode_info::refresh_hz() const
    {
        if (refresh_denominator == 0 || refresh_numerator == 0)
        {
            return 60.0;
        }
        return static_cast<double>(refresh_numerator) / static_cast<double>(refresh_denominator);
    }

    std::int64_t display_mode_info::refresh_period_ns() const
    {
        if (refresh_numerator == 0)
        {
            return 16'666'667;
        }
        return static_cast<std::int64_t>((1'000'000'000LL * static_cast<std::int64_t>(refresh_denominator)) /
                                         static_cast<std::int64_t>(refresh_numerator));
    }

    struct gpu_device::state
    {
        ComPtr<IDXGIFactory6> factory;
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        gpu_adapter_info info;
        bool tearing = false;
        std::recursive_mutex mutex;
        // until the presenter says otherwise: a queue of 8 plus 3 beside it
        std::atomic<unsigned int> presentation_hold{11};
    };

    gpu_device::gpu_device(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    gpu_device::~gpu_device() = default;

    outcome<std::unique_ptr<gpu_device>> gpu_device::create(gpu_preference preference)
    {
        auto created = std::make_unique<state>();
        HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&created->factory));
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("CreateDXGIFactory2", result));
        }
        D3D_FEATURE_LEVEL const levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        UINT const flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        bool software = preference == gpu_preference::software;

        // Prefer a hardware adapter; fall back to the WARP software renderer so
        // the app still runs on a PC with no usable D3D11 GPU/driver.
        if (!software)
        {
            created->adapter = select_adapter(created->factory.Get(), preference);
            if (created->adapter)
            {
                result = D3D11CreateDevice(created->adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                           static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &created->device,
                                           nullptr, &created->context);
                if (FAILED(result))
                {
                    log::warning("hardware D3D11 device failed ({}); falling back to the software renderer (WARP)",
                                 make_hresult_failure("D3D11CreateDevice", result).describe());
                    created->adapter.Reset();
                    created->device.Reset();
                    created->context.Reset();
                    software = true;
                }
            }
            else
            {
                log::warning("no hardware GPU adapter found; using the software renderer (WARP)");
                software = true;
            }
        }

        if (software)
        {
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                                       static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &created->device,
                                       nullptr, &created->context);
            if (FAILED(result))
            {
                // some WARP builds reject the video flag: retry without it
                result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                           &created->device, nullptr, &created->context);
            }
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure("D3D11CreateDevice (WARP)", result));
            }
        }

        if (created->adapter)
        {
            DXGI_ADAPTER_DESC1 description{};
            created->adapter->GetDesc1(&description);
            created->info.description = utf16_to_utf8(description.Description);
            created->info.vendor_id = description.VendorId;
            created->info.device_id = description.DeviceId;
            created->info.dedicated_video_memory = description.DedicatedVideoMemory;
            for (ComPtr<IDXGIOutput> output; created->adapter->EnumOutputs(created->info.output_count, &output) !=
                                             DXGI_ERROR_NOT_FOUND;
                 output.Reset())
            {
                ++created->info.output_count;
            }
        }
        else
        {
            created->info.description = "Microsoft Basic Render Driver (WARP software)";
        }

        // FFmpeg's D3D11VA decoder and the presenter run on different threads.
        ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(created->device.As(&multithread)))
        {
            multithread->SetMultithreadProtected(TRUE);
        }

        ComPtr<ID3D11VideoDevice> video_device;
        if (SUCCEEDED(created->device.As(&video_device)))
        {
            created->info.h264_decode = decoder_supports(video_device.Get(), profile_h264_vld_nofgt, DXGI_FORMAT_NV12);
            created->info.hevc_main_decode =
                decoder_supports(video_device.Get(), profile_hevc_vld_main, DXGI_FORMAT_NV12);
            created->info.hevc_main10_decode =
                decoder_supports(video_device.Get(), profile_hevc_vld_main10, DXGI_FORMAT_P010);
        }

        BOOL allow_tearing = FALSE;
        if (SUCCEEDED(created->factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
                                                            sizeof(allow_tearing))))
        {
            created->tearing = allow_tearing == TRUE;
        }

        log::info("GPU ({}): {} (vendor {:04x}, {} MiB) decode: H.264 {}, HEVC Main {}, HEVC Main10 {}",
                  preference_name(preference), created->info.description, created->info.vendor_id,
                  created->info.dedicated_video_memory / (1024 * 1024), created->info.h264_decode ? "hw" : "sw",
                  created->info.hevc_main_decode ? "hw" : "sw", created->info.hevc_main10_decode ? "hw" : "sw");
        return std::unique_ptr<gpu_device>{new gpu_device{std::move(created)}};
    }

    ID3D11Device *gpu_device::device() const
    {
        return impl->device.Get();
    }

    ID3D11DeviceContext *gpu_device::context() const
    {
        return impl->context.Get();
    }

    IDXGIFactory2 *gpu_device::factory() const
    {
        return impl->factory.Get();
    }

    gpu_adapter_info const &gpu_device::adapter_info() const
    {
        return impl->info;
    }

    bool gpu_device::tearing_supported() const
    {
        return impl->tearing;
    }

    std::recursive_mutex &gpu_device::context_mutex()
    {
        return impl->mutex;
    }

    std::optional<std::string> gpu_device::device_removed_reason() const
    {
        HRESULT const reason = impl->device->GetDeviceRemovedReason();
        if (reason == S_OK)
        {
            return std::nullopt;
        }
        return make_hresult_failure("D3D11 device (GetDeviceRemovedReason)", reason).describe();
    }

    void gpu_device::set_presentation_hold(unsigned int pictures)
    {
        impl->presentation_hold.store(std::max(pictures, 1U), std::memory_order_relaxed);
    }

    unsigned int gpu_device::presentation_hold() const
    {
        return impl->presentation_hold.load(std::memory_order_relaxed);
    }

    outcome<display_mode_info> gpu_device::primary_display_mode()
    {
        display_mode_info mode{};
        HMONITOR const monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFOEXW monitor_info{};
        monitor_info.cbSize = sizeof(monitor_info);
        if (GetMonitorInfoW(monitor, &monitor_info) == 0)
        {
            return std::unexpected(make_win32_failure("GetMonitorInfoW", GetLastError()));
        }
        mode.gdi_device_name = monitor_info.szDevice;
        mode.desktop_left = monitor_info.rcMonitor.left;
        mode.desktop_top = monitor_info.rcMonitor.top;
        mode.width = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
        mode.height = monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;

        // DWM composition rate: the clock every windowed/independent flip
        // present is paced by (in clone mode it follows one of the panels)
        DWM_TIMING_INFO timing{};
        timing.cbSize = sizeof(timing);
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.rateRefresh.uiNumerator != 0)
        {
            mode.refresh_numerator = timing.rateRefresh.uiNumerator;
            mode.refresh_denominator = timing.rateRefresh.uiDenominator;
        }

        UINT32 path_count = 0;
        UINT32 mode_count = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) == ERROR_SUCCESS)
        {
            std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(),
                                   nullptr) == ERROR_SUCCESS)
            {
                for (UINT32 index = 0; index < path_count; ++index)
                {
                    DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name{};
                    source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                    source_name.header.size = sizeof(source_name);
                    source_name.header.adapterId = paths[index].sourceInfo.adapterId;
                    source_name.header.id = paths[index].sourceInfo.id;
                    if (DisplayConfigGetDeviceInfo(&source_name.header) != ERROR_SUCCESS ||
                        std::wstring_view{source_name.viewGdiDeviceName} != std::wstring_view{mode.gdi_device_name})
                    {
                        continue;
                    }
                    DISPLAYCONFIG_TARGET_DEVICE_NAME target_name{};
                    target_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                    target_name.header.size = sizeof(target_name);
                    target_name.header.adapterId = paths[index].targetInfo.adapterId;
                    target_name.header.id = paths[index].targetInfo.id;
                    DisplayConfigGetDeviceInfo(&target_name.header);
                    DISPLAYCONFIG_RATIONAL const rate = paths[index].targetInfo.refreshRate;
                    display_target_info target{};
                    target.monitor_name = utf16_to_utf8(target_name.monitorFriendlyDeviceName);
                    target.internal_panel =
                        paths[index].targetInfo.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL;
                    target.refresh_hz = rate.Denominator != 0 ? static_cast<double>(rate.Numerator) / rate.Denominator
                                                              : 0.0;
                    if (target.monitor_name.empty())
                    {
                        target.monitor_name = target.internal_panel ? "built-in panel" : "monitor";
                    }
                    mode.targets.push_back(std::move(target));
                    if (mode.refresh_numerator == 0 && rate.Numerator != 0)
                    {
                        mode.refresh_numerator = rate.Numerator;
                        mode.refresh_denominator = rate.Denominator;
                    }
                }
            }
        }
        return mode;
    }
}
