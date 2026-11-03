module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>

module uxplay_audio_output;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_clock;
import uxplay_media_types;
import uxplay_system_tuning;

using Microsoft::WRL::ComPtr;

namespace uxplay
{
    namespace
    {
        constexpr double hard_resync_ns = 30'000'000.0;
        constexpr double correction_deadband_ns = 500'000.0;
        // error (ns) corrected per second of playback at full correction
        constexpr double correction_horizon_ns = 1'000'000'000.0;
        constexpr double max_rate_correction = 0.003;
        constexpr double contiguous_tolerance_ns = 2'000'000.0;
        constexpr double max_gap_fill_ns = 500'000'000.0;
        constexpr std::size_t max_queued_frames = 48000 * 4;

        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, defined locally (no ksuser.lib)
        constexpr GUID subtype_ieee_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

        bool is_float32(WAVEFORMATEX const *format)
        {
            if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32)
            {
                return true;
            }
            if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            {
                auto const *extensible = reinterpret_cast<WAVEFORMATEXTENSIBLE const *>(format);
                return IsEqualGUID(extensible->SubFormat, subtype_ieee_float) && format->wBitsPerSample == 32;
            }
            return false;
        }

        // 4-point cubic Hermite (Catmull-Rom) interpolation
        float hermite(float previous, float current, float next, float after, float fraction)
        {
            float const c1 = 0.5F * (next - previous);
            float const c2 = previous - 2.5F * current + 2.0F * next - 0.5F * after;
            float const c3 = 0.5F * (after - previous) + 1.5F * (current - next);
            return ((c3 * fraction + c2) * fraction + c1) * fraction + current;
        }

        struct source_timeline
        {
            std::deque<float> samples; // interleaved stereo
            double base_time_ns = 0.0; // time of frame 0 in `samples`
            double read_position = 0.0; // frames from base, fractional
            bool anchored = false;

            [[nodiscard]] std::size_t frames() const
            {
                return samples.size() / 2;
            }

            void clear()
            {
                samples.clear();
                read_position = 0.0;
                anchored = false;
            }

            void consume_whole_frames(double frame_ns)
            {
                auto const whole = static_cast<std::size_t>(std::max(0.0, std::floor(read_position)) );
                std::size_t const removable = whole > 2 ? whole - 2 : 0; // keep history for interpolation
                std::size_t const count = std::min(removable, frames());
                if (count == 0)
                {
                    return;
                }
                samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(count * 2));
                read_position -= static_cast<double>(count);
                base_time_ns += static_cast<double>(count) * frame_ns;
            }
        };

        class endpoint_notifier final : public IMMNotificationClient
        {
        public:
            explicit endpoint_notifier(std::atomic<bool> &changed, HANDLE wake_event)
                : changed_flag{changed},
                  wake{wake_event}
            {
            }

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interface_id, void **object) override
            {
                if (IsEqualIID(interface_id, __uuidof(IUnknown)) || IsEqualIID(interface_id, __uuidof(IMMNotificationClient)))
                {
                    *object = static_cast<IMMNotificationClient *>(this);
                    return S_OK;
                }
                *object = nullptr;
                return E_NOINTERFACE;
            }

            // lifetime is owned by audio_output::state, not by COM references
            ULONG STDMETHODCALLTYPE AddRef() override
            {
                return 1;
            }

            ULONG STDMETHODCALLTYPE Release() override
            {
                return 1;
            }

            HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
            {
                if (flow == eRender && role == eConsole)
                {
                    changed_flag.store(true);
                    SetEvent(wake);
                }
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override
            {
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override
            {
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override
            {
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, PROPERTYKEY const) override
            {
                return S_OK;
            }

        private:
            std::atomic<bool> &changed_flag;
            HANDLE wake;
        };
    }

    namespace
    {
        // Decoded AAC can exceed full scale (AirPlay mirroring peaks of
        // +2.5 dBFS were measured); the endpoint would clip them hard.  Unity
        // below -1 dBFS, then a smooth tanh knee that approaches 0 dBFS.
        float soft_limit(float sample)
        {
            constexpr float knee = 0.891F; // -1 dBFS
            float const magnitude = std::abs(sample);
            if (magnitude <= knee)
            {
                return sample;
            }
            float const limited = knee + (1.0F - knee) * std::tanh((magnitude - knee) / (1.0F - knee));
            return std::copysign(limited, sample);
        }
    }

    struct audio_output::state
    {
        audio_format_listener listener;
        std::thread thread;
        HANDLE audio_event = nullptr;
        HANDLE wake_event = nullptr;
        std::atomic<bool> stopping{false};
        std::atomic<bool> endpoint_changed{false};

        mutable std::mutex mutex; // guards timelines, format, volume
        std::array<source_timeline, media_source_count> timelines;
        std::array<std::atomic<std::int64_t>, media_source_count> delays{};
        audio_device_format device_format{};
        std::array<float, media_source_count> target_gains{1.0F, 1.0F};
        std::array<std::uint64_t, media_source_count> frames_played_since_report{}; // reset by take_flow()
        bool muted = false;
        std::int64_t latency_ns = 0;

        // audio-thread-only
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render_client;
        ComPtr<IAudioClock> audio_clock;
        UINT32 buffer_frames = 0;
        std::uint64_t frames_written = 0;
        int device_channels = 2;
        float current_gain = 1.0F;
        float heard_gain = 1.0F;
        std::vector<float> scratch;
        std::optional<media_source> heard_source;

        status open_endpoint();
        void close_endpoint();
        void render_block(UINT32 frame_count, double first_frame_time_ns, float *destination);
        void thread_main(std::promise<status> &ready);
    };

    status audio_output::state::open_endpoint()
    {
        close_endpoint();
        ComPtr<IMMDevice> device;
        HRESULT result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("GetDefaultAudioEndpoint", result));
        }
        ComPtr<IAudioClient3> client3;
        result = device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &client3);
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("IMMDevice::Activate IAudioClient3", result));
        }
        WAVEFORMATEX *mix_format = nullptr;
        result = client3->GetMixFormat(&mix_format);
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("IAudioClient::GetMixFormat", result));
        }
        std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mix_owner{mix_format, CoTaskMemFree};

        WAVEFORMATEXTENSIBLE float_format{};
        WAVEFORMATEX const *chosen = mix_format;
        bool const native_float = is_float32(mix_format);
        if (!native_float)
        {
            float_format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            float_format.Format.nChannels = mix_format->nChannels;
            float_format.Format.nSamplesPerSec = mix_format->nSamplesPerSec;
            float_format.Format.wBitsPerSample = 32;
            float_format.Format.nBlockAlign = static_cast<WORD>(4 * mix_format->nChannels);
            float_format.Format.nAvgBytesPerSec = mix_format->nSamplesPerSec * float_format.Format.nBlockAlign;
            float_format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            float_format.Samples.wValidBitsPerSample = 32;
            float_format.dwChannelMask = mix_format->nChannels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
            float_format.SubFormat = subtype_ieee_float;
            chosen = &float_format.Format;
        }

        bool low_latency = false;
        UINT32 minimum_period = 0;
        if (native_float)
        {
            UINT32 default_period = 0;
            UINT32 fundamental_period = 0;
            UINT32 maximum_period = 0;
            if (SUCCEEDED(client3->GetSharedModeEnginePeriod(chosen, &default_period, &fundamental_period,
                                                             &minimum_period, &maximum_period)) &&
                minimum_period > 0 &&
                SUCCEEDED(client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minimum_period,
                                                               chosen, nullptr)))
            {
                low_latency = true;
            }
        }
        if (!low_latency)
        {
            // fresh client: a failed InitializeSharedAudioStream leaves it unusable
            client3.Reset();
            result = device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &client3);
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure("IMMDevice::Activate IAudioClient3 (retry)", result));
            }
            DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
            if (!native_float)
            {
                flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            }
            REFERENCE_TIME const buffer_duration = 100'000; // 10 ms in 100 ns units
            result = client3->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer_duration, 0, chosen, nullptr);
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure("IAudioClient::Initialize", result));
            }
        }
        client = client3;
        result = client->SetEventHandle(audio_event);
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("IAudioClient::SetEventHandle", result));
        }
        result = client->GetBufferSize(&buffer_frames);
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("IAudioClient::GetBufferSize", result));
        }
        result = client->GetService(IID_PPV_ARGS(&render_client));
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("GetService IAudioRenderClient", result));
        }
        result = client->GetService(IID_PPV_ARGS(&audio_clock));
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("GetService IAudioClock", result));
        }
        REFERENCE_TIME stream_latency = 0;
        client->GetStreamLatency(&stream_latency);

        device_channels = chosen->nChannels;
        frames_written = 0;
        audio_device_format const opened{static_cast<int>(chosen->nSamplesPerSec), static_cast<int>(chosen->nChannels)};
        bool format_changed = false;
        {
            std::scoped_lock lock{mutex};
            format_changed = opened != device_format;
            device_format = opened;
            double const period_ns = low_latency ? 1e9 * minimum_period / opened.sample_rate
                                                 : 1e9 * buffer_frames / opened.sample_rate;
            latency_ns = static_cast<std::int64_t>(period_ns) + clock::hundred_ns_to_ns(stream_latency);
            if (format_changed)
            {
                for (source_timeline &timeline : timelines)
                {
                    timeline.clear();
                }
            }
        }
        result = client->Start();
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("IAudioClient::Start", result));
        }
        log::info("audio output: {} Hz, {} ch, {} buffer {} frames ({} low-latency period {} frames), latency {:.1f} ms",
                  opened.sample_rate, opened.channels, native_float ? "float32" : "converted float32", buffer_frames,
                  low_latency ? "IAudioClient3" : "standard", low_latency ? minimum_period : 0U,
                  static_cast<double>(latency_ns) / 1e6);
        if (format_changed && listener)
        {
            listener(opened);
        }
        return {};
    }

    void audio_output::state::close_endpoint()
    {
        if (client)
        {
            client->Stop();
        }
        audio_clock.Reset();
        render_client.Reset();
        client.Reset();
        buffer_frames = 0;
    }

    void audio_output::state::render_block(UINT32 frame_count, double first_frame_time_ns, float *destination)
    {
        scratch.assign(static_cast<std::size_t>(frame_count) * 2, 0.0F);
        float gain_target = heard_gain;
        {
            std::scoped_lock lock{mutex};
            double const frame_ns = 1e9 / device_format.sample_rate;

            std::optional<media_source> source;
            for (media_source const candidate : sources_by_priority)
            {
                source_timeline const &timeline = timelines[source_index(candidate)];
                double const delay = static_cast<double>(delays[source_index(candidate)].load(std::memory_order_relaxed));
                if (timeline.anchored && timeline.frames() > 0 &&
                    timeline.base_time_ns + delay + static_cast<double>(timeline.frames()) * frame_ns > first_frame_time_ns)
                {
                    source = candidate;
                    break;
                }
            }
            if (source)
            {
                gain_target = target_gains[source_index(*source)];
                heard_gain = gain_target;
            }
            if (muted)
            {
                gain_target = 0.0F; // ramped like a volume change: no click
            }
            if (source != heard_source)
            {
                if (source)
                {
                    log::info("audio source: {}", media_source_name(*source));
                }
                heard_source = source;
            }

            // inactive sources: discard what is already in the past
            for (media_source const candidate : sources_by_priority)
            {
                if (candidate == source)
                {
                    continue;
                }
                source_timeline &timeline = timelines[source_index(candidate)];
                if (timeline.anchored)
                {
                    double const delay = static_cast<double>(delays[source_index(candidate)].load(std::memory_order_relaxed));
                    timeline.read_position = std::max(timeline.read_position,
                                                      (first_frame_time_ns - delay - timeline.base_time_ns) / frame_ns);
                    timeline.consume_whole_frames(frame_ns);
                    if (timeline.read_position >= static_cast<double>(timeline.frames()))
                    {
                        timeline.clear();
                    }
                }
            }

            if (source)
            {
                source_timeline &timeline = timelines[source_index(*source)];
                double const delay = static_cast<double>(delays[source_index(*source)].load(std::memory_order_relaxed));
                UINT32 produced = 0;
                while (produced < frame_count)
                {
                    double const now_ns = first_frame_time_ns + produced * frame_ns;
                    double const data_time = timeline.base_time_ns + delay + timeline.read_position * frame_ns;
                    double const error = data_time - now_ns; // >0: data is due later
                    std::size_t const available = timeline.frames();
                    if (static_cast<double>(available) <= timeline.read_position + 2.0)
                    {
                        break; // underrun: rest stays silent
                    }
                    if (error > hard_resync_ns)
                    {
                        auto const silent = static_cast<UINT32>(std::min<double>(frame_count - produced, error / frame_ns));
                        produced += std::max<UINT32>(silent, 1);
                        continue;
                    }
                    if (error < -hard_resync_ns)
                    {
                        timeline.read_position += -error / frame_ns;
                        continue;
                    }
                    double step = 1.0;
                    if (std::abs(error) > correction_deadband_ns)
                    {
                        step = 1.0 + std::clamp(-error / correction_horizon_ns, -max_rate_correction, max_rate_correction);
                    }
                    else
                    {
                        // snap to the sample grid when in sync: bit-exact copy
                        double const rounded = std::round(timeline.read_position);
                        if (std::abs(rounded - timeline.read_position) < 0.02)
                        {
                            timeline.read_position = rounded;
                        }
                    }
                    for (; produced < frame_count; ++produced)
                    {
                        auto const index = static_cast<std::size_t>(timeline.read_position);
                        if (index + 2 >= available)
                        {
                            break;
                        }
                        float const fraction = static_cast<float>(timeline.read_position - static_cast<double>(index));
                        std::size_t const previous = index > 0 ? index - 1 : 0;
                        for (int channel = 0; channel < 2; ++channel)
                        {
                            float value = timeline.samples[index * 2 + channel];
                            if (fraction != 0.0F)
                            {
                                value = hermite(timeline.samples[previous * 2 + channel], value,
                                                timeline.samples[(index + 1) * 2 + channel],
                                                timeline.samples[(index + 2) * 2 + channel], fraction);
                            }
                            scratch[static_cast<std::size_t>(produced) * 2 + channel] = value;
                        }
                        timeline.read_position += step;
                        ++frames_played_since_report[source_index(*source)];
                        // re-evaluate the error every 256 frames
                        if ((produced & 255U) == 255U)
                        {
                            ++produced;
                            break;
                        }
                    }
                    timeline.consume_whole_frames(frame_ns);
                }
                timeline.consume_whole_frames(frame_ns);
            }
        }

        // volume ramp (~10 ms) and channel mapping
        float const ramp = 1.0F / (0.010F * 48000.0F);
        for (UINT32 frame = 0; frame < frame_count; ++frame)
        {
            current_gain += std::clamp(gain_target - current_gain, -ramp, ramp);
            float const left = soft_limit(scratch[static_cast<std::size_t>(frame) * 2] * current_gain);
            float const right = soft_limit(scratch[static_cast<std::size_t>(frame) * 2 + 1] * current_gain);
            float *out = destination + static_cast<std::size_t>(frame) * static_cast<std::size_t>(device_channels);
            if (device_channels == 1)
            {
                out[0] = 0.5F * (left + right);
                continue;
            }
            out[0] = left;
            out[1] = right;
            for (int channel = 2; channel < device_channels; ++channel)
            {
                out[channel] = 0.0F;
            }
        }
    }

    void audio_output::state::thread_main(std::promise<status> &ready)
    {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool const com_initialized = SUCCEEDED(result);
        system_tuning::mmcss_scope const mmcss{L"Pro Audio"};

        result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(result))
        {
            ready.set_value(std::unexpected(make_hresult_failure("CoCreateInstance MMDeviceEnumerator", result)));
            if (com_initialized)
            {
                CoUninitialize();
            }
            return;
        }
        endpoint_notifier notifier{endpoint_changed, wake_event};
        enumerator->RegisterEndpointNotificationCallback(&notifier);

        status const opened = open_endpoint();
        ready.set_value(opened);
        int consecutive_failures = 0;

        while (!stopping.load())
        {
            HANDLE const handles[2] = {audio_event, wake_event};
            WaitForMultipleObjects(2, handles, FALSE, 200);
            if (stopping.load())
            {
                break;
            }
            if (endpoint_changed.exchange(false) || !client)
            {
                status const reopened = open_endpoint();
                if (!reopened)
                {
                    if (++consecutive_failures <= 3)
                    {
                        log::error("audio endpoint reopen: {}", reopened.error().describe());
                    }
                    Sleep(500);
                    continue;
                }
                consecutive_failures = 0;
            }

            UINT32 padding = 0;
            result = client->GetCurrentPadding(&padding);
            if (result == AUDCLNT_E_DEVICE_INVALIDATED)
            {
                log::warning("audio device invalidated, reopening default endpoint");
                close_endpoint();
                continue;
            }
            if (FAILED(result))
            {
                continue;
            }
            UINT32 const available = buffer_frames - padding;
            if (available == 0)
            {
                continue;
            }

            // time at which the next written frame will be heard
            double first_frame_time = static_cast<double>(clock::now_ns());
            UINT64 position = 0;
            UINT64 qpc_position = 0;
            UINT64 frequency = 0;
            if (SUCCEEDED(audio_clock->GetFrequency(&frequency)) && frequency > 0 &&
                SUCCEEDED(audio_clock->GetPosition(&position, &qpc_position)))
            {
                double const played_frames = static_cast<double>(position) * device_format.sample_rate /
                                             static_cast<double>(frequency);
                first_frame_time = static_cast<double>(clock::hundred_ns_to_ns(static_cast<std::int64_t>(qpc_position))) +
                                   (static_cast<double>(frames_written) - played_frames) * 1e9 / device_format.sample_rate;
            }

            BYTE *buffer = nullptr;
            result = render_client->GetBuffer(available, &buffer);
            if (FAILED(result))
            {
                continue;
            }
            render_block(available, first_frame_time, reinterpret_cast<float *>(buffer));
            render_client->ReleaseBuffer(available, 0);
            frames_written += available;
        }

        close_endpoint();
        enumerator->UnregisterEndpointNotificationCallback(&notifier);
        enumerator.Reset();
        if (com_initialized)
        {
            CoUninitialize();
        }
    }

    audio_output::audio_output(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    audio_output::~audio_output()
    {
        impl->stopping.store(true);
        SetEvent(impl->wake_event);
        if (impl->thread.joinable())
        {
            impl->thread.join();
        }
        CloseHandle(impl->audio_event);
        CloseHandle(impl->wake_event);
    }

    outcome<std::unique_ptr<audio_output>> audio_output::create(audio_format_listener listener)
    {
        auto created = std::make_unique<state>();
        created->listener = std::move(listener);
        created->audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        created->wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (created->audio_event == nullptr || created->wake_event == nullptr)
        {
            return std::unexpected(make_win32_failure("CreateEventW (audio)", GetLastError()));
        }
        std::promise<status> ready;
        std::future<status> ready_result = ready.get_future();
        state *const running = created.get();
        created->thread = std::thread{[running, &ready]
        {
            running->thread_main(ready);
        }};
        status const started = ready_result.get();
        auto output = std::unique_ptr<audio_output>{new audio_output{std::move(created)}};
        if (!started)
        {
            // keep the thread alive: it retries when the default endpoint changes
            log::error("audio output start: {}", started.error().describe());
        }
        return output;
    }

    audio_device_format audio_output::format() const
    {
        std::scoped_lock lock{impl->mutex};
        return impl->device_format;
    }

    void audio_output::enqueue(media_source source, std::int64_t media_time_ns, std::span<float const> stereo_samples,
                               int sample_rate, bool timestamped)
    {
        std::scoped_lock lock{impl->mutex};
        if (sample_rate != impl->device_format.sample_rate || stereo_samples.empty())
        {
            return;
        }
        double const frame_ns = 1e9 / sample_rate;
        source_timeline &timeline = impl->timelines[source_index(source)];
        auto const chunk_time = static_cast<double>(media_time_ns);
        std::size_t skip_frames = 0;
        if (timeline.anchored && !timestamped)
        {
            // play on arrival: keep appending; re-anchor only after an underrun
            if (timeline.read_position + 2.0 >= static_cast<double>(timeline.frames()))
            {
                timeline.clear();
            }
        }
        else if (timeline.anchored)
        {
            double const expected = timeline.base_time_ns + static_cast<double>(timeline.frames()) * frame_ns;
            double const delta = chunk_time - expected;
            if (delta > contiguous_tolerance_ns && delta < max_gap_fill_ns)
            {
                auto const gap = static_cast<std::size_t>(delta / frame_ns);
                timeline.samples.insert(timeline.samples.end(), gap * 2, 0.0F);
            }
            else if (delta < -contiguous_tolerance_ns && delta > -100'000'000.0)
            {
                skip_frames = static_cast<std::size_t>(-delta / frame_ns); // overlap: drop duplicated start
            }
            else if (std::abs(delta) >= max_gap_fill_ns || delta <= -100'000'000.0)
            {
                timeline.clear(); // timeline jump (seek, new stream): re-anchor
            }
        }
        if (!timeline.anchored)
        {
            timeline.anchored = true;
            timeline.base_time_ns = chunk_time;
            timeline.read_position = 0.0;
        }
        std::size_t const offset = std::min(skip_frames * 2, stereo_samples.size());
        timeline.samples.insert(timeline.samples.end(), stereo_samples.begin() + static_cast<std::ptrdiff_t>(offset),
                                stereo_samples.end());
        if (timeline.frames() > max_queued_frames)
        {
            timeline.clear();
            log::warning("audio queue overflow for {}: resynchronizing", media_source_name(source));
        }
    }

    void audio_output::set_delay(media_source source, std::int64_t delay_ns)
    {
        impl->delays[source_index(source)].store(delay_ns, std::memory_order_relaxed);
    }

    void audio_output::release_source(media_source source)
    {
        std::scoped_lock lock{impl->mutex};
        impl->timelines[source_index(source)].clear();
    }

    void audio_output::set_volume(media_source source, float linear_gain)
    {
        std::scoped_lock lock{impl->mutex};
        impl->target_gains[source_index(source)] = std::clamp(linear_gain, 0.0F, 10.0F);
    }

    std::int64_t audio_output::device_latency_ns() const
    {
        std::scoped_lock lock{impl->mutex};
        return impl->latency_ns;
    }

    audio_flow audio_output::take_flow(media_source source)
    {
        std::scoped_lock lock{impl->mutex};
        audio_flow flow;
        flow.played_frames = std::exchange(impl->frames_played_since_report[source_index(source)], 0);
        flow.gain = impl->target_gains[source_index(source)];
        flow.muted = impl->muted;
        return flow;
    }

    void audio_output::set_muted(bool muted)
    {
        std::scoped_lock lock{impl->mutex};
        impl->muted = muted;
    }
}
