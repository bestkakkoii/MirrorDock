// SPDX-License-Identifier: GPL-3.0-or-later
// One HTTP request and its answer, through the system's own HTTP stack
// (WinHTTP: the proxy and the certificates Windows is set up with).  For the
// things the program fetches from the internet: the Developer Disk Image and
// Apple's signature for it (remote control), and the newest release it asks
// GitHub about (the update check of the media engine).
export module uxplay_http_client;

import std;
import uxplay_status;

export namespace uxplay
{
    struct http_request
    {
        std::string url; // http:// or https://
        // sent as it is; the default names neither this program nor its user
        std::string user_agent = "http-client";
        // with a body the request is a POST of this type
        std::string content_type;
        std::span<std::uint8_t const> body;
        // further header lines, each "Name: value" without a line end
        std::vector<std::string> headers;
        // an answer with a longer body is refused
        std::size_t longest_answer = 64 * 1024 * 1024;
        // for each step: resolving the name, connecting, sending, every piece that is received
        std::chrono::milliseconds patience = std::chrono::seconds{30};
        // asked between the pieces of the answer; true ends the request with a failure
        std::function<bool()> cancelled;
        // told after every piece of the answer how many bytes of its body have arrived
        std::function<void(std::size_t received)> received;
        // When not empty (https only): the one root certificate (DER) the
        // server's certificate must lead to, in place of the roots Windows
        // trusts - for a server whose certificate comes from its owner's own
        // authority.  Nothing is sent to a server that does not pass, and a
        // redirection is not followed.
        std::span<std::uint8_t const> only_trusted_root;
    };

    struct http_answer
    {
        int status = 0; // 200, 404, ...
        std::vector<std::uint8_t> body;
    };

    // A value for every answer of the server, whatever its status; a failure
    // when there was none (no network, a certificate that is not trusted, ...).
    [[nodiscard]] outcome<http_answer> http_exchange(http_request const &request);
}
