// SPDX-License-Identifier: MIT
module uxplay_build_info;

import std;

namespace uxplay
{
    namespace
    {
        // rows written by win_native/CMakeLists.txt (uxplay_generate_build_info)
        constexpr build_input inputs[] = {
#include "uxplay_build_inputs.inc"
        };

        constexpr release_record releases[] = {
#include "uxplay_release_history.inc"
        };
    }

    std::span<build_input const> build_inputs()
    {
        return inputs;
    }

    std::span<release_record const> release_history()
    {
        return releases;
    }
}
