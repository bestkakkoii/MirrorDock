// SPDX-License-Identifier: GPL-3.0-or-later
module uxplay_byte_stream;

import std;
import uxplay_status;

namespace uxplay
{
    status byte_stream::read_exact(std::span<std::uint8_t> buffer)
    {
        std::size_t filled = 0;
        while (filled < buffer.size())
        {
            outcome<std::size_t> const got = read_some(buffer.subspan(filled));
            if (!got)
            {
                return std::unexpected(got.error());
            }
            filled += *got;
        }
        return {};
    }
}
