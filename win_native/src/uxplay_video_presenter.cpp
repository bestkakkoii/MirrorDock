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
        constexpr std::size_t max_queued_frames_per_source = 8;

        struct alignas(16) present_constants
        {
            float color_row0[4];
            float color_row1[4];
            float color_row2[4];
            float luma_size[2];
            float visible_scale[2];
            float filter_mode;
            float padding[3];
        };
        static_assert(sizeof(present_constants) % 16 == 0);

        struct pending_frame
        {
            std::unique_ptr<decoded_frame> frame;
            std::int64_t present_time_ns = 0;
        };

        struct source_slot
        {
            std::deque<pending_frame> queue;
            std::unique_ptr<decoded_frame> current;
            bool active = false;
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
        std::int64_t refresh_period = 16'666'667;
        display_change_listener listener;

        // GPU objects (render thread only, under gpu->context_mutex())
        ComPtr<IDXGISwapChain2> swap_chain;
        HANDLE frame_latency_waitable = nullptr;
        ComPtr<ID3D11VertexShader> vertex_shader;
        ComPtr<ID3D11PixelShader> pixel_shader_nv12;
        ComPtr<ID3D11PixelShader> pixel_shader_i420;
        ComPtr<ID3D11SamplerState> sampler;
        ComPtr<ID3D11Buffer> constant_buffer;
        ComPtr<ID3D11RenderTargetView> back_buffer_view;
        int swap_width = 0;
        int swap_height = 0;

        // hardware frame views cached per (texture, slice)
        ID3D11Texture2D *cached_texture = nullptr;
        std::map<UINT, plane_views> slice_views;
        // copy target when decoder textures are not shader-readable
        ComPtr<ID3D11Texture2D> copy_texture;
        plane_views copy_views;
        // upload targets for software-decoded frames
        std::array<ComPtr<ID3D11Texture2D>, 3> upload_textures;
        plane_views upload_views;
        int upload_width = 0;
        int upload_height = 0;
        frame_memory upload_kind = frame_memory::cpu_yuv420p;

        // shared state (guarded by mutex)
        mutable std::mutex mutex;
        std::condition_variable wake;
        std::array<source_slot, media_source_count> slots;
        bool redraw_requested = false;
        int requested_width = 0;
        int requested_height = 0;
        bool stopping = false;
        presenter_statistics stats;
        std::optional<presented_stream> announced;

        std::thread render_thread;

        status create_pipeline();
        status ensure_swap_chain(int width, int height);
        outcome<plane_views const *> views_for(decoded_frame const &frame);
        status upload(decoded_frame const &frame);
        void draw(decoded_frame const *frame);
        void render_loop();
        [[nodiscard]] std::optional<media_source> displayed_source() const;
    };

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
            gpu->factory()->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
            result = created.As(&swap_chain);
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure("IDXGISwapChain2 query", result));
            }
            swap_chain->SetMaximumFrameLatency(1);
            frame_latency_waitable = swap_chain->GetFrameLatencyWaitableObject();
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
                HRESULT const result = device->CreateTexture2D(&description, nullptr, &upload_textures[plane]);
                if (FAILED(result))
                {
                    return std::unexpected(make_hresult_failure("CreateTexture2D upload plane", result));
                }
                auto view = make_slice_view(device, upload_textures[plane].Get(), description.Format, 0);
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
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT const result = context->Map(upload_textures[plane].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
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
            context->Unmap(upload_textures[plane].Get(), 0);
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

    void video_presenter::state::draw(decoded_frame const *frame)
    {
        ID3D11DeviceContext *context = gpu->context();
        if (!back_buffer_view)
        {
            ComPtr<ID3D11Texture2D> back_buffer;
            HRESULT const result = swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
            if (FAILED(result))
            {
                log::error("{}", make_hresult_failure("IDXGISwapChain::GetBuffer", result).describe());
                return;
            }
            gpu->device()->CreateRenderTargetView(back_buffer.Get(), nullptr, &back_buffer_view);
        }
        float const black[4] = {0.0F, 0.0F, 0.0F, 1.0F};
        context->ClearRenderTargetView(back_buffer_view.Get(), black);
        ID3D11RenderTargetView *targets[] = {back_buffer_view.Get()};
        context->OMSetRenderTargets(1, targets, nullptr);
        if (frame == nullptr)
        {
            return;
        }

        auto views = views_for(*frame);
        if (!views)
        {
            log::error("video frame views: {}", views.error().describe());
            return;
        }

        // aspect-correct letterbox inside the client area
        double const scale = std::min(static_cast<double>(swap_width) / frame->width(),
                                      static_cast<double>(swap_height) / frame->height());
        double const view_width = frame->width() * scale;
        double const view_height = frame->height() * scale;
        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = static_cast<float>(std::floor((swap_width - view_width) / 2.0));
        viewport.TopLeftY = static_cast<float>(std::floor((swap_height - view_height) / 2.0));
        viewport.Width = static_cast<float>(std::round(view_width));
        viewport.Height = static_cast<float>(std::round(view_height));
        viewport.MaxDepth = 1.0F;
        context->RSSetViewports(1, &viewport);

        ComPtr<ID3D11Resource> luma_resource;
        (*views)->luma->GetResource(&luma_resource);
        ComPtr<ID3D11Texture2D> luma_texture;
        luma_resource.As(&luma_texture);
        D3D11_TEXTURE2D_DESC texture_description{};
        luma_texture->GetDesc(&texture_description);

        present_constants constants{};
        fill_color_matrix(constants, frame->color_matrix(), frame->color_range());
        constants.luma_size[0] = static_cast<float>(texture_description.Width);
        constants.luma_size[1] = static_cast<float>(texture_description.Height);
        constants.visible_scale[0] = static_cast<float>(frame->width()) / static_cast<float>(texture_description.Width);
        constants.visible_scale[1] =
            static_cast<float>(frame->height()) / static_cast<float>(texture_description.Height);
        constants.filter_mode = scale >= 1.02 ? 1.0F : (scale <= 0.98 ? 2.0F : 0.0F);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(constant_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(constant_buffer.Get(), 0);
        }

        bool const three_planes = frame->memory() == frame_memory::cpu_yuv420p;
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
        std::int64_t const lead = refresh_period / 2;
        double lateness_average = 0.0;

        for (;;)
        {
            std::unique_lock lock{mutex};
            // wait until a frame is due, a redraw is needed, or we stop
            for (;;)
            {
                if (stopping)
                {
                    return;
                }
                std::optional<media_source> const shown = displayed_source();
                bool due = false;
                std::int64_t next_due = std::numeric_limits<std::int64_t>::max();
                if (shown)
                {
                    source_slot const &slot = slots[source_index(*shown)];
                    if (!slot.queue.empty())
                    {
                        next_due = slot.queue.front().present_time_ns - lead;
                        due = next_due <= clock::now_ns();
                    }
                }
                if (due || redraw_requested)
                {
                    break;
                }
                if (next_due == std::numeric_limits<std::int64_t>::max())
                {
                    wake.wait(lock);
                }
                else
                {
                    wake.wait_for(lock, std::chrono::nanoseconds{std::max<std::int64_t>(next_due - clock::now_ns(), 0)});
                }
            }
            lock.unlock();

            // the previous frame must have been flipped before we render the next
            if (frame_latency_waitable != nullptr)
            {
                WaitForSingleObjectEx(frame_latency_waitable, 100, TRUE);
            }

            lock.lock();
            if (stopping)
            {
                return;
            }
            std::int64_t const now = clock::now_ns();
            std::optional<media_source> const shown = displayed_source();
            decoded_frame const *to_draw = nullptr;
            std::optional<presented_stream> stream;
            if (shown)
            {
                source_slot &slot = slots[source_index(*shown)];
                // newest due frame wins; older due frames are dropped
                std::optional<pending_frame> chosen;
                while (!slot.queue.empty() && slot.queue.front().present_time_ns - lead <= now)
                {
                    if (chosen)
                    {
                        ++stats.dropped;
                    }
                    chosen = std::move(slot.queue.front());
                    slot.queue.pop_front();
                }
                if (chosen)
                {
                    lateness_average = 0.95 * lateness_average + 0.05 * static_cast<double>(now - chosen->present_time_ns);
                    slot.current = std::move(chosen->frame);
                }
                if (slot.current)
                {
                    to_draw = slot.current.get();
                    stream = presented_stream{*shown, to_draw->width(), to_draw->height()};
                }
            }
            int const width = requested_width;
            int const height = requested_height;
            redraw_requested = false;
            bool const announce = stream != announced;
            announced = stream;

            {
                std::scoped_lock gpu_lock{gpu->context_mutex()};
                status const ready = ensure_swap_chain(width, height);
                if (!ready)
                {
                    log::error("{}", ready.error().describe());
                }
                else
                {
                    draw(to_draw);
                    HRESULT const result = swap_chain->Present(1, 0);
                    if (FAILED(result))
                    {
                        log::error("{}", make_hresult_failure("IDXGISwapChain::Present", result).describe());
                    }
                    else if (to_draw != nullptr)
                    {
                        ++stats.presented;
                    }
                }
            }
            stats.mean_lateness_ns = static_cast<std::int64_t>(lateness_average);
            lock.unlock();

            if (announce && listener)
            {
                listener(stream);
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
        impl->wake.notify_all();
        if (impl->render_thread.joinable())
        {
            impl->render_thread.join();
        }
        std::scoped_lock gpu_lock{impl->gpu->context_mutex()};
        impl->slots = {};
        impl->slice_views.clear();
        impl->back_buffer_view.Reset();
        impl->swap_chain.Reset();
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
        impl->wake.notify_all();
    }

    void video_presenter::submit(media_source source, std::unique_ptr<decoded_frame> frame,
                                 std::int64_t present_time_ns)
    {
        {
            std::scoped_lock lock{impl->mutex};
            source_slot &slot = impl->slots[source_index(source)];
            slot.active = true;
            auto const position = std::upper_bound(slot.queue.begin(), slot.queue.end(), present_time_ns,
                                                   [](std::int64_t time, pending_frame const &queued)
                                                   {
                                                       return time < queued.present_time_ns;
                                                   });
            slot.queue.insert(position, pending_frame{std::move(frame), present_time_ns});
            while (slot.queue.size() > max_queued_frames_per_source)
            {
                slot.queue.pop_front();
                ++impl->stats.dropped;
            }
        }
        impl->wake.notify_all();
    }

    void video_presenter::release_source(media_source source, bool keep_last_frame)
    {
        std::unique_ptr<decoded_frame> released;
        std::deque<pending_frame> released_queue;
        {
            std::scoped_lock lock{impl->mutex};
            source_slot &slot = impl->slots[source_index(source)];
            slot.active = false;
            released_queue.swap(slot.queue);
            if (!keep_last_frame)
            {
                released = std::move(slot.current);
            }
            impl->redraw_requested = true;
        }
        impl->wake.notify_all();
        // decoder textures must be released under the GPU lock
        std::scoped_lock gpu_lock{impl->gpu->context_mutex()};
        released.reset();
        released_queue.clear();
    }

    std::int64_t video_presenter::refresh_period_ns() const
    {
        return impl->refresh_period;
    }

    presenter_statistics video_presenter::statistics() const
    {
        std::scoped_lock lock{impl->mutex};
        return impl->stats;
    }
}
