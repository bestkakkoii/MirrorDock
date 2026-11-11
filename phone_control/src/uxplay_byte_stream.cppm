// SPDX-License-Identifier: GPL-3.0-or-later
// A reliable stream of bytes to the phone: a socket through usbmuxd, TLS on
// one, or a connection inside the developer tunnel.  The layers above (plist
// messages, RemoteXPC) are written against this.
//
// One thread may read while another writes.  shut_down() may be called from
// any thread: it ends the stream and makes a waiting read or write return
// with a failure.
export module uxplay_byte_stream;

import std;
import uxplay_status;

export namespace uxplay
{
    // for set_read_patience: a read waits for as long as it takes
    inline constexpr std::chrono::milliseconds no_read_limit = std::chrono::milliseconds::max();

    class byte_stream
    {
    public:
        virtual ~byte_stream() = default;

        // Waits for data and returns how much was put into `buffer` (at least
        // one byte).  A failure when the stream ended or broke, or when the
        // phone sent nothing within the read patience.
        [[nodiscard]] virtual outcome<std::size_t> read_some(std::span<std::uint8_t> buffer) = 0;
        [[nodiscard]] virtual status write_all(std::span<std::uint8_t const> data) = 0;
        virtual void shut_down() = 0;
        // How long one read_some waits for the phone before it fails.  For a
        // stream that asks and gets an answer; one that listens for whatever
        // comes sets no_read_limit.  May be called from any thread; a read
        // that waits already keeps the patience it began with.
        virtual void set_read_patience(std::chrono::milliseconds patience) = 0;

        // exactly buffer.size() bytes
        [[nodiscard]] status read_exact(std::span<std::uint8_t> buffer);
    };
}
