// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "video_present_vs.h"
#include "video_present_ps_nv12.h"
#include "video_present_ps_i420.h"

module uxplay_video_presenter;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_clock;
import uxplay_gpu_device;
import uxplay_video_frame;
import uxplay_media_types;
import uxplay_system_tuning;

using Microsoft::WRL::ComPtr;

namespace uxplay
{
    namespace
    {
        // Pictures alive beside the queue at any moment: the displayed one,
        // one on its way in from the decoder and one being replaced or evicted.
        constexpr unsigned int pictures_beside_queue = 3;
        constexpr std::int64_t error_report_interval_ns = 10 * clock::ns_per_second;
        constexpr std::size_t most_interval_samples = 4096;
        constexpr std::int64_t no_deadline = std::numeric_limits<std::int64_t>::max();
        // longest wait for the previous picture to be flipped (a hidden window never flips)
        constexpr DWORD flip_wait_ms = 100;

        struct presentation_policy
        {
            std::size_t queue_limit;
            // in-order presentation: a due picture that is more than this many
            // refresh periods late gives way to a successor that is due too
            int late_periods;
            bool newest_due_wins;
            bool bilinear_only;
        };

        constexpr presentation_policy policy_for(performance_mode mode)
        {
            switch (mode)
            {
            case performance_mode::quality:
                return presentation_policy{13, 2, false, false};
            case performance_mode::performance:
                // the same queue as balanced: an iPhone's pictures arrive
                // about 85 ms before their timestamps, i.e. 5 to 7 of them wait
                // at 60 fps whatever the mode (a queue of 5 evicted them unseen)
                return presentation_policy{8, 0, true, true};
            case performance_mode::balanced:
                break;
            }
            return presentation_policy{8, 1, false, false};
        }

        constexpr unsigned int presentation_hold_for(performance_mode mode)
        {
            return static_cast<unsigned int>(policy_for(mode).queue_limit) + pictures_beside_queue;
        }

        // a crop that leaves less than this in either direction is not applied
        constexpr int smallest_crop = 16;

        // layout of cbuffer present_constants in video_present.hlsl
        struct alignas(16) present_constants
        {
            float color_row0[4];
            float color_row1[4];
            float color_row2[4];
            float luma_size[2];
            float visible_scale[2];
            float visible_origin[2];
            float filter_mode;
            float padding;
            float luma_bounds[4];
            float chroma_bounds[4];
        };
        static_assert(sizeof(present_constants) == 7 * 16);

        struct pending_frame
        {
            std::unique_ptr<decoded_frame> frame;
            std::int64_t present_time_ns = 0;
        };

        struct source_slot
        {
            std::deque<pending_frame> queue;
            // shared with the render thread while it draws the picture
            std::shared_ptr<decoded_frame> current;
            bool active = false;
            // The part of the pictures to show (empty: all of it).  It belongs
            // to pictures of crop_width x crop_height; 0 x 0 means it was asked
            // for before any picture existed and is checked against the first.
            picture_region crop;
            int crop_width = 0;
            int crop_height = 0;

            void drop_crop()
            {
                crop = {};
                crop_width = 0;
                crop_height = 0;
            }
        };

        // `wanted` applied to a width x height picture: clamped to the picture,
        // edges moved inwards to even positions (4:2:0 chroma).  An empty
        // result means the whole picture; no result means that less than
        // smallest_crop would remain in one direction.
        std::optional<picture_region> fit_region(picture_region const &wanted, int width, int height)
        {
            if (wanted.empty())
            {
                return picture_region{};
            }
            std::int64_t const wanted_right = std::int64_t{wanted.left} + wanted.width;
            std::int64_t const wanted_bottom = std::int64_t{wanted.top} + wanted.height;
            int left = std::clamp(wanted.left, 0, width);
            int top = std::clamp(wanted.top, 0, height);
            int right = static_cast<int>(std::clamp<std::int64_t>(wanted_right, left, width));
            int bottom = static_cast<int>(std::clamp<std::int64_t>(wanted_bottom, top, height));
            if (left == 0 && top == 0 && right == width && bottom == height)
            {
                return picture_region{};
            }
            left += left & 1;
            top += top & 1;
            right -= right & 1;
            bottom -= bottom & 1;
            if (right - left < smallest_crop || bottom - top < smallest_crop)
            {
                return std::nullopt;
            }
            return picture_region{left, top, right - left, bottom - top};
        }

        std::string crop_complaint(picture_region const &wanted, int width, int height)
        {
            return std::format("crop {}x{} at ({}, {}) leaves less than {}x{} of a {}x{} picture: the whole picture "
                               "is shown",
                               wanted.width, wanted.height, wanted.left, wanted.top, smallest_crop, smallest_crop,
                               width, height);
        }

        struct owned_handle
        {
            HANDLE value = nullptr;

            owned_handle() = default;
            owned_handle(owned_handle const &) = delete;
            owned_handle &operator=(owned_handle const &) = delete;

            ~owned_handle()
            {
                reset();
            }

            void reset(HANDLE replacement = nullptr)
            {
                if (value != nullptr)
                {
                    CloseHandle(value);
                }
                value = replacement;
            }
        };

        // YCbCr (8-bit code values normalized to [0,1]) to RGB.
        void fill_color_matrix(present_constants &constants, frame_color_matrix matrix, frame_color_range range)
        {
            double kr = 0.2126;
            double kb = 0.0722;
            if (matrix == frame_color_matrix::bt601)
            {
                kr = 0.299;
                kb = 0.114;
            }
            else if (matrix == frame_color_matrix::bt2020)
            {
                kr = 0.2627;
                kb = 0.0593;
            }
            double const kg = 1.0 - kr - kb;
            bool const full = range == frame_color_range::full;
            double const luma_scale = full ? 1.0 : 255.0 / 219.0;
            double const luma_offset = full ? 0.0 : 16.0 / 255.0;
            double const chroma_scale = full ? 1.0 : 255.0 / 224.0;
            double const chroma_offset = 128.0 / 255.0;

            double const coefficients[3][3] = {
                {1.0, 0.0, 2.0 * (1.0 - kr)},
                {1.0, -2.0 * kb * (1.0 - kb) / kg, -2.0 * kr * (1.0 - kr) / kg},
                {1.0, 2.0 * (1.0 - kb), 0.0},
            };
            float *rows[3] = {constants.color_row0, constants.color_row1, constants.color_row2};
            for (int row = 0; row < 3; ++row)
            {
                double const y = coefficients[row][0] * luma_scale;
                double const cb = coefficients[row][1] * chroma_scale;
                double const cr = coefficients[row][2] * chroma_scale;
                rows[row][0] = static_cast<float>(y);
                rows[row][1] = static_cast<float>(cb);
                rows[row][2] = static_cast<float>(cr);
                rows[row][3] = static_cast<float>(-(y * luma_offset + cb * chroma_offset + cr * chroma_offset));
            }
        }

        struct plane_views
        {
            ComPtr<ID3D11ShaderResourceView> luma;
            ComPtr<ID3D11ShaderResourceView> chroma;
            ComPtr<ID3D11ShaderResourceView> chroma_second;
        };

        outcome<ComPtr<ID3D11ShaderResourceView>> make_slice_view(ID3D11Device *device, ID3D11Texture2D *texture,
                                                                  DXGI_FORMAT format, UINT slice)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC description{};
            description.Format = format;
            description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            description.Texture2DArray.MostDetailedMip = 0;
            description.Texture2DArray.MipLevels = 1;
            description.Texture2DArray.FirstArraySlice = slice;
            description.Texture2DArray.ArraySize = 1;
            ComPtr<ID3D11ShaderResourceView> view;
            HRESULT const result = device->CreateShaderResourceView(texture, &description, &view);
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure("CreateShaderResourceView", result));
            }
            return view;
        }
    }

    struct video_presenter::state
    {
        gpu_device *gpu = nullptr;
        HWND window = nullptr;
        std::atomic<std::int64_t> refresh_period{16'666'667};
        std::atomic<performance_mode> mode{performance_mode::balanced};
        std::atomic<bool> device_lost{false};
        display_change_listener listener;
        // auto-reset: something the render thread waits for has changed
        owned_handle wake_event;

        // GPU objects (render thread only, under gpu->context_mutex())
        ComPtr<IDXGISwapChain2> swap_chain;
        owned_handle frame_latency_waitable;
        ComPtr<ID3D11VertexShader> vertex_shader;
        ComPtr<ID3D11PixelShader> pixel_shader_nv12;
        ComPtr<ID3D11PixelShader> pixel_shader_i420;
        ComPtr<ID3D11SamplerState> sampler;
        ComPtr<ID3D11Buffer> constant_buffer;
        ComPtr<ID3D11RenderTargetView> back_buffer_view;
        int swap_width = 0;
        int swap_height = 0;

        // per stream: hardware frame views cached per slice of one decoder texture array
        ID3D11Texture2D *cached_texture = nullptr;
        std::map<UINT, plane_views> slice_views;
        // per stream: copy target when decoder textures are not shader-readable
        ComPtr<ID3D11Texture2D> copy_texture;
        plane_views copy_views;
        // per stream: upload targets for software-decoded frames
        std::array<ComPtr<ID3D11Texture2D>, 3> upload_textures;
        plane_views upload_views;
        int upload_width = 0;
        int upload_height = 0;
        frame_memory upload_kind = frame_memory::cpu_yuv420p;

        // render thread only
        std::optional<presented_stream> announced;
        bool occluded = false; // the last Present reported an invisible window
        clock::interval_gate render_error_gate{error_report_interval_ns};
        std::string last_render_error;

        // shared state (guarded by mutex; never held during GPU work)
        mutable std::mutex mutex;
        std::array<source_slot, media_source_count> slots;
        bool redraw_requested = false;
        bool release_requested = false; // free the per-stream GPU objects once nothing is displayed
        int requested_width = 0;
        int requested_height = 0;
        bool stopping = false;
        presenter_statistics stats;
        std::vector<std::int64_t> interval_lateness;
        std::size_t interval_peak_depth = 0;
        std::int64_t interval_wake_overshoot = 0;
        int interval_stream_width = 0;
        int interval_stream_height = 0;
        // a screenshot someone waits for (capture()); the render thread takes it
        std::shared_ptr<std::promise<outcome<captured_picture>>> capture_request;

        std::thread render_thread;

        void wake() const;
        status create_pipeline();
        status ensure_swap_chain(int width, int height);
        void release_stream_objects();
        outcome<plane_views const *> views_for(decoded_frame const &frame);
        status upload(decoded_frame const &frame);
        status draw(decoded_frame const &frame, picture_region const &visible, bool bilinear_only);
        status draw_into(decoded_frame const &frame, picture_region const &visible, bool bilinear_only,
                         ID3D11RenderTargetView *destination, int destination_width, int destination_height);
        outcome<captured_picture> read_back(decoded_frame const &frame, picture_region const &visible);
        void report_render_failure(failure const &problem);
        void render_loop();
        [[nodiscard]] std::optional<media_source> displayed_source() const;
    };

    void video_presenter::state::wake() const
    {
        SetEvent(wake_event.value);
    }

    status video_presenter::state::create_pipeline()
    {
        ID3D11Device *device = gpu->device();
        HRESULT result = device->CreateVertexShader(shader_vs_fullscreen, sizeof(shader_vs_fullscreen), nullptr,
                                                    &vertex_shader);
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("CreateVertexShader", result));
        }
        result = device->CreatePixelShader(shader_ps_nv12, sizeof(shader_ps_nv12), nullptr, &pixel_shader_nv12);
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("CreatePixelShader nv12", result));
        }
        result = device->CreatePixelShader(shader_ps_i420, sizeof(shader_ps_i420), nullptr, &pixel_shader_i420);
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("CreatePixelShader i420", result));
        }
        D3D11_SAMPLER_DESC sampler_description{};
        sampler_description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
        result = device->CreateSamplerState(&sampler_description, &sampler);
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("CreateSamplerState", result));
        }
        D3D11_BUFFER_DESC buffer_description{};
        buffer_description.ByteWidth = sizeof(present_constants);
        buffer_description.Usage = D3D11_USAGE_DYNAMIC;
        buffer_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        result = device->CreateBuffer(&buffer_description, nullptr, &constant_buffer);
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("CreateBuffer constants", result));
        }
        return {};
    }

    status video_presenter::state::ensure_swap_chain(int width, int height)
    {
        width = std::max(width, 1);
        height = std::max(height, 1);
        if (swap_chain && width == swap_width && height == swap_height)
        {
            return {};
        }
        // nothing may refer to the back buffers while they are replaced
        gpu->context()->OMSetRenderTargets(0, nullptr, nullptr);
        back_buffer_view.Reset();
        if (!swap_chain)
        {
            DXGI_SWAP_CHAIN_DESC1 description{};
            description.Width = static_cast<UINT>(width);
            description.Height = static_cast<UINT>(height);
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = 2;
            description.Scaling = DXGI_SCALING_NONE;
            description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            ComPtr<IDXGISwapChain1> created;
            HRESULT result =
                gpu->factory()->CreateSwapChainForHwnd(gpu->device(), window, &description, nullptr, nullptr, &created);
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure("CreateSwapChainForHwnd", result));
            }
            // Alt+Enter is handled by the window (borderless toggle), never DXGI.
            result = gpu->factory()->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
            if (FAILED(result))
            {
                log::warning("{}", make_hresult_failure("IDXGIFactory::MakeWindowAssociation", result).describe());
            }
            ComPtr<IDXGISwapChain2> waitable_chain;
            result = created.As(&waitable_chain);
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure("IDXGISwapChain2 query", result));
            }
            result = waitable_chain->SetMaximumFrameLatency(1);
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure("IDXGISwapChain2::SetMaximumFrameLatency", result));
            }
            swap_chain = std::move(waitable_chain);
            frame_latency_waitable.reset(swap_chain->GetFrameLatencyWaitableObject());
            occluded = false;
        }
        else
        {
            HRESULT const result = swap_chain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                                             DXGI_FORMAT_UNKNOWN,
                                                             DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure("IDXGISwapChain::ResizeBuffers", result));
            }
        }
        swap_width = width;
        swap_height = height;
        return {};
    }

    // Everything that exists only while a stream is shown.  The views are the
    // last presenter objects that refer to decoder textures.
    void video_presenter::state::release_stream_objects()
    {
        bool const usable = !device_lost.load(std::memory_order_relaxed);
        if (usable)
        {
            // drop every binding (draw() sets the whole pipeline state anew):
            // a bound back buffer would keep the swap chain alive
            gpu->context()->ClearState();
        }
        slice_views.clear();
        cached_texture = nullptr;
        copy_views = {};
        copy_texture.Reset();
        upload_views = {};
        for (auto &texture : upload_textures)
        {
            texture.Reset();
        }
        upload_width = 0;
        upload_height = 0;
        back_buffer_view.Reset();
        frame_latency_waitable.reset();
        swap_chain.Reset();
        swap_width = 0;
        swap_height = 0;
        occluded = false;
        if (usable)
        {
            // D3D11 destroys released objects lazily and nothing else flushes
            // while no stream is shown; the window also cannot get its next
            // flip-model swap chain before the old one is really gone
            gpu->context()->Flush();
        }
    }

    status video_presenter::state::upload(decoded_frame const &frame)
    {
        ID3D11Device *device = gpu->device();
        ID3D11DeviceContext *context = gpu->context();
        bool const nv12 = frame.memory() == frame_memory::cpu_nv12;
        int const width = frame.width();
        int const height = frame.height();
        int const plane_count = nv12 ? 2 : 3;
        if (width != upload_width || height != upload_height || frame.memory() != upload_kind || !upload_textures[0])
        {
            upload_views = {};
            for (auto &texture : upload_textures)
            {
                texture.Reset();
            }
            upload_width = 0;
            upload_height = 0;
            for (int plane = 0; plane < plane_count; ++plane)
            {
                bool const chroma = plane > 0;
                D3D11_TEXTURE2D_DESC description{};
                description.Width = static_cast<UINT>(chroma ? (width + 1) / 2 : width);
                description.Height = static_cast<UINT>(chroma ? (height + 1) / 2 : height);
                description.MipLevels = 1;
                description.ArraySize = 1;
                description.Format = (nv12 && chroma) ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
                description.SampleDesc.Count = 1;
                description.Usage = D3D11_USAGE_DYNAMIC;
                description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                auto &target = upload_textures[static_cast<std::size_t>(plane)];
                HRESULT const result = device->CreateTexture2D(&description, nullptr, &target);
                if (FAILED(result))
                {
                    return std::unexpected(make_hresult_failure("CreateTexture2D upload plane", result));
                }
                auto view = make_slice_view(device, target.Get(), description.Format, 0);
                if (!view)
                {
                    return std::unexpected(view.error());
                }
                (plane == 0 ? upload_views.luma : plane == 1 ? upload_views.chroma : upload_views.chroma_second) =
                    std::move(*view);
            }
            upload_width = width;
            upload_height = height;
            upload_kind = frame.memory();
        }
        for (int plane = 0; plane < plane_count; ++plane)
        {
            bool const chroma = plane > 0;
            int const rows = chroma ? (height + 1) / 2 : height;
            int const row_bytes = chroma ? (nv12 ? ((width + 1) / 2) * 2 : (width + 1) / 2) : width;
            ID3D11Texture2D *const target = upload_textures[static_cast<std::size_t>(plane)].Get();
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT const result = context->Map(target, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure("ID3D11DeviceContext::Map upload", result));
            }
            auto *destination = static_cast<std::uint8_t *>(mapped.pData);
            std::uint8_t const *source = frame.plane(plane);
            for (int row = 0; row < rows; ++row)
            {
                std::memcpy(destination + static_cast<std::size_t>(row) * mapped.RowPitch,
                            source + static_cast<std::ptrdiff_t>(row) * frame.stride(plane),
                            static_cast<std::size_t>(row_bytes));
            }
            context->Unmap(target, 0);
        }
        return {};
    }

    outcome<plane_views const *> video_presenter::state::views_for(decoded_frame const &frame)
    {
        if (frame.memory() != frame_memory::d3d11_nv12)
        {
            status const uploaded = upload(frame);
            if (!uploaded)
            {
                return std::unexpected(uploaded.error());
            }
            return &upload_views;
        }

        ID3D11Device *device = gpu->device();
        ID3D11Texture2D *texture = frame.texture();
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        // 10-bit (HDR) streams decode to P010: 16-bit containers, same layout
        bool const ten_bit = description.Format == DXGI_FORMAT_P010;
        DXGI_FORMAT const luma_format = ten_bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        DXGI_FORMAT const chroma_format = ten_bit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
        if ((description.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0)
        {
            // decoder-only texture array: copy the slice into a sampleable texture
            D3D11_TEXTURE2D_DESC copy_description{};
            if (copy_texture)
            {
                copy_texture->GetDesc(&copy_description);
            }
            if (!copy_texture || copy_description.Width != description.Width ||
                copy_description.Height != description.Height || copy_description.Format != description.Format)
            {
                copy_description = description;
                copy_description.ArraySize = 1;
                copy_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                copy_description.MiscFlags = 0;
                copy_description.Usage = D3D11_USAGE_DEFAULT;
                copy_views = {};
                copy_texture.Reset();
                HRESULT const result = device->CreateTexture2D(&copy_description, nullptr, &copy_texture);
                if (FAILED(result))
                {
                    return std::unexpected(make_hresult_failure("CreateTexture2D copy target", result));
                }
                auto luma = make_slice_view(device, copy_texture.Get(), luma_format, 0);
                auto chroma = make_slice_view(device, copy_texture.Get(), chroma_format, 0);
                if (!luma || !chroma)
                {
                    copy_texture.Reset();
                    return std::unexpected(!luma ? luma.error() : chroma.error());
                }
                copy_views = plane_views{std::move(*luma), std::move(*chroma), nullptr};
            }
            gpu->context()->CopySubresourceRegion(copy_texture.Get(), 0, 0, 0, 0, texture,
                                                  D3D11CalcSubresource(0, frame.array_index(), description.MipLevels),
                                                  nullptr);
            return &copy_views;
        }

        if (texture != cached_texture)
        {
            // a new decoder pool (stream start, rotation): the old views go
            slice_views.clear();
            cached_texture = texture;
        }
        UINT const slice = frame.array_index();
        if (auto const found = slice_views.find(slice); found != slice_views.end())
        {
            return &found->second;
        }
        auto luma = make_slice_view(device, texture, luma_format, slice);
        auto chroma = make_slice_view(device, texture, chroma_format, slice);
        if (!luma || !chroma)
        {
            return std::unexpected(!luma ? luma.error() : chroma.error());
        }
        auto const inserted = slice_views.emplace(slice, plane_views{std::move(*luma), std::move(*chroma), nullptr});
        return &inserted.first->second;
    }

    // Draws the part `visible` of the picture (never empty, inside the picture)
    // letterboxed into the client area.  Decoder slices, the copy texture and
    // software uploads all go through here: `visible` is in picture pixels,
    // which are the texels of each of those textures from their top-left corner.
    status video_presenter::state::draw(decoded_frame const &frame, picture_region const &visible,
                                        bool bilinear_only)
    {
        if (!back_buffer_view)
        {
            ComPtr<ID3D11Texture2D> back_buffer;
            HRESULT result = swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure("IDXGISwapChain::GetBuffer", result));
            }
            result = gpu->device()->CreateRenderTargetView(back_buffer.Get(), nullptr, &back_buffer_view);
            if (FAILED(result) || !back_buffer_view)
            {
                back_buffer_view.Reset();
                return std::unexpected(make_hresult_failure("CreateRenderTargetView", result));
            }
        }
        return draw_into(frame, visible, bilinear_only, back_buffer_view.Get(), swap_width, swap_height);
    }

    // The same into any render target of destination_width x
    // destination_height: the back buffer, or the texture a screenshot is
    // read from.
    status video_presenter::state::draw_into(decoded_frame const &frame, picture_region const &visible,
                                             bool bilinear_only, ID3D11RenderTargetView *destination,
                                             int destination_width, int destination_height)
    {
        ID3D11DeviceContext *context = gpu->context();
        auto views = views_for(frame);
        if (!views)
        {
            return std::unexpected(views.error());
        }

        ComPtr<ID3D11Resource> luma_resource;
        (*views)->luma->GetResource(&luma_resource);
        ComPtr<ID3D11Texture2D> luma_texture;
        if (HRESULT const result = luma_resource.As(&luma_texture); FAILED(result))
        {
            return std::unexpected(make_hresult_failure("luma plane texture query", result));
        }
        D3D11_TEXTURE2D_DESC texture_description{};
        luma_texture->GetDesc(&texture_description);

        // aspect-correct letterbox of the visible part inside the destination
        // (the client area): where that has the part's aspect ratio it touches
        // all four edges
        double const scale = std::min(static_cast<double>(destination_width) / visible.width,
                                      static_cast<double>(destination_height) / visible.height);
        double const view_width = visible.width * scale;
        double const view_height = visible.height * scale;

        present_constants constants{};
        fill_color_matrix(constants, frame.color_matrix(), frame.color_range());
        double const texture_width = texture_description.Width;
        double const texture_height = texture_description.Height;
        constants.luma_size[0] = static_cast<float>(texture_width);
        constants.luma_size[1] = static_cast<float>(texture_height);
        constants.visible_origin[0] = static_cast<float>(visible.left / texture_width);
        constants.visible_origin[1] = static_cast<float>(visible.top / texture_height);
        constants.visible_scale[0] = static_cast<float>(visible.width / texture_width);
        constants.visible_scale[1] = static_cast<float>(visible.height / texture_height);
        // 0 bilinear, 1 Catmull-Rom (enlarging), 2 four-tap supersampling (shrinking)
        constants.filter_mode = bilinear_only ? 0.0F : (scale >= 1.02 ? 1.0F : (scale <= 0.98 ? 2.0F : 0.0F));
        // Every fetch is clamped to the centres of the outermost texels of the
        // visible part, so no filter tap reads what lies beside it (the bars
        // that were cut off, or the padding of a decoder surface).
        int const right = visible.left + visible.width;
        int const bottom = visible.top + visible.height;
        constants.luma_bounds[0] = static_cast<float>((visible.left + 0.5) / texture_width);
        constants.luma_bounds[1] = static_cast<float>((visible.top + 0.5) / texture_height);
        constants.luma_bounds[2] = static_cast<float>((right - 0.5) / texture_width);
        constants.luma_bounds[3] = static_cast<float>((bottom - 0.5) / texture_height);
        // chroma planes have half the size (rounded up for odd software pictures)
        double const chroma_width = (texture_description.Width + 1) / 2;
        double const chroma_height = (texture_description.Height + 1) / 2;
        constants.chroma_bounds[0] = static_cast<float>((visible.left / 2 + 0.5) / chroma_width);
        constants.chroma_bounds[1] = static_cast<float>((visible.top / 2 + 0.5) / chroma_height);
        constants.chroma_bounds[2] = static_cast<float>(((right + 1) / 2 - 0.5) / chroma_width);
        constants.chroma_bounds[3] = static_cast<float>(((bottom + 1) / 2 - 0.5) / chroma_height);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (HRESULT const result = context->Map(constant_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            FAILED(result))
        {
            return std::unexpected(make_hresult_failure("ID3D11DeviceContext::Map constants", result));
        }
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(constant_buffer.Get(), 0);

        float const black[4] = {0.0F, 0.0F, 0.0F, 1.0F};
        context->ClearRenderTargetView(destination, black);
        ID3D11RenderTargetView *targets[] = {destination};
        context->OMSetRenderTargets(1, targets, nullptr);
        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = static_cast<float>(std::floor((destination_width - view_width) / 2.0));
        viewport.TopLeftY = static_cast<float>(std::floor((destination_height - view_height) / 2.0));
        viewport.Width = static_cast<float>(std::round(view_width));
        viewport.Height = static_cast<float>(std::round(view_height));
        viewport.MaxDepth = 1.0F;
        context->RSSetViewports(1, &viewport);

        bool const three_planes = frame.memory() == frame_memory::cpu_yuv420p;
        ID3D11ShaderResourceView *resources[3] = {(*views)->luma.Get(), (*views)->chroma.Get(),
                                                  (*views)->chroma_second.Get()};
        ID3D11Buffer *buffers[] = {constant_buffer.Get()};
        ID3D11SamplerState *samplers[] = {sampler.Get()};
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(nullptr);
        context->VSSetShader(vertex_shader.Get(), nullptr, 0);
        context->PSSetShader(three_planes ? pixel_shader_i420.Get() : pixel_shader_nv12.Get(), nullptr, 0);
        context->PSSetShaderResources(0, three_planes ? 3 : 2, resources);
        context->PSSetConstantBuffers(0, 1, buffers);
        context->PSSetSamplers(0, 1, samplers);
        context->Draw(3, 0);
        ID3D11ShaderResourceView *cleared[3] = {nullptr, nullptr, nullptr};
        context->PSSetShaderResources(0, 3, cleared);
        return {};
    }

    // The visible part of the picture, pixel for pixel, read back from the
    // GPU (render thread, GPU lock held).  It goes through the same shader
    // as the screen, at a scale of one: what is saved is what is shown, minus
    // the window's scaling.
    outcome<captured_picture> video_presenter::state::read_back(decoded_frame const &frame,
                                                                picture_region const &visible)
    {
        ID3D11Device *device = gpu->device();
        ID3D11DeviceContext *context = gpu->context();
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(visible.width);
        description.Height = static_cast<UINT>(visible.height);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> drawn;
        if (HRESULT const result = device->CreateTexture2D(&description, nullptr, &drawn); FAILED(result))
        {
            return std::unexpected(make_hresult_failure("CreateTexture2D (screenshot)", result));
        }
        ComPtr<ID3D11RenderTargetView> view;
        if (HRESULT const result = device->CreateRenderTargetView(drawn.Get(), nullptr, &view); FAILED(result) || !view)
        {
            return std::unexpected(make_hresult_failure("CreateRenderTargetView (screenshot)", result));
        }
        status const rendered = draw_into(frame, visible, true, view.Get(), visible.width, visible.height);
        // the texture goes away with this call: nothing may stay bound to it
        context->OMSetRenderTargets(0, nullptr, nullptr);
        if (!rendered)
        {
            return std::unexpected(rendered.error());
        }
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (HRESULT const result = device->CreateTexture2D(&description, nullptr, &staging); FAILED(result))
        {
            return std::unexpected(make_hresult_failure("CreateTexture2D (screenshot copy)", result));
        }
        context->CopyResource(staging.Get(), drawn.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (HRESULT const result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped); FAILED(result))
        {
            return std::unexpected(make_hresult_failure("ID3D11DeviceContext::Map (screenshot)", result));
        }
        captured_picture picture;
        picture.width = visible.width;
        picture.height = visible.height;
        std::size_t const row_bytes = static_cast<std::size_t>(visible.width) * 4;
        picture.pixels.resize(row_bytes * static_cast<std::size_t>(visible.height));
        auto const *source = static_cast<std::uint8_t const *>(mapped.pData);
        for (int row = 0; row < visible.height; ++row)
        {
            std::memcpy(picture.pixels.data() + static_cast<std::size_t>(row) * row_bytes,
                        source + static_cast<std::size_t>(row) * mapped.RowPitch, row_bytes);
        }
        context->Unmap(staging.Get(), 0);
        return picture;
    }

    // A failed GPU call.  Device loss is reported once and ends all GPU work;
    // anything else may repeat every frame, so it is logged the first time
    // and then as a count.
    void video_presenter::state::report_render_failure(failure const &problem)
    {
        if (std::optional<std::string> const reason = gpu->device_removed_reason(); reason)
        {
            if (!device_lost.exchange(true))
            {
                log::error("D3D11 device lost: {}; noticed when {}; video output has stopped", *reason,
                           problem.describe());
            }
            return;
        }
        last_render_error = problem.describe();
        if (std::optional<std::uint64_t> const earlier = render_error_gate.pass(clock::now_ns()); earlier)
        {
            if (*earlier == 0)
            {
                log::error("video presentation: {}", last_render_error);
            }
            else
            {
                log::error("video presentation: {} ({} more failures since the last report)", last_render_error,
                           *earlier);
            }
        }
    }

    std::optional<media_source> video_presenter::state::displayed_source() const
    {
        for (media_source const source : sources_by_priority)
        {
            source_slot const &slot = slots[source_index(source)];
            if (slot.active && (slot.current || !slot.queue.empty()))
            {
                return source;
            }
        }
        // released sources that kept their last frame ("-nc")
        for (media_source const source : sources_by_priority)
        {
            if (slots[source_index(source)].current)
            {
                return source;
            }
        }
        return std::nullopt;
    }

    void video_presenter::state::render_loop()
    {
        system_tuning::mmcss_scope const mmcss{L"Playback"};
        clock::precise_waiter waiter;
        if (!waiter.high_resolution())
        {
            log::warning("video pacing: this Windows version has no high-resolution timers, frame wake-ups follow "
                         "the system timer tick");
        }
        double lateness_average = 0.0;
        std::int64_t wake_overshoot = 0; // worst since it was last added to the interval figures

        for (;;)
        {
            // ---- sleep until a picture is due, a redraw or release is wanted, or we stop
            std::int64_t deadline = no_deadline;
            bool work_pending = false;
            bool displayable = false;
            {
                std::scoped_lock lock{mutex};
                if (stopping)
                {
                    return;
                }
                work_pending = redraw_requested || release_requested;
                if (std::optional<media_source> const shown = displayed_source(); shown)
                {
                    displayable = true;
                    source_slot const &slot = slots[source_index(*shown)];
                    if (!slot.queue.empty())
                    {
                        // recomputed each cycle so a monitor/refresh change takes effect live
                        deadline = slot.queue.front().present_time_ns -
                                   refresh_period.load(std::memory_order_relaxed) / 2;
                    }
                }
            }
            if (!work_pending && deadline > clock::now_ns())
            {
                if (deadline == no_deadline)
                {
                    if (WaitForSingleObject(wake_event.value, INFINITE) == WAIT_FAILED)
                    {
                        report_render_failure(make_win32_failure("WaitForSingleObject (render wake)", GetLastError()));
                        Sleep(10);
                    }
                }
                else if (!waiter.wait_until(wake_event.value, deadline))
                {
                    // the timer ended the wait: how late did the thread get to run?
                    wake_overshoot = std::max(wake_overshoot, clock::now_ns() - deadline);
                }
                continue; // whatever woke us, look at the state again
            }

            // ---- the previous picture must have been flipped before the next is rendered
            if (displayable && frame_latency_waitable.value != nullptr && !device_lost.load(std::memory_order_relaxed))
            {
                WaitForSingleObjectEx(frame_latency_waitable.value, occluded ? DWORD{0} : flip_wait_ms, FALSE);
            }

            // ---- choose what to show (queue lock only)
            presentation_policy const policy = policy_for(mode.load(std::memory_order_relaxed));
            std::shared_ptr<decoded_frame> to_draw;
            std::shared_ptr<decoded_frame> replaced;
            std::vector<std::unique_ptr<decoded_frame>> superseded;
            std::optional<presented_stream> stream;
            picture_region visible;                  // the part of to_draw that is shown
            std::optional<std::string> crop_warning; // logged after the queue lock
            int width = 0;
            int height = 0;
            bool release_now = false;
            std::shared_ptr<std::promise<outcome<captured_picture>>> capture_now;
            {
                std::scoped_lock lock{mutex};
                if (stopping)
                {
                    return;
                }
                capture_now = std::move(capture_request);
                capture_request.reset();
                std::int64_t const now = clock::now_ns();
                std::int64_t const period = refresh_period.load(std::memory_order_relaxed);
                std::int64_t const lead = period / 2;
                std::optional<media_source> const shown = displayed_source();
                if (shown)
                {
                    source_slot &slot = slots[source_index(*shown)];
                    auto const due = [now, lead](pending_frame const &queued)
                    {
                        return queued.present_time_ns - lead <= now;
                    };
                    std::optional<pending_frame> chosen;
                    if (policy.newest_due_wins)
                    {
                        while (!slot.queue.empty() && due(slot.queue.front()))
                        {
                            if (chosen)
                            {
                                superseded.push_back(std::move(chosen->frame));
                            }
                            chosen = std::move(slot.queue.front());
                            slot.queue.pop_front();
                        }
                    }
                    else
                    {
                        // in order: a picture that is too late gives way, but only
                        // to a successor that can be shown right now
                        std::int64_t const tolerance = policy.late_periods * period;
                        while (slot.queue.size() >= 2 && due(slot.queue[1]) &&
                               now - slot.queue.front().present_time_ns > tolerance)
                        {
                            superseded.push_back(std::move(slot.queue.front().frame));
                            slot.queue.pop_front();
                        }
                        if (!slot.queue.empty() && due(slot.queue.front()))
                        {
                            chosen = std::move(slot.queue.front());
                            slot.queue.pop_front();
                        }
                    }
                    if (chosen)
                    {
                        std::int64_t const lateness = now - chosen->present_time_ns;
                        lateness_average = 0.95 * lateness_average + 0.05 * static_cast<double>(lateness);
                        if (interval_lateness.size() < most_interval_samples)
                        {
                            interval_lateness.push_back(lateness);
                        }
                        replaced = std::move(slot.current);
                        slot.current = std::move(chosen->frame);
                        // a crop belongs to pictures of one size
                        int const picture_width = slot.current->width();
                        int const picture_height = slot.current->height();
                        if (slot.crop.empty())
                        {
                            slot.drop_crop();
                        }
                        else if (slot.crop_width == 0 && slot.crop_height == 0)
                        {
                            // asked for before any picture existed: this is the first
                            std::optional<picture_region> const fitted =
                                fit_region(slot.crop, picture_width, picture_height);
                            if (!fitted)
                            {
                                crop_warning = crop_complaint(slot.crop, picture_width, picture_height);
                            }
                            slot.crop = fitted.value_or(picture_region{});
                            slot.crop_width = picture_width;
                            slot.crop_height = picture_height;
                        }
                        else if (slot.crop_width != picture_width || slot.crop_height != picture_height)
                        {
                            slot.drop_crop();
                        }
                    }
                    if (slot.current)
                    {
                        to_draw = slot.current;
                        visible = slot.crop.empty() ? picture_region{0, 0, to_draw->width(), to_draw->height()}
                                                    : slot.crop;
                        stream = presented_stream{*shown, to_draw->width(), to_draw->height(), visible};
                    }
                }
                stats.dropped_late += superseded.size();
                stats.dropped = stats.dropped_late + stats.dropped_overflow;
                stats.mean_lateness_ns = static_cast<std::int64_t>(lateness_average);
                interval_wake_overshoot = std::max(interval_wake_overshoot, std::exchange(wake_overshoot, 0));
                width = requested_width;
                height = requested_height;
                redraw_requested = false;
                release_now = std::exchange(release_requested, false) && !shown;
            }
            // pictures that will not be shown go back to the decoder at once
            superseded.clear();
            replaced.reset();
            if (crop_warning)
            {
                log::warning("video presentation: {}", *crop_warning);
            }

            // ---- draw and present (GPU lock only)
            bool presented = false;
            std::optional<failure> problem;
            if (to_draw && !device_lost.load(std::memory_order_relaxed))
            {
                std::scoped_lock gpu_lock{gpu->context_mutex()};
                status rendered = ensure_swap_chain(width, height);
                if (rendered)
                {
                    rendered = draw(*to_draw, visible, policy.bilinear_only);
                }
                if (rendered)
                {
                    HRESULT const result = swap_chain->Present(1, 0);
                    if (FAILED(result))
                    {
                        rendered = std::unexpected(make_hresult_failure("IDXGISwapChain::Present", result));
                    }
                    else
                    {
                        presented = true;
                        occluded = result == DXGI_STATUS_OCCLUDED;
                    }
                }
                if (!rendered)
                {
                    problem = rendered.error();
                }
            }
            if (problem)
            {
                report_render_failure(*problem);
            }
            else if (std::uint64_t const overdue = render_error_gate.take_overdue(clock::now_ns()); overdue > 0)
            {
                log::error("video presentation: {} more failures, most recent: {}", overdue, last_render_error);
            }
            if (presented)
            {
                std::scoped_lock lock{mutex};
                ++stats.presented;
                interval_stream_width = to_draw->width();
                interval_stream_height = to_draw->height();
            }
            if (capture_now)
            {
                // a screenshot: the picture that was just drawn, read back
                outcome<captured_picture> copy = std::unexpected(make_failure("screenshot", 0, "no picture is shown"));
                if (device_lost.load(std::memory_order_relaxed))
                {
                    copy = std::unexpected(make_failure("screenshot", 0, "the graphics device was lost"));
                }
                else if (to_draw)
                {
                    std::scoped_lock gpu_lock{gpu->context_mutex()};
                    copy = read_back(*to_draw, visible);
                }
                capture_now->set_value(std::move(copy));
                capture_now.reset();
            }
            to_draw.reset();

            // ---- tell the owner what is on screen, then free what an ended stream left
            bool const announce = stream != announced;
            announced = stream;
            if (announce && listener)
            {
                listener(stream);
            }
            if (release_now)
            {
                std::scoped_lock gpu_lock{gpu->context_mutex()};
                release_stream_objects();
            }
        }
    }

    video_presenter::video_presenter(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    video_presenter::~video_presenter()
    {
        {
            std::scoped_lock lock{impl->mutex};
            impl->stopping = true;
        }
        impl->wake();
        if (impl->render_thread.joinable())
        {
            impl->render_thread.join();
        }
        std::scoped_lock gpu_lock{impl->gpu->context_mutex()};
        impl->slots = {};
        impl->release_stream_objects();
    }

    outcome<std::unique_ptr<video_presenter>> video_presenter::create(gpu_device &device, void *window_handle,
                                                                      std::int64_t refresh_period_ns,
                                                                      display_change_listener listener)
    {
        auto created = std::make_unique<state>();
        created->gpu = &device;
        created->window = static_cast<HWND>(window_handle);
        created->refresh_period = refresh_period_ns > 0 ? refresh_period_ns : 16'666'667;
        created->listener = std::move(listener);
        created->wake_event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (created->wake_event.value == nullptr)
        {
            return std::unexpected(make_win32_failure("CreateEventW (render wake)", GetLastError()));
        }
        RECT client{};
        GetClientRect(created->window, &client);
        created->requested_width = std::max<int>(client.right - client.left, 1);
        created->requested_height = std::max<int>(client.bottom - client.top, 1);
        {
            std::scoped_lock gpu_lock{device.context_mutex()};
            status const pipeline = created->create_pipeline();
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
        }
        device.set_presentation_hold(presentation_hold_for(created->mode.load()));
        state *const running = created.get();
        created->render_thread = std::thread{[running]
        {
            running->render_loop();
        }};
        return std::unique_ptr<video_presenter>{new video_presenter{std::move(created)}};
    }

    void video_presenter::resize(int client_width, int client_height)
    {
        {
            std::scoped_lock lock{impl->mutex};
            impl->requested_width = std::max(client_width, 1);
            impl->requested_height = std::max(client_height, 1);
            impl->redraw_requested = true;
        }
        impl->wake();
    }

    void video_presenter::submit(media_source source, std::unique_ptr<decoded_frame> frame,
                                 std::int64_t present_time_ns)
    {
        if (!frame)
        {
            return;
        }
        std::vector<std::unique_ptr<decoded_frame>> evicted;
        bool head_changed = false;
        {
            std::scoped_lock lock{impl->mutex};
            source_slot &slot = impl->slots[source_index(source)];
            slot.active = true;
            auto const position = std::upper_bound(slot.queue.begin(), slot.queue.end(), present_time_ns,
                                                   [](std::int64_t time, pending_frame const &queued)
                                                   {
                                                       return time < queued.present_time_ns;
                                                   });
            head_changed = position == slot.queue.begin();
            slot.queue.insert(position, pending_frame{std::move(frame), present_time_ns});
            std::size_t const limit = policy_for(impl->mode.load(std::memory_order_relaxed)).queue_limit;
            while (slot.queue.size() > limit)
            {
                evicted.push_back(std::move(slot.queue.front().frame));
                slot.queue.pop_front();
                ++impl->stats.dropped_overflow;
                head_changed = true;
            }
            impl->stats.dropped = impl->stats.dropped_late + impl->stats.dropped_overflow;
            impl->interval_peak_depth = std::max(impl->interval_peak_depth, slot.queue.size());
        }
        if (head_changed)
        {
            // the render thread sleeps until the head of the queue is due
            impl->wake();
        }
    }

    void video_presenter::release_source(media_source source, bool keep_last_frame)
    {
        std::shared_ptr<decoded_frame> released;
        std::deque<pending_frame> released_queue;
        {
            std::scoped_lock lock{impl->mutex};
            source_slot &slot = impl->slots[source_index(source)];
            slot.active = false;
            slot.drop_crop(); // a kept last picture is shown whole again
            released_queue.swap(slot.queue);
            if (!keep_last_frame)
            {
                released = std::move(slot.current);
                impl->release_requested = true;
            }
            impl->redraw_requested = true;
        }
        impl->wake();
        // decoder textures are released under the GPU lock
        std::scoped_lock gpu_lock{impl->gpu->context_mutex()};
        released.reset();
        released_queue.clear();
    }

    void video_presenter::set_crop(media_source source, picture_region region)
    {
        std::optional<std::string> warning;
        {
            std::scoped_lock lock{impl->mutex};
            source_slot &slot = impl->slots[source_index(source)];
            slot.drop_crop();
            if (!region.empty())
            {
                if (slot.current)
                {
                    // the picture on screen says at once whether the region fits
                    int const picture_width = slot.current->width();
                    int const picture_height = slot.current->height();
                    std::optional<picture_region> const fitted = fit_region(region, picture_width, picture_height);
                    if (!fitted)
                    {
                        warning = crop_complaint(region, picture_width, picture_height);
                    }
                    slot.crop = fitted.value_or(picture_region{});
                    slot.crop_width = picture_width;
                    slot.crop_height = picture_height;
                }
                else
                {
                    // no picture yet: the render thread checks it against the first one
                    slot.crop = region;
                }
            }
            // the picture on screen is drawn again, also while the stream is still
            impl->redraw_requested = true;
        }
        impl->wake();
        if (warning)
        {
            log::warning("video presentation: {}", *warning);
        }
    }

    std::int64_t video_presenter::refresh_period_ns() const
    {
        return impl->refresh_period.load(std::memory_order_relaxed);
    }

    void video_presenter::set_refresh_period(std::int64_t refresh_period_ns)
    {
        if (refresh_period_ns > 0)
        {
            impl->refresh_period.store(refresh_period_ns, std::memory_order_relaxed);
            impl->wake(); // the due time of the waiting picture moved
        }
    }

    void video_presenter::set_performance_mode(performance_mode mode)
    {
        impl->mode.store(mode, std::memory_order_relaxed);
        impl->gpu->set_presentation_hold(presentation_hold_for(mode));
        {
            // the picture on screen is drawn again with the mode's filter
            std::scoped_lock lock{impl->mutex};
            impl->redraw_requested = true;
        }
        impl->wake();
    }

    outcome<captured_picture> video_presenter::capture(std::chrono::milliseconds patience)
    {
        auto const request = std::make_shared<std::promise<outcome<captured_picture>>>();
        std::future<outcome<captured_picture>> answer = request->get_future();
        {
            std::scoped_lock lock{impl->mutex};
            if (impl->capture_request)
            {
                return std::unexpected(make_failure("screenshot", 0, "another one is being taken"));
            }
            impl->capture_request = request;
            impl->redraw_requested = true; // the render thread draws the picture again, then copies it
        }
        impl->wake();
        if (answer.wait_for(patience) != std::future_status::ready)
        {
            std::scoped_lock lock{impl->mutex};
            if (impl->capture_request == request)
            {
                impl->capture_request.reset();
                return std::unexpected(make_failure("screenshot", patience.count(),
                                                    "the picture was not drawn again within this many milliseconds"));
            }
            // the render thread took the request in this very moment: its answer is on the way
        }
        try
        {
            return answer.get();
        }
        catch (std::future_error const &cause)
        {
            return std::unexpected(make_failure("screenshot", cause.code().value(), "the presenter ended first"));
        }
    }

    presenter_statistics video_presenter::statistics() const
    {
        presenter_statistics result;
        {
            std::scoped_lock lock{impl->mutex};
            result = impl->stats;
        }
        result.device_lost = impl->device_lost.load(std::memory_order_relaxed);
        return result;
    }

    presenter_interval video_presenter::take_interval()
    {
        presenter_interval result;
        std::vector<std::int64_t> lateness;
        {
            std::scoped_lock lock{impl->mutex};
            lateness.swap(impl->interval_lateness);
            result.peak_queue_depth = std::exchange(impl->interval_peak_depth, 0);
            result.worst_wake_overshoot_ns = std::exchange(impl->interval_wake_overshoot, 0);
            result.stream_width = std::exchange(impl->interval_stream_width, 0);
            result.stream_height = std::exchange(impl->interval_stream_height, 0);
        }
        if (!lateness.empty())
        {
            auto const percentile = [&lateness](std::size_t percent)
            {
                auto const rank = lateness.begin() + static_cast<std::ptrdiff_t>((lateness.size() * percent) / 100);
                std::nth_element(lateness.begin(), rank, lateness.end());
                return *rank;
            };
            result.lateness_p50_ns = percentile(50);
            result.lateness_p95_ns = percentile(95);
        }
        return result;
    }
}
