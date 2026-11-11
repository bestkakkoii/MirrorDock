// SPDX-License-Identifier: MIT
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

        // The stream runs only while a source has queued audio.  After
        // idle_stop_ns without any it is stopped (no period wake-ups).  The
        // stopped endpoint is released, which ends the audio session, after
        // idle_release_ns when no source is in use, or after
        // idle_release_live_ns of silence from a source that still is.
        constexpr std::int64_t idle_stop_ns = 3 * clock::ns_per_second;
        constexpr std::int64_t idle_release_ns = 2 * clock::ns_per_second;
        constexpr std::int64_t idle_release_live_ns = 300 * clock::ns_per_second;
        // a running stream is served at the latest after this time without a period event
        constexpr DWORD period_watchdog_ms = 200;
        // upper bound of one timed sleep, above every deadline used here (it is evaluated again afterwards)
        constexpr std::int64_t longest_timed_wait_ms = 3'600'000;
        // pause of a pass whose wait failed, so the thread cannot spin
        constexpr DWORD wait_failure_pause_ms = 10;

        // Back-off between attempts to open a failed endpoint: doubles while
        // the failures continue, starts over after retry_settled_ns without one.
        constexpr std::int64_t retry_delay_min_ns = 250 * clock::ns_per_ms;
        constexpr std::int64_t retry_delay_max_ns = 5 * clock::ns_per_second;
        constexpr std::int64_t retry_settled_ns = 10 * clock::ns_per_second;
        // attempts to open an endpoint that no source is waiting for (after
        // a start failure or an endpoint change), so format() follows the device
        constexpr int endpoint_probe_attempts = 6;
        // repetitions of one failure are written at most this often
        constexpr std::int64_t failure_report_interval_ns = 10 * clock::ns_per_second;
        // default endpoint poll, used only when endpoint notifications are unavailable
        constexpr std::int64_t endpoint_poll_ns = 3 * clock::ns_per_second;

        // failure::code of an HRESULT (see make_hresult_failure)
        constexpr std::int64_t device_invalidated_code =
            static_cast<std::int64_t>(static_cast<std::uint32_t>(AUDCLNT_E_DEVICE_INVALIDATED));

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

        // Interleaved samples of one source in a block of memory that is
        // allocated once: queueing and playing never touch the heap, so the
        // lock shared by the network thread and the audio thread is held for
        // copies only.  It has the sequence semantics of the growing queue it
        // replaces (append at the end, drop at the front, index from the
        // front); enqueue() keeps the size within the capacity, which is the
        // overflow limit of that queue.
        class sample_ring
        {
        public:
            [[nodiscard]] std::size_t size() const
            {
                return count;
            }

            [[nodiscard]] float operator[](std::size_t index) const
            {
                return storage[wrapped(head + index)];
            }

            void clear()
            {
                head = 0;
                count = 0;
            }

            void append(std::span<float const> samples)
            {
                std::span<float const> const accepted = samples.first(std::min(samples.size(), storage.size() - count));
                std::size_t const end = wrapped(head + count);
                std::size_t const before_wrap = std::min(accepted.size(), storage.size() - end);
                std::ranges::copy(accepted.first(before_wrap), storage.begin() + static_cast<std::ptrdiff_t>(end));
                std::ranges::copy(accepted.subspan(before_wrap), storage.begin());
                count += accepted.size();
            }

            void append_silence(std::size_t sample_count)
            {
                std::size_t const accepted = std::min(sample_count, storage.size() - count);
                std::size_t const end = wrapped(head + count);
                std::size_t const before_wrap = std::min(accepted, storage.size() - end);
                std::fill_n(storage.begin() + static_cast<std::ptrdiff_t>(end), before_wrap, 0.0F);
                std::fill_n(storage.begin(), accepted - before_wrap, 0.0F);
                count += accepted;
            }

            void drop_front(std::size_t sample_count)
            {
                std::size_t const dropped = std::min(sample_count, count);
                head = wrapped(head + dropped);
                count -= dropped;
            }

        private:
            // positions stay below twice the capacity
            [[nodiscard]] std::size_t wrapped(std::size_t position) const
            {
                return position < storage.size() ? position : position - storage.size();
            }

            // the overflow limit in samples; one more is kept when a chunk had an odd sample count
            std::vector<float> storage = std::vector<float>(max_queued_frames * 2 + 2);
            std::size_t head = 0;
            std::size_t count = 0;
        };

        struct source_timeline
        {
            sample_ring samples; // interleaved stereo
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
                samples.drop_front(count * 2);
                read_position -= static_cast<double>(count);
                base_time_ns += static_cast<double>(count) * frame_ns;
            }
        };

        // What the sources ask of the endpoint (read under the queue lock).
        struct source_demand
        {
            bool pending = false; // a source has queued audio: the stream has to run
            bool live = false;    // a source is in use (first chunk ... release_source())
        };

        // Failures of the audio thread.  The first failure of a kind (stage
        // and code) is written at once; its repetitions are counted and
        // written as one line at most every failure_report_interval_ns, so a
        // broken endpoint cannot flood the log and no failure goes unreported.
        class failure_report
        {
        public:
            void add(failure const &failed, std::int64_t now_ns)
            {
                bool const same_kind = reported && failed.stage == last.stage && failed.code == last.code;
                if (same_kind && now_ns - written_ns < failure_report_interval_ns)
                {
                    ++unreported;
                    return;
                }
                if (!same_kind)
                {
                    flush(); // the count of the previous kind
                }
                last = failed;
                reported = true;
                if (unreported > 0)
                {
                    write(std::format("{} (and {} more since the previous report)", last.describe(), unreported));
                }
                else
                {
                    write(last.describe());
                }
                unreported = 0;
                written_ns = now_ns;
            }

            // writes a count that is still waiting for its interval
            void flush()
            {
                if (unreported > 0)
                {
                    write(std::format("{} ({} more since the previous report)", last.describe(), unreported));
                    unreported = 0;
                }
            }

        private:
            void write(std::string const &text) const
            {
                if (last.code == device_invalidated_code)
                {
                    log::warning("audio output: {}", text); // the ordinary start of an endpoint switch
                    return;
                }
                log::error("audio output: {}", text);
            }

            failure last;
            std::int64_t written_ns = 0;
            std::uint64_t unreported = 0;
            bool reported = false;
        };

        // COM (multithreaded apartment) on the audio thread for its lifetime
        class com_apartment
        {
        public:
            com_apartment()
                : initialized{CoInitializeEx(nullptr, COINIT_MULTITHREADED)}
            {
            }

            ~com_apartment()
            {
                if (SUCCEEDED(initialized))
                {
                    CoUninitialize();
                }
            }

            com_apartment(com_apartment const &) = delete;
            com_apartment &operator=(com_apartment const &) = delete;

            [[nodiscard]] HRESULT result() const
            {
                return initialized;
            }

        private:
            HRESULT initialized;
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
        ~state();

        audio_format_listener listener;
        std::thread thread;
        HANDLE audio_event = nullptr; // period event of the running stream
        HANDLE wake_event = nullptr;  // shutdown, endpoint change, audio for a stopped stream, released source
        std::atomic<bool> stopping{false};
        std::atomic<bool> endpoint_changed{false};

        mutable std::mutex mutex; // guards timelines, live, format, volume, wake_on_enqueue
        std::array<source_timeline, media_source_count> timelines;
        std::array<bool, media_source_count> live{}; // in use: from the first chunk to release_source()
        std::array<std::atomic<std::int64_t>, media_source_count> delays{};
        audio_device_format device_format{};
        std::array<float, media_source_count> target_gains = []
        {
            std::array<float, media_source_count> unity{};
            unity.fill(1.0F);
            return unity;
        }();
        std::array<std::uint64_t, media_source_count> frames_played_since_report{}; // reset by take_flow()
        bool muted = false;
        std::int64_t latency_ns = 0;
        bool wake_on_enqueue = false; // the audio thread sleeps until a source has queued audio

        // Left by the audio thread for another thread to write to the log
        // (write_notes): it does not wait for the log sink while it feeds the
        // device.
        std::atomic<int> heard_note{0};                 // 1 + index of the source that became audible
        std::atomic<std::int64_t> restart_note_ns{-1};  // wake-up of the stopped stream: request to first buffer
        std::atomic<std::int64_t> wake_requested_ns{0}; // when enqueue() asked for that wake-up

        // audio-thread-only
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render_client;
        ComPtr<IAudioClock> audio_clock;
        UINT64 clock_frequency = 0; // units of IAudioClock positions per second
        UINT32 buffer_frames = 0;
        std::uint64_t frames_written = 0; // since the stream started
        int device_channels = 2;
        float current_gain = 1.0F;
        float heard_gain = 1.0F;
        std::vector<float> scratch;
        std::optional<media_source> heard_source;
        bool stream_running = false;
        std::optional<std::int64_t> silent_since_ns; // running without queued audio since then
        std::int64_t stopped_since_ns = 0;           // the open endpoint is stopped since then
        int probe_attempts = 0;                      // attempts left to open an endpoint nobody waits for
        std::int64_t retry_at_ns = 0;                // no attempt to open before this time
        std::int64_t retry_delay_ns = retry_delay_min_ns;
        std::int64_t last_failure_ns = 0;
        failure_report failures;
        bool notifications_registered = false;
        std::int64_t next_poll_ns = 0;   // polling fallback
        std::wstring observed_endpoint;  // polling fallback: default endpoint at the previous poll

        [[nodiscard]] status open_endpoint();
        void close_endpoint();
        [[nodiscard]] status start_stream();
        [[nodiscard]] status stop_stream();
        [[nodiscard]] bool poll_default_endpoint();
        [[nodiscard]] source_demand demand_or_arm_wake();
        [[nodiscard]] bool render_block(UINT32 frame_count, double first_frame_time_ns, float *destination);
        [[nodiscard]] status serve_period(std::int64_t now_ns);
        [[nodiscard]] DWORD service(std::int64_t now_ns);
        [[nodiscard]] DWORD idle_wait_ms(std::int64_t now_ns, source_demand demand) const;
        void endpoint_failed(failure const &failed, std::int64_t now_ns);
        void schedule_retry(std::int64_t now_ns);
        void write_notes();
        void thread_main(std::promise<status> &ready);
    };

    audio_output::state::~state()
    {
        if (audio_event != nullptr)
        {
            CloseHandle(audio_event);
        }
        if (wake_event != nullptr)
        {
            CloseHandle(wake_event);
        }
    }

    // Opens the default render endpoint and leaves its stream stopped
    // (start_stream).  The stream is prepared completely before any member
    // changes: the local interface pointers release a half-opened client, so
    // every failure leaves the endpoint closed and service() opens it again.
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
        result = client3->SetEventHandle(audio_event);
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("IAudioClient::SetEventHandle", result));
        }
        UINT32 opened_buffer_frames = 0;
        result = client3->GetBufferSize(&opened_buffer_frames);
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("IAudioClient::GetBufferSize", result));
        }
        ComPtr<IAudioRenderClient> opened_render_client;
        result = client3->GetService(IID_PPV_ARGS(&opened_render_client));
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("GetService IAudioRenderClient", result));
        }
        ComPtr<IAudioClock> opened_clock;
        result = client3->GetService(IID_PPV_ARGS(&opened_clock));
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("GetService IAudioClock", result));
        }
        UINT64 opened_frequency = 0; // fixed for the lifetime of the stream
        result = opened_clock->GetFrequency(&opened_frequency);
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("IAudioClock::GetFrequency", result));
        }
        if (opened_buffer_frames == 0 || opened_frequency == 0)
        {
            return std::unexpected(make_failure("audio endpoint open", 0,
                                                std::format("unusable stream: buffer {} frames, clock frequency {}",
                                                            opened_buffer_frames, opened_frequency)));
        }
        REFERENCE_TIME stream_latency = 0;
        result = client3->GetStreamLatency(&stream_latency);
        if (FAILED(result))
        {
            // only device_latency_ns() depends on it: the stream itself is usable
            log::warning("audio output: {}", make_hresult_failure("IAudioClient::GetStreamLatency", result).describe());
            stream_latency = 0;
        }

        client = client3;
        render_client = opened_render_client;
        audio_clock = opened_clock;
        clock_frequency = opened_frequency;
        buffer_frames = opened_buffer_frames;
        device_channels = chosen->nChannels;
        frames_written = 0;
        scratch.assign(static_cast<std::size_t>(buffer_frames) * 2, 0.0F); // sized here, not in the period loop
        audio_device_format const opened{static_cast<int>(chosen->nSamplesPerSec), static_cast<int>(chosen->nChannels)};
        bool format_changed = false;
        {
            std::scoped_lock lock{mutex};
            format_changed = opened != device_format;
            device_format = opened;
            // what the render loop keeps queued in the endpoint is the whole
            // buffer, which a low-latency stream reports larger than its
            // period: the presentation delay must cover that, or a source
            // would always be judged too late to be played
            double const queued_frames = std::max(static_cast<double>(low_latency ? minimum_period : buffer_frames),
                                                  static_cast<double>(buffer_frames));
            double const period_ns = 1e9 * queued_frames / opened.sample_rate;
            latency_ns = static_cast<std::int64_t>(period_ns) + clock::hundred_ns_to_ns(stream_latency);
            if (format_changed)
            {
                for (source_timeline &timeline : timelines)
                {
                    timeline.clear();
                }
            }
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
        if (client && stream_running)
        {
            HRESULT const result = client->Stop();
            if (FAILED(result))
            {
                // the client is released next; after a device loss this repeats the reported failure
                log::debug("audio output: {}", make_hresult_failure("IAudioClient::Stop (closing)", result).describe());
            }
        }
        stream_running = false;
        silent_since_ns.reset();
        audio_clock.Reset();
        render_client.Reset();
        client.Reset();
        clock_frequency = 0;
        buffer_frames = 0;
        frames_written = 0;
    }

    // Starts the stopped stream.  It is empty and at device position zero
    // (new, or reset by stop_stream), so frames_written and the IAudioClock
    // position count from the same origin again.
    status audio_output::state::start_stream()
    {
        HRESULT const result = client->Start();
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("IAudioClient::Start", result));
        }
        frames_written = 0;
        silent_since_ns.reset();
        stream_running = true;
        return {};
    }

    // Stops the stream and empties it; Reset also returns the device position
    // to zero, so the wake-up needs nothing but Start().
    status audio_output::state::stop_stream()
    {
        stream_running = false;
        silent_since_ns.reset();
        HRESULT result = client->Stop();
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("IAudioClient::Stop", result));
        }
        result = client->Reset();
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("IAudioClient::Reset", result));
        }
        frames_written = 0;
        return {};
    }

    // Polling fallback for a failed notification registration: true when the
    // default render endpoint is not the one of the previous poll.  A failed
    // query is an identity of its own ("no endpoint"), so it is reported when
    // it begins and not on every poll.
    bool audio_output::state::poll_default_endpoint()
    {
        std::wstring identity;
        std::optional<failure> failed;
        ComPtr<IMMDevice> device;
        HRESULT result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(result))
        {
            failed = make_hresult_failure("GetDefaultAudioEndpoint (poll)", result);
        }
        else
        {
            LPWSTR id = nullptr;
            result = device->GetId(&id);
            std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> const id_owner{id, CoTaskMemFree};
            if (FAILED(result) || id == nullptr)
            {
                failed = make_hresult_failure("IMMDevice::GetId (poll)", FAILED(result) ? result : E_POINTER);
            }
            else
            {
                identity = id;
            }
        }
        if (failed)
        {
            identity = L"?" + std::to_wstring(failed->code); // no endpoint id starts like this
        }
        if (identity == observed_endpoint)
        {
            return false;
        }
        if (failed)
        {
            log::warning("audio output: {}", failed->describe());
        }
        observed_endpoint = std::move(identity);
        return true;
    }

    // What the sources need now.  When none has queued audio, the next
    // enqueue() is asked to wake the audio thread; both happen under the queue
    // lock, so no chunk can slip in between this check and the sleep.
    source_demand audio_output::state::demand_or_arm_wake()
    {
        std::scoped_lock lock{mutex};
        source_demand demand;
        demand.pending = std::ranges::any_of(timelines, &source_timeline::anchored);
        demand.live = std::ranges::any_of(live, std::identity{});
        if (!demand.pending)
        {
            wake_on_enqueue = true;
        }
        return demand;
    }

    // Fills one block of the endpoint buffer; true while a source has queued
    // audio.  Nothing is logged or allocated here: the lock is shared with
    // enqueue() on the network thread.
    bool audio_output::state::render_block(UINT32 frame_count, double first_frame_time_ns, float *destination)
    {
        scratch.assign(static_cast<std::size_t>(frame_count) * 2, 0.0F);
        float gain_target = heard_gain;
        bool source_pending = false;
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
                    // "audio source: ..." is written by write_notes()
                    heard_note.store(1 + static_cast<int>(source_index(*source)), std::memory_order_relaxed);
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
            source_pending = std::ranges::any_of(timelines, &source_timeline::anchored);
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
        return source_pending;
    }

    // One period of the running stream: the free part of the endpoint buffer
    // is filled with the audio that is due when it will be heard.  After
    // idle_stop_ns without queued audio the stream is stopped.
    status audio_output::state::serve_period(std::int64_t now_ns)
    {
        UINT32 padding = 0;
        HRESULT result = client->GetCurrentPadding(&padding);
        if (FAILED(result))
        {
            return std::unexpected(make_hresult_failure("IAudioClient::GetCurrentPadding", result));
        }
        if (padding < buffer_frames)
        {
            UINT32 const available = buffer_frames - padding;

            // time at which the next written frame will be heard
            UINT64 position = 0;
            UINT64 qpc_position = 0;
            result = audio_clock->GetPosition(&position, &qpc_position);
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure("IAudioClock::GetPosition", result));
            }
            double const played_frames = static_cast<double>(position) * device_format.sample_rate /
                                         static_cast<double>(clock_frequency);
            double const first_frame_time =
                static_cast<double>(clock::hundred_ns_to_ns(static_cast<std::int64_t>(qpc_position))) +
                (static_cast<double>(frames_written) - played_frames) * 1e9 / device_format.sample_rate;

            BYTE *buffer = nullptr;
            result = render_client->GetBuffer(available, &buffer);
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure("IAudioRenderClient::GetBuffer", result));
            }
            if (buffer == nullptr)
            {
                return std::unexpected(make_hresult_failure("IAudioRenderClient::GetBuffer (no buffer)", E_POINTER));
            }
            bool const source_pending = render_block(available, first_frame_time, reinterpret_cast<float *>(buffer));
            result = render_client->ReleaseBuffer(available, 0);
            if (FAILED(result))
            {
                return std::unexpected(make_hresult_failure("IAudioRenderClient::ReleaseBuffer", result));
            }
            if (frames_written == 0)
            {
                // first buffer after enqueue() woke the stopped stream: what the idle stop cost the first sound
                std::int64_t const requested_ns = wake_requested_ns.exchange(0, std::memory_order_relaxed);
                if (requested_ns != 0)
                {
                    restart_note_ns.store(clock::now_ns() - requested_ns, std::memory_order_relaxed);
                }
            }
            frames_written += available;
            if (source_pending)
            {
                silent_since_ns.reset();
            }
            else if (!silent_since_ns)
            {
                silent_since_ns = now_ns;
            }
        }
        if (!silent_since_ns || now_ns - *silent_since_ns < idle_stop_ns)
        {
            return {};
        }
        if (demand_or_arm_wake().pending)
        {
            silent_since_ns.reset(); // a chunk arrived after the last block
            return {};
        }
        status stopped = stop_stream();
        if (!stopped)
        {
            return stopped;
        }
        stopped_since_ns = now_ns;
        failures.flush();
        write_notes();
        log::info("audio output: no audio for {} s, stream stopped", idle_stop_ns / clock::ns_per_second);
        return {};
    }

    // One pass of the audio thread.  Returns how long it may sleep when
    // neither event is signalled: a stopped stream has no periodic wake-up,
    // only the deadline of its next step, if there is one.
    DWORD audio_output::state::service(std::int64_t now_ns)
    {
        bool changed = endpoint_changed.exchange(false);
        if (!notifications_registered && now_ns >= next_poll_ns)
        {
            next_poll_ns = now_ns + endpoint_poll_ns;
            changed = poll_default_endpoint() || changed;
        }
        if (changed)
        {
            // The open client belongs to the previous default device.  The
            // new one is opened at once, also while idle, so format() and the
            // decoders follow it; its stream starts with the next audio.
            if (client)
            {
                log::info("audio output: default endpoint changed");
            }
            close_endpoint();
            probe_attempts = endpoint_probe_attempts;
            retry_delay_ns = retry_delay_min_ns;
            retry_at_ns = now_ns;
        }

        if (stream_running)
        {
            status const served = serve_period(now_ns);
            if (!served)
            {
                endpoint_failed(served.error(), now_ns);
            }
            if (stream_running)
            {
                return period_watchdog_ms;
            }
        }

        // stopped or closed
        source_demand demand = demand_or_arm_wake();
        if (!client && (demand.pending || probe_attempts > 0) && now_ns >= retry_at_ns)
        {
            probe_attempts = std::max(probe_attempts - 1, 0);
            status const opened = open_endpoint();
            if (opened)
            {
                probe_attempts = 0;
                stopped_since_ns = now_ns;
                demand = demand_or_arm_wake(); // a new format emptied the queues
            }
            else
            {
                endpoint_failed(opened.error(), now_ns);
            }
        }
        if (client && demand.pending)
        {
            status const started = start_stream();
            if (started)
            {
                return 0; // the first buffer is written at once, not at the next period event
            }
            endpoint_failed(started.error(), now_ns);
        }
        std::int64_t const release_delay_ns = demand.live ? idle_release_live_ns : idle_release_ns;
        if (client && now_ns - stopped_since_ns >= release_delay_ns)
        {
            // no stream, no audio session: UxPlay leaves the volume mixer
            close_endpoint();
            log::info("audio output: no audio, endpoint released");
        }
        if (!client && !demand.pending && probe_attempts == 0)
        {
            failures.flush(); // nothing is tried again before an event: report a waiting count now
        }
        return idle_wait_ms(now_ns, demand);
    }

    DWORD audio_output::state::idle_wait_ms(std::int64_t now_ns, source_demand demand) const
    {
        std::optional<std::int64_t> deadline;
        if (client)
        {
            // then the endpoint is released
            deadline = stopped_since_ns + (demand.live ? idle_release_live_ns : idle_release_ns);
        }
        else if (demand.pending || probe_attempts > 0)
        {
            deadline = retry_at_ns; // back-off before the next attempt to open
        }
        if (!notifications_registered)
        {
            deadline = deadline ? std::min(*deadline, next_poll_ns) : next_poll_ns;
        }
        if (!deadline)
        {
            return INFINITE; // only an event ends this sleep
        }
        std::int64_t const remaining_ms = (*deadline - now_ns + clock::ns_per_ms - 1) / clock::ns_per_ms;
        return static_cast<DWORD>(std::clamp<std::int64_t>(remaining_ms, 0, longest_timed_wait_ms));
    }

    // A failed step of the endpoint: reported (rate-limited), then the
    // endpoint is closed and opened again after the back-off.
    void audio_output::state::endpoint_failed(failure const &failed, std::int64_t now_ns)
    {
        failures.add(failed, now_ns);
        schedule_retry(now_ns);
    }

    void audio_output::state::schedule_retry(std::int64_t now_ns)
    {
        close_endpoint();
        if (now_ns - last_failure_ns > retry_settled_ns)
        {
            retry_delay_ns = retry_delay_min_ns; // the endpoint worked, or was not needed, for a while
        }
        last_failure_ns = now_ns;
        retry_at_ns = now_ns + retry_delay_ns;
        retry_delay_ns = std::min(retry_delay_ns * 2, retry_delay_max_ns);
    }

    // Writes what the audio thread left for the log.  Called by enqueue() on
    // the network thread, and by the audio thread once its stream is stopped.
    void audio_output::state::write_notes()
    {
        if (heard_note.load(std::memory_order_relaxed) != 0)
        {
            int const heard = heard_note.exchange(0, std::memory_order_relaxed);
            if (heard != 0)
            {
                log::info("audio source: {}", media_source_name(static_cast<media_source>(heard - 1)));
            }
        }
        if (restart_note_ns.load(std::memory_order_relaxed) >= 0)
        {
            std::int64_t const restart_ns = restart_note_ns.exchange(-1, std::memory_order_relaxed);
            if (restart_ns >= 0)
            {
                log::info("audio output: stream restarted, first buffer {:.1f} ms after the audio was queued",
                          static_cast<double>(restart_ns) / 1e6);
            }
        }
    }

    void audio_output::state::thread_main(std::promise<status> &ready)
    {
        com_apartment const com;
        if (FAILED(com.result()))
        {
            ready.set_value(std::unexpected(make_hresult_failure("CoInitializeEx (audio thread)", com.result())));
            return;
        }
        system_tuning::mmcss_scope const mmcss{L"Pro Audio"};

        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(result))
        {
            ready.set_value(std::unexpected(make_hresult_failure("CoCreateInstance MMDeviceEnumerator", result)));
            return;
        }
        endpoint_notifier notifier{endpoint_changed, wake_event};
        result = enumerator->RegisterEndpointNotificationCallback(&notifier);
        notifications_registered = SUCCEEDED(result);
        if (!notifications_registered)
        {
            log::warning("audio output: {}; the default endpoint is polled every {} s instead",
                         make_hresult_failure("RegisterEndpointNotificationCallback", result).describe(),
                         endpoint_poll_ns / clock::ns_per_second);
            static_cast<void>(poll_default_endpoint()); // remembers the endpoint that is opened next
            next_poll_ns = clock::now_ns() + endpoint_poll_ns;
        }

        {
            std::int64_t const now_ns = clock::now_ns();
            status opened = open_endpoint();
            if (opened)
            {
                stopped_since_ns = now_ns;
            }
            else
            {
                // reported by create(); tried again a few times, then when
                // audio is queued or the default endpoint changes
                probe_attempts = endpoint_probe_attempts;
                schedule_retry(now_ns);
            }
            ready.set_value(std::move(opened));
        }

        while (!stopping.load())
        {
            DWORD const timeout = service(clock::now_ns());
            // the period event matters only while the stream runs
            HANDLE const handles[2] = {wake_event, audio_event};
            if (WaitForMultipleObjects(stream_running ? 2U : 1U, handles, FALSE, timeout) == WAIT_FAILED)
            {
                DWORD const wait_error = GetLastError();
                failures.add(make_win32_failure("WaitForMultipleObjects (audio thread)", wait_error), clock::now_ns());
                Sleep(std::min<DWORD>(timeout, wait_failure_pause_ms)); // keep serving, without spinning
            }
        }

        close_endpoint();
        failures.flush();
        write_notes();
        if (notifications_registered)
        {
            result = enumerator->UnregisterEndpointNotificationCallback(&notifier);
            if (FAILED(result))
            {
                log::warning("audio output: {}",
                             make_hresult_failure("UnregisterEndpointNotificationCallback", result).describe());
            }
        }
        enumerator.Reset();
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
    }

    outcome<std::unique_ptr<audio_output>> audio_output::create(audio_format_listener listener)
    {
        auto created = std::make_unique<state>();
        created->listener = std::move(listener);
        created->audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (created->audio_event == nullptr)
        {
            return std::unexpected(make_win32_failure("CreateEventW (audio period)", GetLastError()));
        }
        created->wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (created->wake_event == nullptr)
        {
            return std::unexpected(make_win32_failure("CreateEventW (audio wake)", GetLastError()));
        }
        std::promise<status> ready;
        std::future<status> ready_result = ready.get_future();
        auto output = std::unique_ptr<audio_output>{new audio_output{std::move(created)}};
        state *const running = output->impl.get();
        running->thread = std::thread{[running, &ready]
        {
            running->thread_main(ready);
        }};
        status const started = ready_result.get();
        if (!started)
        {
            // The output stays usable: the audio thread opens the endpoint
            // again when audio is queued or the default endpoint changes (it
            // has ended only if COM itself could not be set up).
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
        bool overflowed = false;
        bool wake = false;
        {
            std::scoped_lock lock{impl->mutex};
            if (sample_rate != impl->device_format.sample_rate || stereo_samples.empty())
            {
                return;
            }
            impl->live[source_index(source)] = true;
            double const frame_ns = 1e9 / sample_rate;
            source_timeline &timeline = impl->timelines[source_index(source)];
            auto const chunk_time = static_cast<double>(media_time_ns);
            std::size_t skip_frames = 0;
            std::size_t gap_frames = 0;
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
                    gap_frames = static_cast<std::size_t>(delta / frame_ns); // silence up to the chunk
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
            std::span<float const> const fresh = stereo_samples.subspan(offset);
            // The queue is one block of memory: the overflow is decided before
            // the copy, with the size the queue would have after it.
            if ((timeline.samples.size() + gap_frames * 2 + fresh.size()) / 2 > max_queued_frames)
            {
                timeline.clear();
                overflowed = true;
            }
            else
            {
                timeline.samples.append_silence(gap_frames * 2);
                timeline.samples.append(fresh);
            }
            if (impl->wake_on_enqueue && timeline.anchored)
            {
                impl->wake_on_enqueue = false;
                wake = true;
            }
        }
        // nothing is logged under the lock: the audio thread waits for it
        if (wake)
        {
            // the stream is stopped (idle): the audio thread starts it for this chunk
            impl->wake_requested_ns.store(clock::now_ns(), std::memory_order_relaxed);
            if (SetEvent(impl->wake_event) == FALSE)
            {
                log::error("audio output: {}", make_win32_failure("SetEvent (audio wake)", GetLastError()).describe());
            }
        }
        if (overflowed)
        {
            log::warning("audio queue overflow for {}: resynchronizing", media_source_name(source));
        }
        impl->write_notes();
    }

    void audio_output::set_delay(media_source source, std::int64_t delay_ns)
    {
        impl->delays[source_index(source)].store(delay_ns, std::memory_order_relaxed);
    }

    void audio_output::release_source(media_source source)
    {
        {
            std::scoped_lock lock{impl->mutex};
            impl->timelines[source_index(source)].clear();
            impl->live[source_index(source)] = false;
        }
        // a stopped stream is released sooner once no source is in use
        if (SetEvent(impl->wake_event) == FALSE)
        {
            log::error("audio output: {}", make_win32_failure("SetEvent (source released)", GetLastError()).describe());
        }
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
