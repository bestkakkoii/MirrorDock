// SPDX-License-Identifier: GPL-3.0-or-later
// RemoteXPC: how the developer services of an iPhone are spoken to inside the
// tunnel.  A connection is HTTP/2 (used only as a frame and flow-control
// layer: two streams, no real headers) carrying Apple's XPC messages - typed
// trees of dictionaries, arrays, texts, numbers and data.
//
// xpc_value is such a tree; remote_xpc_connection sends and receives them.
// A message can announce files (xpc_value::of_file_transfer); the content of
// each then travels on a stream of its own (remote_xpc_connection::send_file).
// All members may be called from any thread.
export module uxplay_remote_xpc;

import std;
import uxplay_status;
import uxplay_byte_stream;

export namespace uxplay
{
    struct xpc_value
    {
        enum class kind
        {
            null,
            flag,
            signed_number,
            unsigned_number,
            real,
            date, // nanoseconds since 1970
            data,
            text,
            uuid,
            array,
            dictionary,
            file_transfer // the announcement of a file whose content follows on its own stream
        };

        kind type = kind::null;
        // flag, signed_number (two's complement), unsigned_number, date; file_transfer: the size in bytes
        std::uint64_t number = 0;
        std::uint64_t transfer = 0; // file_transfer: what ties the announcement to the stream
        double real = 0.0;
        std::string text;
        std::vector<std::uint8_t> bytes; // data; uuid (16 bytes)
        std::vector<xpc_value> items;    // array
        std::vector<std::pair<std::string, xpc_value>> entries; // dictionary, in the order of the wire

        [[nodiscard]] static xpc_value of_flag(bool value);
        [[nodiscard]] static xpc_value of_signed(std::int64_t value);
        [[nodiscard]] static xpc_value of_unsigned(std::uint64_t value);
        [[nodiscard]] static xpc_value of_text(std::string_view value);
        [[nodiscard]] static xpc_value of_data(std::span<std::uint8_t const> value);
        [[nodiscard]] static xpc_value of_uuid(std::span<std::uint8_t const, 16> value);
        [[nodiscard]] static xpc_value of_array(std::vector<xpc_value> values = {});
        [[nodiscard]] static xpc_value of_dictionary();
        // `transfer` is chosen by the sender (1, 2, ... within a message)
        [[nodiscard]] static xpc_value of_file_transfer(std::uint64_t transfer, std::uint64_t size);

        // adds or replaces an entry of a dictionary; returns this value
        xpc_value &set(std::string_view key, xpc_value value);
        // the entry of a dictionary; nullptr when there is none
        [[nodiscard]] xpc_value const *find(std::string_view key) const;

        // the content, when the value is of that kind
        [[nodiscard]] std::optional<std::string_view> as_text() const;
        // unsigned_number, or a signed_number that is not negative
        [[nodiscard]] std::optional<std::uint64_t> as_unsigned() const;
        [[nodiscard]] std::optional<bool> as_flag() const;

        // for the log: the structure and the short values
        [[nodiscard]] std::string describe(std::size_t depth = 0) const;
    };

    class remote_xpc_connection
    {
    public:
        // Takes a TCP connection inside the tunnel and performs the opening exchange.
        [[nodiscard]] static outcome<std::unique_ptr<remote_xpc_connection>> open(std::unique_ptr<byte_stream> stream);

        ~remote_xpc_connection();
        remote_xpc_connection(remote_xpc_connection const &) = delete;
        remote_xpc_connection &operator=(remote_xpc_connection const &) = delete;

        // a message (a dictionary) no answer is awaited for
        [[nodiscard]] status send(xpc_value const &message);
        // a message, and the next message the phone sends
        [[nodiscard]] outcome<xpc_value> request(xpc_value const &message, std::chrono::milliseconds patience);
        // A message that asks for an answer, without waiting for it: for a
        // request whose files go out (send_file) before the phone answers.
        // The answer is then taken with receive().
        [[nodiscard]] status begin_request(xpc_value const &message);
        // The content of a file a message announced with the same `transfer`.
        [[nodiscard]] status send_file(std::uint64_t transfer, std::span<std::uint8_t const> content);
        // the next message of the phone (a dictionary that is not empty)
        [[nodiscard]] outcome<xpc_value> receive(std::chrono::milliseconds patience);
        // false once the connection is over (closed or reset by the phone, the tunnel gone)
        [[nodiscard]] bool alive() const;

    private:
        struct state;

        explicit remote_xpc_connection(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
