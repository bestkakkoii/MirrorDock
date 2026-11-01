// D3D11 device bound to the GPU that scans out the monitor.
//
// On this Optimus laptop the monitor is driven by the Intel HD Graphics 530
// while the GTX 960M has no outputs.  Decoding and presenting on the adapter
// that owns the output keeps decoded NV12 surfaces zero-copy all the way to
// the swap chain (no cross-adapter copies over PCIe) and lets DXGI use
// independent flip for the borderless fullscreen window.
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

export module uxplay_gpu_device;

import std;
import uxplay_status;

export namespace uxplay
{
    // automatic: the adapter that physically scans out the desktop monitor
    // (on this laptop the Intel HD 530, even when the NVIDIA "high
    // performance" preference makes DXGI report the outputs on the GTX).
    enum class gpu_preference
    {
        automatic,
        integrated,
        discrete
    };

    struct gpu_adapter_info
    {
        std::string description;
        unsigned int vendor_id = 0;
        unsigned int device_id = 0;
        std::uint64_t dedicated_video_memory = 0;
        unsigned int output_count = 0;
        bool h264_decode = false;
        bool hevc_main_decode = false;
        bool hevc_main10_decode = false;
    };

    struct display_target_info
    {
        std::string monitor_name;
        bool internal_panel = false;
        double refresh_hz = 0.0;
    };

    struct display_mode_info
    {
        int width = 0;
        int height = 0;
        int desktop_left = 0;
        int desktop_top = 0;
        std::wstring gdi_device_name;
        // composition clock of DWM: presents are paced by it
        std::uint32_t refresh_numerator = 0;
        std::uint32_t refresh_denominator = 1;
        // physical monitors showing this desktop source (clone mode: several)
        std::vector<display_target_info> targets;

        [[nodiscard]] double refresh_hz() const;
        [[nodiscard]] std::int64_t refresh_period_ns() const;
    };

    class gpu_device
    {
    public:
        static outcome<std::unique_ptr<gpu_device>> create(gpu_preference preference);

        ~gpu_device();
        gpu_device(gpu_device const &) = delete;
        gpu_device &operator=(gpu_device const &) = delete;

        [[nodiscard]] ID3D11Device *device() const;
        [[nodiscard]] ID3D11DeviceContext *context() const;
        [[nodiscard]] IDXGIFactory2 *factory() const;
        [[nodiscard]] gpu_adapter_info const &adapter_info() const;
        [[nodiscard]] bool tearing_supported() const;

        // Guards every use of the immediate context, shared with FFmpeg's
        // D3D11VA lock callbacks so decode and present never interleave.
        [[nodiscard]] std::recursive_mutex &context_mutex();

        // Primary desktop monitor: geometry, DWM composition rate, targets.
        [[nodiscard]] static outcome<display_mode_info> primary_display_mode();

    private:
        struct state;

        explicit gpu_device(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
