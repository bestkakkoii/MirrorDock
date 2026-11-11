// SPDX-License-Identifier: GPL-3.0-or-later
// The bytes of one connection to the phone (a socket from usbmuxd), plain or
// - once start_tls was called - encrypted.  The phone's services start plain
// and switch to TLS when the session is established, with the certificate
// and key of this PC's pairing; the phone's own certificate is not checked
// against anything (the pairing over the cable is what is trusted, as in
// Apple's own tools).
export module uxplay_device_stream;

import std;
import uxplay_status;
import uxplay_byte_stream;
import uxplay_usbmux;

export namespace uxplay
{
    class device_stream final : public byte_stream
    {
    public:
        explicit device_stream(device_socket socket);
        ~device_stream() override;
        device_stream(device_stream const &) = delete;
        device_stream &operator=(device_stream const &) = delete;

        // From here on everything is encrypted.  Certificate and private key
        // are PEM texts (as the pairing record holds them).  Not while another
        // thread reads or writes.
        [[nodiscard]] status start_tls(std::span<std::uint8_t const> certificate_pem,
                                       std::span<std::uint8_t const> private_key_pem);

        // (a read waits without a limit until set_read_patience says otherwise;
        // a write that the phone does not take for twenty seconds fails)
        [[nodiscard]] outcome<std::size_t> read_some(std::span<std::uint8_t> buffer) override;
        [[nodiscard]] status write_all(std::span<std::uint8_t const> data) override;
        void shut_down() override;
        void set_read_patience(std::chrono::milliseconds patience) override;

    private:
        struct state;

        std::unique_ptr<state> impl;
    };
}
