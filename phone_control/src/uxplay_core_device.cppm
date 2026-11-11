// SPDX-License-Identifier: GPL-3.0-or-later
// The developer services of an iPhone behind its tunnel ("CoreDevice", what
// Xcode's Device Hub talks to): the directory that names them and their
// ports, the envelope of a call, and the offer that starts a stream of the
// screen.
//
// The wire formats here follow what the pymobiledevice3 project found out
// about them (https://github.com/doronz88/pymobiledevice3, GPL-3.0).
export module uxplay_core_device;

import std;
import uxplay_status;
import uxplay_remote_xpc;
import uxplay_device_tunnel;

export namespace uxplay
{
    // a random identifier (version 4)
    [[nodiscard]] std::array<std::uint8_t, 16> new_uuid();
    // 8-4-4-4-12, upper case
    [[nodiscard]] std::string uuid_text(std::span<std::uint8_t const, 16> uuid);

    class service_directory
    {
    public:
        // Connects to the phone's directory inside the tunnel and reads it.
        // `identity` is what this PC presents itself with: the phone expects
        // every connection to a tunnel to carry the same one.
        [[nodiscard]] static outcome<std::unique_ptr<service_directory>> open(
            device_tunnel &tunnel, std::span<std::uint8_t const, 16> identity);

        ~service_directory();
        service_directory(service_directory const &) = delete;
        service_directory &operator=(service_directory const &) = delete;

        [[nodiscard]] std::size_t size() const;
        [[nodiscard]] bool offers(std::string_view service) const;
        // a RemoteXPC connection to one of the services
        [[nodiscard]] outcome<std::unique_ptr<remote_xpc_connection>> connect(std::string_view service);

    private:
        struct state;

        explicit service_directory(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };

    // One call of a CoreDevice service: `input` in its envelope, and the
    // output.  When the phone answers with an error, the failure carries its
    // code (failure::code) and its words.
    [[nodiscard]] outcome<xpc_value> core_device_invoke(remote_xpc_connection &connection, std::string_view feature,
                                                        std::string_view action, xpc_value input,
                                                        std::chrono::milliseconds patience);

    // The input of "start a video stream of the screen": the phone sends
    // the stream (RTP) from `phone_address` to `receiver_port` at
    // `receiver_address`, both ends of the tunnel.
    [[nodiscard]] outcome<xpc_value> video_stream_request(std::string_view receiver_address,
                                                          std::uint16_t receiver_port, std::string_view phone_address);

    inline constexpr std::string_view display_service_name = "com.apple.coredevice.displayservice";
    inline constexpr std::string_view start_stream_feature = "com.apple.coredevice.feature.startmediastream";
    inline constexpr std::string_view start_stream_action = "com.apple.coredevice.action.mediastreamstart";
    inline constexpr std::string_view stop_stream_feature = "com.apple.coredevice.feature.stopmediastream";
    inline constexpr std::string_view stop_stream_action = "com.apple.coredevice.action.mediastreamstop";
    // the phone refuses a stream of the screen while an app uses the camera or the microphone
    inline constexpr std::int64_t media_in_use_code = 9022;
}
