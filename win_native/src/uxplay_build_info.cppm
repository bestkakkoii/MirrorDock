// SPDX-License-Identifier: MIT
// What this program was built from, and when its versions were published:
// the tables the "About" window shows.
//
// Both come from files of the source tree, turned into data when the build
// is configured (win_native/CMakeLists.txt): the inputs from
// build_scripts/build_inputs.tsv (the list fetch_deps.sh fetches from), with
// the versions the build really used where it knows them, and the releases
// from the headings of CHANGELOG.md.
export module uxplay_build_info;

import std;

export namespace uxplay
{
    enum class build_input_kind
    {
        library,  // linked into the program
        tool,     // used to build it
        system,   // parts of Windows it calls
        run_time, // fetched when the program needs it
        credit    // work it is built on without containing it
    };

    // empty text: nothing recorded
    struct build_input
    {
        build_input_kind kind = build_input_kind::library;
        std::string_view name;
        std::string_view version;
        std::string_view revision; // a commit, in full
        std::string_view date;     // ISO 8601
        std::string_view licence;
        std::string_view homepage;
    };

    // in the order of the list
    [[nodiscard]] std::span<build_input const> build_inputs();

    struct release_record
    {
        std::string_view version; // "1.10", "1.9.1"
        std::string_view date;    // ISO 8601
    };

    // every published version of the program, the newest first
    [[nodiscard]] std::span<release_record const> release_history();
}
