module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <libusb.h>

module uxplay_usb_mirror;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_clock;
import uxplay_media_types;
import uxplay_media_engine;
import uxplay_video_decoder;
import uxplay_audio_decoder;
import uxplay_qt_protocol;

namespace uxplay
{
    namespace
    {
        constexpr std::uint16_t apple_vendor_id = 0x05ac;
        constexpr std::uint8_t vendor_class = 0xff;
        constexpr std::uint8_t usbmux_subclass = 0xfe;
        constexpr std::uint8_t quicktime_subclass = 0x2a;
        constexpr std::uint8_t quicktime_request = 0x52;
        // bmRequestType 0x40: host-to-device, vendor, recipient device
        constexpr std::uint8_t vendor_device_request =
            static_cast<std::uint8_t>(LIBUSB_REQUEST_TYPE_VENDOR) | static_cast<std::uint8_t>(LIBUSB_RECIPIENT_DEVICE);
        constexpr int read_transfer_count = 4;
        constexpr int read_transfer_size = 1024 * 1024;
        constexpr int control_timeout_ms = 1000;
        constexpr auto reenumeration_timeout = std::chrono::seconds{8};
        constexpr auto scan_interval = std::chrono::milliseconds{1500};
        constexpr auto unplug_grace = std::chrono::seconds{6};
        constexpr auto retry_backoff = std::chrono::seconds{10};
        // the phone answers within milliseconds once the capture configuration
        // is selected; silence means the capture state of the phone is stale
        constexpr auto stream_start_timeout = std::chrono::seconds{5};
        constexpr auto stream_retry_backoff = std::chrono::seconds{2};
        constexpr int max_endpoint_stalls = 8;

        enum class quicktime_filter
        {
            any,
            required,
            absent
        };

        std::string usb_error(int code)
        {
            return std::format("{} ({})", libusb_error_name(code), libusb_strerror(code));
        }

        failure usb_failure(std::string_view stage, int code)
        {
            return make_failure(stage, code, usb_error(code));
        }

        struct config_layout
        {
            int quicktime_config = -1;
            int quicktime_interface = -1;
            std::uint8_t endpoint_in = 0;
            std::uint8_t endpoint_out = 0;
            bool has_usbmux = false;
        };

        config_layout inspect_configs(libusb_device *device)
        {
            config_layout layout;
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(device, &descriptor) != LIBUSB_SUCCESS)
            {
                return layout;
            }
            for (std::uint8_t index = 0; index < descriptor.bNumConfigurations; ++index)
            {
                libusb_config_descriptor *config = nullptr;
                if (libusb_get_config_descriptor(device, index, &config) != LIBUSB_SUCCESS || config == nullptr)
                {
                    continue;
                }
                for (int interface_index = 0; interface_index < config->bNumInterfaces; ++interface_index)
                {
                    libusb_interface const &usb_interface = config->interface[interface_index];
                    for (int alt = 0; alt < usb_interface.num_altsetting; ++alt)
                    {
                        libusb_interface_descriptor const &setting = usb_interface.altsetting[alt];
                        if (setting.bInterfaceClass != vendor_class)
                        {
                            continue;
                        }
                        if (setting.bInterfaceSubClass == usbmux_subclass)
                        {
                            layout.has_usbmux = true;
                        }
                        if (setting.bInterfaceSubClass == quicktime_subclass)
                        {
                            layout.quicktime_config = config->bConfigurationValue;
                            layout.quicktime_interface = setting.bInterfaceNumber;
                            for (int endpoint = 0; endpoint < setting.bNumEndpoints; ++endpoint)
                            {
                                libusb_endpoint_descriptor const &endpoint_descriptor = setting.endpoint[endpoint];
                                if ((endpoint_descriptor.bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_BULK)
                                {
                                    continue;
                                }
                                if ((endpoint_descriptor.bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0)
                                {
                                    layout.endpoint_in = endpoint_descriptor.bEndpointAddress;
                                }
                                else
                                {
                                    layout.endpoint_out = endpoint_descriptor.bEndpointAddress;
                                }
                            }
                        }
                    }
                }
                libusb_free_config_descriptor(config);
            }
            return layout;
        }

        // Smallest observed (arrival - device time) with a slow upward leak, so
        // the mapping follows clock drift but never jumps on jitter.
        class delay_floor_tracker
        {
        public:
            std::int64_t update(std::int64_t observed_ns)
            {
                if (!initialized || observed_ns < floor_ns)
                {
                    floor_ns = observed_ns;
                    initialized = true;
                }
                else
                {
                    floor_ns += (observed_ns - floor_ns) / 4096;
                }
                return floor_ns;
            }

            void reset()
            {
                initialized = false;
            }

        private:
            bool initialized = false;
            std::int64_t floor_ns = 0;
        };

    }

    struct usb_mirror::state
    {
        media_engine *engine = nullptr;
        usb_mirror_events events;
        qt::host_display display;
        libusb_context *context = nullptr;
        std::thread worker;
        std::mutex mutex;
        std::condition_variable wake;
        bool rescan_requested = true;
        bool stopping = false;
        // internal: stop resubmitting reads (end of every session)
        std::atomic<bool> stop_session_requested{false};
        // set by stop_current_session(): the user closed the USB video
        std::atomic<bool> user_stop_requested{false};
        std::atomic<bool> session_active{false};
        std::atomic<bool> mirroring_enabled{true};
        // stopped by the user: not restarted until the device has been away
        // from the bus for unplug_grace (its own re-enumeration is shorter)
        std::map<std::string, std::chrono::steady_clock::time_point> suppressed_serials;
        // failed to start: retried after retry_backoff
        std::map<std::string, std::chrono::steady_clock::time_point> retry_after;

        // per-session (worker thread only)
        libusb_device_handle *handle = nullptr;
        config_layout layout;
        std::unique_ptr<qt::session> protocol;
        qt::packet_reassembler reassembler;
        std::vector<libusb_transfer *> reads;
        int reads_in_flight = 0;
        int writes_in_flight = 0;
        std::set<libusb_transfer *> pending_writes; // cancelled at the end of a session
        bool device_lost = false;
        delay_floor_tracker video_delay;
        delay_floor_tracker audio_delay;
        bool video_configured = false;
        bool audio_configured = false;
        std::uint64_t protocol_errors = 0;
        std::uint64_t bytes_received = 0;
        int endpoint_stalls = 0;
        bool quick_retry = false; // the session failed after a phone reset: restart soon

        void worker_loop();
        std::optional<std::pair<libusb_device *, std::string>> find_device(quicktime_filter filter,
                                                                           std::string const &serial);
        std::optional<std::pair<libusb_device *, std::string>> wait_for_device(quicktime_filter filter,
                                                                               std::string const &serial);
        status run_session(libusb_device *device, std::string const &serial);
        status open_quicktime(libusb_device *device, std::string const &serial);
        status send_quicktime_request(libusb_device *device, std::uint16_t index, std::string_view stage);
        void send(std::vector<std::uint8_t> packet);
        void on_read(libusb_transfer *transfer);
        void finish_session(std::string const &serial);

        static void LIBUSB_CALL read_callback(libusb_transfer *transfer);
        static void LIBUSB_CALL write_callback(libusb_transfer *transfer);
    };

    void LIBUSB_CALL usb_mirror::state::read_callback(libusb_transfer *transfer)
    {
        static_cast<state *>(transfer->user_data)->on_read(transfer);
    }

    void LIBUSB_CALL usb_mirror::state::write_callback(libusb_transfer *transfer)
    {
        auto *owner = static_cast<state *>(transfer->user_data);
        --owner->writes_in_flight;
        owner->pending_writes.erase(transfer); // freed by libusb after this callback
        if (transfer->status != LIBUSB_TRANSFER_COMPLETED && transfer->status != LIBUSB_TRANSFER_CANCELLED)
        {
            if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE)
            {
                owner->device_lost = true;
            }
            else if (++owner->protocol_errors <= 5)
            {
                log::warning("USB write failed: status {}", static_cast<int>(transfer->status));
            }
        }
        // buffer and transfer are freed by libusb (FREE_BUFFER | FREE_TRANSFER)
    }

    void usb_mirror::state::send(std::vector<std::uint8_t> packet)
    {
        if (handle == nullptr || device_lost)
        {
            return;
        }
        libusb_transfer *transfer = libusb_alloc_transfer(0);
        auto *buffer = static_cast<unsigned char *>(std::malloc(packet.size()));
        if (transfer == nullptr || buffer == nullptr)
        {
            libusb_free_transfer(transfer);
            std::free(buffer);
            return;
        }
        std::memcpy(buffer, packet.data(), packet.size());
        libusb_fill_bulk_transfer(transfer, handle, layout.endpoint_out, buffer, static_cast<int>(packet.size()),
                                  write_callback, this, 2000);
        transfer->flags = LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER;
        int const result = libusb_submit_transfer(transfer);
        if (result != LIBUSB_SUCCESS)
        {
            libusb_free_transfer(transfer); // FREE_BUFFER releases the buffer too
            if (result == LIBUSB_ERROR_NO_DEVICE)
            {
                device_lost = true;
            }
            else if (++protocol_errors <= 5)
            {
                log::warning("USB write submit: {}", usb_error(result));
            }
            return;
        }
        ++writes_in_flight;
        pending_writes.insert(transfer);
    }

    void usb_mirror::state::on_read(libusb_transfer *transfer)
    {
        if (transfer->status == LIBUSB_TRANSFER_COMPLETED && transfer->actual_length > 0)
        {
            if (bytes_received == 0)
            {
                log::info("USB: the iPhone started the screen-capture stream");
            }
            bytes_received += static_cast<std::uint64_t>(transfer->actual_length);
            std::span<std::uint8_t const> const data{transfer->buffer, static_cast<std::size_t>(transfer->actual_length)};
            status const appended = reassembler.append(data, [this](std::span<std::uint8_t const> packet)
            {
                status const handled = protocol->handle_packet(packet);
                if (!handled && ++protocol_errors <= 10)
                {
                    log::warning("USB protocol: {}", handled.error().describe());
                }
            });
            if (!appended)
            {
                log::error("USB stream: {}", appended.error().describe());
            }
        }
        else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE || transfer->status == LIBUSB_TRANSFER_ERROR)
        {
            device_lost = true;
        }
        else if (transfer->status == LIBUSB_TRANSFER_STALL && ++endpoint_stalls <= max_endpoint_stalls)
        {
            log::warning("USB: capture endpoint stalled ({} of {})", endpoint_stalls, max_endpoint_stalls);
        }

        // a stalling endpoint completes every resubmission at once: give up
        // instead of spinning, the session is restarted from scratch
        bool const resubmit = transfer->status != LIBUSB_TRANSFER_CANCELLED && !device_lost && !stop_session_requested &&
                              endpoint_stalls <= max_endpoint_stalls;
        if (resubmit)
        {
            int const result = libusb_submit_transfer(transfer);
            if (result == LIBUSB_SUCCESS)
            {
                return;
            }
            device_lost = result == LIBUSB_ERROR_NO_DEVICE || device_lost;
            log::warning("USB read resubmit: {}", usb_error(result));
        }
        --reads_in_flight;
    }

    std::optional<std::pair<libusb_device *, std::string>> usb_mirror::state::find_device(quicktime_filter filter,
                                                                                         std::string const &serial)
    {
        libusb_device **list = nullptr;
        ssize_t const count = libusb_get_device_list(context, &list);
        if (count < 0)
        {
            return std::nullopt;
        }
        std::optional<std::pair<libusb_device *, std::string>> found;
        std::set<std::string> present;
        for (ssize_t index = 0; index < count && list != nullptr; ++index)
        {
            libusb_device *device = list[index];
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(device, &descriptor) != LIBUSB_SUCCESS ||
                descriptor.idVendor != apple_vendor_id)
            {
                continue;
            }
            config_layout const layout_found = inspect_configs(device);
            if (!layout_found.has_usbmux)
            {
                continue; // not an iOS device (keyboard, adapter, ...)
            }
            libusb_device_handle *probe = nullptr;
            int const opened = libusb_open(device, &probe);
            if (opened != LIBUSB_SUCCESS)
            {
                static std::atomic<int> reported{0};
                if (reported.fetch_add(1) < 3)
                {
                    // Do not advise a libusb driver swap: it replaces Apple's
                    // driver and breaks AirPlay over the USB cable, and the
                    // QuickTime capture does not stream on iOS 16+ anyway.
                    log::warning("legacy QuickTime USB capture: the iPhone cannot be opened ({}): it uses Apple's "
                                 "driver, which AirPlay over the USB cable needs; this capture streams only on "
                                 "iOS 15 and earlier",
                                 usb_error(opened));
                }
                continue;
            }
            unsigned char text[128] = {};
            int const length = libusb_get_string_descriptor_ascii(probe, descriptor.iSerialNumber, text, sizeof(text));
            libusb_close(probe);
            if (length <= 0)
            {
                continue;
            }
            // recent iPhones pad the serial number string with NUL characters
            std::string_view serial_text{reinterpret_cast<char const *>(text), static_cast<std::size_t>(length)};
            serial_text = serial_text.substr(0, serial_text.find('\0'));
            if (serial_text.empty())
            {
                continue;
            }
            std::string const device_serial{serial_text};
            present.insert(device_serial);
            if (found)
            {
                continue;
            }
            if (!serial.empty() && device_serial != serial)
            {
                continue;
            }
            if (serial.empty() && suppressed_serials.contains(device_serial))
            {
                suppressed_serials[device_serial] = std::chrono::steady_clock::now(); // still present
                continue;
            }
            if (serial.empty())
            {
                if (auto const retry = retry_after.find(device_serial);
                    retry != retry_after.end() && std::chrono::steady_clock::now() < retry->second)
                {
                    continue;
                }
            }
            if ((filter == quicktime_filter::required && layout_found.quicktime_config < 0) ||
                (filter == quicktime_filter::absent && layout_found.quicktime_config >= 0))
            {
                continue;
            }
            found = std::pair{libusb_ref_device(device), device_serial};
        }
        libusb_free_device_list(list, 1);
        if (serial.empty())
        {
            // a suppressed device that stayed away long enough was unplugged
            auto const now = std::chrono::steady_clock::now();
            std::erase_if(suppressed_serials, [&present, now](auto const &entry)
            {
                return !present.contains(entry.first) && now - entry.second > unplug_grace;
            });
            std::erase_if(retry_after, [&present](auto const &entry)
            {
                return !present.contains(entry.first);
            });
        }
        return found;
    }

    status usb_mirror::state::send_quicktime_request(libusb_device *device, std::uint16_t index, std::string_view stage)
    {
        libusb_device_handle *control = nullptr;
        int result = libusb_open(device, &control);
        if (result != LIBUSB_SUCCESS)
        {
            return std::unexpected(usb_failure(stage, result));
        }
        // the device disconnects and re-enumerates with the new configuration set
        result = libusb_control_transfer(control, vendor_device_request, quicktime_request, 0x0000, index, nullptr, 0,
                                         control_timeout_ms);
        libusb_close(control);
        log::debug("USB: {} -> {}", stage, result < 0 ? usb_error(result) : std::string{"accepted"});
        if (result < 0 && result != LIBUSB_ERROR_NO_DEVICE && result != LIBUSB_ERROR_PIPE && result != LIBUSB_ERROR_IO)
        {
            return std::unexpected(usb_failure(stage, result));
        }
        return {};
    }

    std::optional<std::pair<libusb_device *, std::string>> usb_mirror::state::wait_for_device(quicktime_filter filter,
                                                                                             std::string const &serial)
    {
        auto const deadline = std::chrono::steady_clock::now() + reenumeration_timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{250});
            if (auto found = find_device(filter, serial); found)
            {
                return found;
            }
        }
        return std::nullopt;
    }

    // Takes over the reference of `device` (released on every path).
    status usb_mirror::state::open_quicktime(libusb_device *device, std::string const &serial)
    {
        std::unique_ptr<libusb_device, decltype(&libusb_unref_device)> current{device, libusb_unref_device};
        layout = inspect_configs(current.get());
        if (layout.quicktime_config >= 0)
        {
            // Left in screen-capture mode by a session that did not end
            // cleanly (crash, forced exit, another tool): the phone starts a
            // new stream only after the mode was switched off and on again.
            log::info("USB: the iPhone is still in screen-capture mode from an earlier session, resetting it");
            status const disabled =
                send_quicktime_request(current.get(), 0x0000, "disable stale QuickTime configuration");
            if (!disabled)
            {
                return disabled;
            }
            // no reference may survive the re-enumeration: libusb would keep
            // serving the cached descriptors of the old configuration set
            current.reset();
            if (auto plain = wait_for_device(quicktime_filter::absent, serial); plain)
            {
                current.reset(plain->first);
            }
            else
            {
                log::warning("USB: the iPhone stayed in screen-capture mode, using it as it is");
                auto as_is = find_device(quicktime_filter::required, serial);
                if (!as_is)
                {
                    return std::unexpected(make_failure("QuickTime reset", 0, "the iPhone disappeared"));
                }
                current.reset(as_is->first);
            }
            layout = inspect_configs(current.get());
        }
        if (layout.quicktime_config < 0)
        {
            status const enabled = send_quicktime_request(current.get(), 0x0002, "enable QuickTime configuration");
            if (!enabled)
            {
                return enabled;
            }
            log::info("USB: QuickTime configuration requested, waiting for the iPhone to reconnect");
            current.reset();
            auto reconnected = wait_for_device(quicktime_filter::required, serial);
            if (!reconnected)
            {
                return std::unexpected(make_failure("QuickTime re-enumeration", 0,
                                                    "the iPhone did not reconnect with the screen-capture "
                                                    "configuration (unlock the phone and trust this computer)"));
            }
            current.reset(reconnected->first);
            layout = inspect_configs(current.get());
        }

        int result = libusb_open(current.get(), &handle);
        if (result != LIBUSB_SUCCESS)
        {
            handle = nullptr;
            return std::unexpected(usb_failure("libusb_open (QuickTime)", result));
        }
        if (layout.quicktime_interface < 0 || layout.endpoint_in == 0 || layout.endpoint_out == 0)
        {
            return std::unexpected(make_failure("QuickTime interface", 0, "bulk endpoints not found"));
        }
        int active_config = 0;
        libusb_get_configuration(handle, &active_config);
        // selected also when already active: SET_CONFIGURATION resets the
        // interface state (toggles, halts) of the capture function
        result = libusb_set_configuration(handle, layout.quicktime_config);
        if (result != LIBUSB_SUCCESS && active_config != layout.quicktime_config)
        {
            return std::unexpected(usb_failure("libusb_set_configuration (QuickTime)", result));
        }
        if (result != LIBUSB_SUCCESS)
        {
            log::debug("USB: re-selecting the active configuration: {}", usb_error(result));
        }
        result = libusb_claim_interface(handle, layout.quicktime_interface);
        if (result != LIBUSB_SUCCESS)
        {
            return std::unexpected(usb_failure("libusb_claim_interface (QuickTime)", result));
        }
        libusb_clear_halt(handle, layout.endpoint_in);
        libusb_clear_halt(handle, layout.endpoint_out);
        log::info("USB: QuickTime capture interface {} (config {} after {}, in {:#04x}, out {:#04x}) ready",
                  layout.quicktime_interface, layout.quicktime_config, active_config, layout.endpoint_in,
                  layout.endpoint_out);
        return {};
    }

    void usb_mirror::state::finish_session(std::string const &serial)
    {
        if (handle != nullptr)
        {
            if (layout.quicktime_interface >= 0)
            {
                libusb_release_interface(handle, layout.quicktime_interface);
            }
            // back to the normal configuration set (the phone re-enumerates)
            libusb_control_transfer(handle, vendor_device_request, quicktime_request,
                                    0x0000, 0x0000, nullptr, 0, control_timeout_ms);
            libusb_close(handle);
            handle = nullptr;
        }
        for (libusb_transfer *transfer : reads)
        {
            libusb_free_transfer(transfer);
        }
        reads.clear();
        protocol.reset();
        reassembler = qt::packet_reassembler{};
        if (video_configured)
        {
            engine->video_stop(media_source::usb);
        }
        if (audio_configured)
        {
            engine->audio_stop(media_source::usb);
        }
        video_configured = false;
        audio_configured = false;
        bool const was_active = session_active.exchange(false);
        if (was_active && events.on_stopped)
        {
            events.on_stopped();
        }
        log::info("USB mirroring of {} ended", serial);
    }

    status usb_mirror::state::run_session(libusb_device *device, std::string const &serial)
    {
        device_lost = false;
        protocol_errors = 0;
        bytes_received = 0;
        endpoint_stalls = 0;
        quick_retry = false;
        reads_in_flight = 0;
        writes_in_flight = 0;
        video_delay.reset();
        audio_delay.reset();
        stop_session_requested.store(false);
        user_stop_requested.store(false);

        status const opened = open_quicktime(device, serial);
        if (!opened)
        {
            finish_session(serial);
            return opened;
        }

        qt::session_handlers handlers;
        handlers.send = [this](std::vector<std::uint8_t> packet)
        {
            send(std::move(packet));
        };
        handlers.on_video_format = [this](qt::video_description const &format)
        {
            bool const hevc = format.codec == qt::fourcc("hvc1") || format.codec == qt::fourcc("hev1");
            engine->video_configure(media_source::usb, hevc ? video_codec::h265 : video_codec::h264, format.width,
                                    format.height);
            video_configured = true;
            if (!session_active.exchange(true) && events.on_started)
            {
                events.on_started();
            }
        };
        handlers.on_video_sample = [this](qt::sample_buffer &&sample, std::int64_t arrival_ns)
        {
            if (!sample.presentation_time || !video_configured)
            {
                return;
            }
            std::int64_t const device_ns = sample.presentation_time->to_ns();
            std::int64_t const offset = video_delay.update(arrival_ns - device_ns);
            engine->video_access_unit(media_source::usb, sample.data, device_ns + offset, arrival_ns);
        };
        handlers.on_audio_format = [this](qt::audio_stream_description const &format)
        {
            if (audio_configured)
            {
                return;
            }
            bool const signed_integer = (format.format_flags & 0x4U) != 0 && (format.format_flags & 0x1U) == 0;
            if (format.format_id != qt::fourcc("lpcm") || format.bits_per_channel != 16 || !signed_integer ||
                format.channels == 0)
            {
                log::warning("USB audio format unsupported (bits {}, flags {:#x}): audio disabled",
                             format.bits_per_channel, format.format_flags);
                return;
            }
            engine->audio_configure(media_source::usb, audio_codec::pcm_s16le, static_cast<int>(format.sample_rate),
                                    static_cast<int>(format.channels), true, 0.0);
            audio_configured = true;
        };
        handlers.on_audio_sample = [this](qt::sample_buffer &&sample, std::int64_t arrival_ns)
        {
            if (!sample.presentation_time || !audio_configured)
            {
                return;
            }
            std::int64_t const device_ns = sample.presentation_time->to_ns();
            std::int64_t const offset = audio_delay.update(arrival_ns - device_ns);
            engine->audio_packet(media_source::usb, sample.data, device_ns + offset, arrival_ns);
        };
        protocol = std::make_unique<qt::session>(std::move(handlers), display);

        for (int index = 0; index < read_transfer_count; ++index)
        {
            libusb_transfer *transfer = libusb_alloc_transfer(0);
            auto *buffer = static_cast<unsigned char *>(std::malloc(read_transfer_size));
            if (transfer == nullptr || buffer == nullptr)
            {
                libusb_free_transfer(transfer);
                std::free(buffer);
                break;
            }
            libusb_fill_bulk_transfer(transfer, handle, layout.endpoint_in, buffer, read_transfer_size, read_callback,
                                      this, 0);
            transfer->flags = LIBUSB_TRANSFER_FREE_BUFFER;
            int const result = libusb_submit_transfer(transfer);
            if (result != LIBUSB_SUCCESS)
            {
                libusb_free_transfer(transfer);
                log::error("USB read submit: {}", usb_error(result));
                break;
            }
            reads.push_back(transfer);
            ++reads_in_flight;
        }
        if (reads_in_flight == 0)
        {
            finish_session(serial);
            return std::unexpected(make_failure("USB reads", 0, "no bulk read could be started"));
        }
        log::info("USB mirroring of {} started (waiting for the iPhone screen stream)", serial);

        // event loop until the device leaves, the user stops, or we shut down
        auto const reads_started = std::chrono::steady_clock::now();
        bool silent_phone = false;
        for (;;)
        {
            timeval timeout{0, 100'000};
            libusb_handle_events_timeout_completed(context, &timeout, nullptr);
            bool stop_now = false;
            {
                std::scoped_lock lock{mutex};
                stop_now = stopping || user_stop_requested.load();
            }
            if (device_lost || stop_now || reads_in_flight == 0)
            {
                break;
            }
            if (bytes_received == 0 && std::chrono::steady_clock::now() - reads_started > stream_start_timeout)
            {
                silent_phone = true;
                break;
            }
        }

        if (!device_lost && protocol && bytes_received > 0)
        {
            // graceful stop: HPA0/HPD0, then wait briefly for the clock releases
            protocol->request_stop();
            auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{600};
            while (std::chrono::steady_clock::now() < deadline && protocol->released_clocks() < 2 && !device_lost)
            {
                timeval timeout{0, 20'000};
                libusb_handle_events_timeout_completed(context, &timeout, nullptr);
            }
        }
        stop_session_requested.store(true); // reads are no longer resubmitted
        for (libusb_transfer *transfer : reads)
        {
            libusb_cancel_transfer(transfer);
        }
        // copy: completions of cancelled writes erase themselves from the set
        for (libusb_transfer *transfer : std::vector<libusb_transfer *>(pending_writes.begin(), pending_writes.end()))
        {
            libusb_cancel_transfer(transfer);
        }
        auto const drain_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{2500};
        while ((reads_in_flight > 0 || writes_in_flight > 0) && std::chrono::steady_clock::now() < drain_deadline)
        {
            timeval timeout{0, 20'000};
            libusb_handle_events_timeout_completed(context, &timeout, nullptr);
        }
        if (reads_in_flight > 0 || writes_in_flight > 0)
        {
            log::warning("USB: {} reads and {} writes still pending after cancellation", reads_in_flight,
                         writes_in_flight);
        }
        if (silent_phone && handle != nullptr && !device_lost)
        {
            // The capture function of the phone is stuck (typically a host
            // that exited without stopping it); it ignores the configuration
            // requests, so the port is cycled: the phone re-enumerates fresh.
            int const reset = libusb_reset_device(handle);
            log::info("USB: the iPhone sent nothing, port reset: {}",
                      reset == LIBUSB_SUCCESS || reset == LIBUSB_ERROR_NOT_FOUND ? "the phone re-enumerates"
                                                                                  : usb_error(reset));
        }
        bool const lost = device_lost;
        bool const stalled = endpoint_stalls > max_endpoint_stalls;
        quick_retry = silent_phone || stalled; // finish_session switched the capture mode off
        finish_session(serial);
        if (lost)
        {
            log::info("USB: iPhone disconnected");
            return {};
        }
        if (silent_phone)
        {
            return std::unexpected(make_failure("USB screen stream", 0,
                                                std::format("the iPhone sent no data within {} s after the capture "
                                                            "configuration was selected",
                                                            stream_start_timeout.count())));
        }
        if (stalled)
        {
            return std::unexpected(make_failure("USB screen stream", LIBUSB_TRANSFER_STALL,
                                                "the capture endpoint keeps stalling"));
        }
        return {};
    }

    void usb_mirror::state::worker_loop()
    {
        for (;;)
        {
            {
                std::unique_lock lock{mutex};
                wake.wait_for(lock, scan_interval, [this]
                {
                    return stopping || rescan_requested;
                });
                if (stopping)
                {
                    return;
                }
                rescan_requested = false;
            }
            if (!mirroring_enabled.load())
            {
                continue;
            }
            auto candidate = find_device(quicktime_filter::any, {});
            if (!candidate)
            {
                continue;
            }
            auto [device, serial] = *candidate;
            log::info("USB: iPhone {} detected, starting USB mirroring (USB has priority over Wi-Fi)", serial);
            status const result = run_session(device, serial);
            bool user_stopped = false;
            {
                std::scoped_lock lock{mutex};
                user_stopped = user_stop_requested.exchange(false) && !stopping;
                if (user_stopped)
                {
                    suppressed_serials[serial] = std::chrono::steady_clock::now();
                }
                else if (!result)
                {
                    // e.g. phone locked or not trusted yet: try again shortly; a
                    // silent phone was reset by finish_session and restarts at once
                    retry_after[serial] = std::chrono::steady_clock::now() + (quick_retry ? stream_retry_backoff
                                                                                              : retry_backoff);
                }
            }
            if (!result)
            {
                log::error("USB mirroring of {}: {} (retrying in {} s)", serial, result.error().describe(),
                           std::chrono::duration_cast<std::chrono::seconds>(quick_retry ? stream_retry_backoff
                                                                                           : retry_backoff)
                               .count());
            }
        }
    }

    usb_mirror::usb_mirror(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    usb_mirror::~usb_mirror()
    {
        {
            std::scoped_lock lock{impl->mutex};
            impl->stopping = true;
        }
        impl->wake.notify_all();
        if (impl->worker.joinable())
        {
            impl->worker.join();
        }
        libusb_exit(impl->context);
    }

    outcome<std::unique_ptr<usb_mirror>> usb_mirror::create(media_engine &engine, usb_mirror_events events,
                                                            int host_display_width, int host_display_height)
    {
        auto created = std::make_unique<state>();
        created->engine = &engine;
        created->events = std::move(events);
        created->display = qt::host_display{host_display_width > 0 ? host_display_width : 1920,
                                            host_display_height > 0 ? host_display_height : 1200};
        int const result = libusb_init_context(&created->context, nullptr, 0);
        if (result != LIBUSB_SUCCESS)
        {
            return std::unexpected(usb_failure("libusb_init_context", result));
        }
        // libusb's own logging repeats every failed open on each rescan
        // (unbounded console spam); failures are reported once, with their
        // libusb error code, by this module instead.
        libusb_set_option(created->context, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);
        state *const running = created.get();
        created->worker = std::thread{[running]
        {
            running->worker_loop();
        }};
        return std::unique_ptr<usb_mirror>{new usb_mirror{std::move(created)}};
    }

    void usb_mirror::rescan()
    {
        {
            std::scoped_lock lock{impl->mutex};
            impl->rescan_requested = true;
        }
        impl->wake.notify_all();
    }

    void usb_mirror::stop_current_session()
    {
        std::scoped_lock lock{impl->mutex};
        if (impl->session_active.load())
        {
            impl->user_stop_requested.store(true);
        }
    }

    bool usb_mirror::active() const
    {
        return impl->session_active.load();
    }

    void usb_mirror::set_enabled(bool enabled)
    {
        {
            std::scoped_lock lock{impl->mutex};
            impl->mirroring_enabled.store(enabled);
            if (enabled)
            {
                impl->suppressed_serials.clear();
                impl->retry_after.clear();
                impl->rescan_requested = true;
            }
            else if (impl->session_active.load())
            {
                impl->user_stop_requested.store(true);
            }
        }
        impl->wake.notify_all();
        log::info("legacy QuickTime USB capture {}", enabled ? "enabled" : "disabled");
    }

    bool usb_mirror::enabled() const
    {
        return impl->mirroring_enabled.load();
    }
}
