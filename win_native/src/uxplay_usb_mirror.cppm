// USB (QuickTime screen capture) mirroring of an iPhone through libusb.
//
// When an Apple device with a usbmux interface appears, the hidden
// QuickTime USB configuration is enabled (vendor control request 0x52),
// selected, and its bulk endpoints are read with several overlapping
// asynchronous transfers.  Decoded media goes to the media engine as the
// highest-priority source, so a plugged-in phone always wins over Wi-Fi.
//
// Device timestamps (video: host time in ns, audio: sample clock) are mapped
// onto the local clock with a minimum-transport-delay tracker per stream.
export module uxplay_usb_mirror;

import std;
import uxplay_status;
import uxplay_media_engine;

export namespace uxplay
{
    struct usb_mirror_events
    {
        std::function<void()> on_started;
        std::function<void()> on_stopped;
    };

    class usb_mirror
    {
    public:
        static outcome<std::unique_ptr<usb_mirror>> create(media_engine &engine, usb_mirror_events events,
                                                           int host_display_width, int host_display_height);

        // Stops any session gracefully and restores the phone's USB configuration.
        ~usb_mirror();
        usb_mirror(usb_mirror const &) = delete;
        usb_mirror &operator=(usb_mirror const &) = delete;

        // Device arrival/removal hint (WM_DEVICECHANGE): rescan immediately.
        void rescan();

        // Stops the current session; the device is not restarted until it is
        // unplugged and plugged in again.
        void stop_current_session();

        [[nodiscard]] bool active() const;

        // Disabled: the current session stops and no device is started.
        void set_enabled(bool enabled);
        [[nodiscard]] bool enabled() const;

    private:
        struct state;

        explicit usb_mirror(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
