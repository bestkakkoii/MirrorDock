// SPDX-License-Identifier: GPL-3.0-or-later
// Remote control of the phone: touch, keyboard and hardware buttons.
//
// AirPlay mirroring is one-way: it has no channel that carries input back to
// the phone.  Input goes another way, through the developer services of iOS
// (the ones Xcode's Device Hub uses to operate a real device): over the USB
// cable this class opens the phone's developer tunnel, starts a stream of the
// screen - iOS lets input through only while one runs; its packets are
// dropped, the picture still comes by AirPlay - and then delivers HID reports
// for the touch screen and for a keyboard it registers.
//
// What the phone must have: iOS 27, Developer Mode switched on and this PC
// trusted.  The developer services come with Apple's Developer Disk Image;
// a phone that lacks it gets it installed here (uxplay_developer_image: the
// first use, and the first after an update of iOS, fetches the image and
// asks Apple's signing service to sign it for this phone - both need the
// internet; see README_AI.md).
//
// Coordinates are 0..65535 across the picture as the phone shows it (the way
// AirPlay mirrors it); this class turns them to the phone's own panel, because
// it asks the phone which way it is held.
//
// The program must have started Windows Sockets (WSAStartup) before.
//
// All members may be called from any thread.  The status callback is called
// from a thread of this object and must not block or destroy the object.
export module uxplay_remote_control;

import std;
import uxplay_status;

export namespace uxplay
{
    enum class remote_control_state
    {
        starting,  // connecting to the phone
        preparing, // installing the Developer Disk Image on the phone: can take a minute
        ready,     // input reaches the phone
        failed     // over: see remote_control_status::problem
    };

    // why the phone cannot be controlled
    enum class remote_control_problem
    {
        none,
        no_device,          // no iPhone on a cable
        not_trusted,        // the iPhone does not trust this PC
        developer_mode_off, // Developer Mode is off on the iPhone
        image_mount_failed, // the Developer Disk Image could not be installed on the iPhone
        tunnel_failed,      // no connection to the developer services
        unsupported_ios,    // the iOS version is too old for this way
        media_in_use,       // the camera or the microphone is in use on the iPhone
        stream_failed,      // the session that lets input through did not start
        connection_lost,    // the phone went away while it was controlled (cable pulled)
        internal            // anything else (detail says what is known)
    };

    struct remote_control_status
    {
        remote_control_state state = remote_control_state::starting;
        remote_control_problem problem = remote_control_problem::none;
        // the stage that failed with the system's or the phone's words: for the log
        std::string detail;
    };

    [[nodiscard]] std::string_view remote_control_problem_name(remote_control_problem problem);

    // One finger: 0..65535 across the picture as the phone shows it.
    struct touch_point
    {
        int identity = 0; // 0..3, the same for as long as the finger is down
        std::uint16_t x = 0;
        std::uint16_t y = 0;
        bool touching = false; // false: the finger lifts at this position
    };

    enum class phone_button
    {
        home,
        lock,
        volume_up,
        volume_down,
        mute
    };

    class remote_control
    {
    public:
        using status_callback = std::function<void(remote_control_status const &)>;

        // Starts connecting to the phone on the cable.  How that goes is
        // reported through on_status: "starting" (more than once, as the
        // stages pass), "preparing" when the phone needs the Developer Disk
        // Image, then "ready" or "failed".  After "ready" a "failed" follows
        // when the phone goes away.  A failed object does nothing any more
        // and is to be destroyed.
        // image_cache: the directory the Developer Disk Image is kept in once
        // it was fetched (empty: it is fetched whenever a phone needs it).
        [[nodiscard]] static outcome<std::unique_ptr<remote_control>> start(status_callback on_status,
                                                                            std::filesystem::path image_cache);

        // Ends the control session: nothing stays pressed on the phone, its
        // screen stream is stopped.  Nothing is reported from here on.  Waits
        // until that is done (a moment; while the phone does not answer, for
        // as long as the stage it hangs in is given: twenty seconds at most).
        ~remote_control();
        remote_control(remote_control const &) = delete;
        remote_control &operator=(remote_control const &) = delete;

        // Every finger that is down or lifts now.  Of a run of moves of the
        // same fingers only the newest is delivered when the phone falls behind.
        void touch(std::span<touch_point const> points);
        // Scrolls what is under (x, y) by (delta_x, delta_y), all in the units
        // of touch_point: a finger of its own drags that far and rests before
        // it lifts (the content follows it and stops there).
        void scroll(std::uint16_t x, std::uint16_t y, int delta_x, int delta_y);
        // A quick flick from (x, y) by (delta_x, delta_y): turns a page, shows
        // the next video, goes back.
        void swipe(std::uint16_t x, std::uint16_t y, int delta_x, int delta_y);
        // the HID keyboard usages (page 7) held down now; empty: none
        void keys(std::span<std::uint8_t const> usages);
        void press(phone_button button);
        // the mirrored picture changed shape: the phone may have been turned
        void picture_changed();

    private:
        struct state;

        explicit remote_control(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };

    // The HID keyboard usage (page 7) of a key as Windows reports it in
    // WM_KEYDOWN / WM_KEYUP: its scan code and the "extended key" flag.
    // Nothing for keys a phone has no use for.
    [[nodiscard]] std::optional<std::uint8_t> keyboard_usage(unsigned scan_code, bool extended);
}
