// SPDX-License-Identifier: GPL-3.0-or-later
// lockdown: the phone's service that identifies it, proves that this PC is
// paired with it and starts its other services.  A session is opened with the
// pairing record usbmuxd keeps (the user tapped "Trust" once); from then on
// the conversation is encrypted.
//
// Messages of lockdown and of the classic services it starts are property
// lists behind a 4-byte big-endian length.
export module uxplay_lockdown;

import std;
import uxplay_status;
import uxplay_byte_stream;
import uxplay_property_list;
import uxplay_usbmux;

export namespace uxplay
{
    // one message: 4 bytes of length (big endian), then the property list
    [[nodiscard]] status send_property_list(byte_stream &stream, property_list const &message);
    [[nodiscard]] outcome<property_list> receive_property_list(byte_stream &stream);

    class lockdown_session
    {
    public:
        // Connects to lockdown on the phone and starts an encrypted session
        // with this PC's pairing.  The phone answers within ten seconds or
        // the call fails.
        [[nodiscard]] static outcome<std::unique_ptr<lockdown_session>> open(usbmux_device const &device);

        ~lockdown_session();
        lockdown_session(lockdown_session const &) = delete;
        lockdown_session &operator=(lockdown_session const &) = delete;

        // The reply to GetValue (its item "Value" is what was asked for).
        // An empty domain is the general one.
        [[nodiscard]] outcome<property_list> value(std::string_view domain, std::string_view key);

        // A connection to one of the phone's services, encrypted when the
        // phone says so.  A read of it waits ten seconds for the phone at
        // most, as suits a service that is asked and answers (see
        // byte_stream::set_read_patience for one that is listened to).
        [[nodiscard]] outcome<std::unique_ptr<byte_stream>> start_service(std::string_view name);

        // Sixteen bytes that identify this PC to the phone's developer
        // services, the same at every start: the identifier of its pairing.
        [[nodiscard]] std::array<std::uint8_t, 16> host_identity() const;

    private:
        struct state;

        explicit lockdown_session(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };

    // Of a failure of lockdown_session::open: the phone does not trust this
    // PC (it was never paired, or the pairing was removed on the phone) -
    // as opposed to a phone that could not be reached.
    [[nodiscard]] bool pairing_refused(failure const &cause);
}
