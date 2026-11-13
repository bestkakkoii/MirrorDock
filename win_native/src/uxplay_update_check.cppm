// SPDX-License-Identifier: MIT
// Asks GitHub whether a newer release of the program exists.
//
// One request per check: the newest published release of the project
// (uxplay_version.h names the address).  What goes out is the request itself
// and the name and version of this program as the "User-Agent" GitHub asks
// every client for; nothing about the user or this PC.  The answer is only
// looked at: nothing is downloaded or installed here.  The user opens the
// release's page or its package in the browser.
//
// update_checker runs the checks on a thread of its own: at once when asked
// (check_now), and by itself once a day while "automatic" is on.  The first
// automatic check waits half a minute after the start, a later one a day
// after the last check that got an answer (across runs: the time is kept in
// the settings).  An automatic check is skipped, and tried again an hour
// later, while Windows says the connection is metered or while the owner says
// no (the PC's internet goes through the iPhone's mobile data); a check the
// user asks for is always made.  A failed automatic check changes nothing the
// user sees and is tried again an hour later.
export module uxplay_update_check;

import std;
import uxplay_status;

export namespace uxplay
{
    struct release_version
    {
        unsigned int major = 0;
        unsigned int minor = 0;
        unsigned int patch = 0;

        auto operator<=>(release_version const &) const = default;

        // "1.10.0"
        [[nodiscard]] std::string text() const;
    };

    // the version of this program (uxplay_version.h)
    [[nodiscard]] release_version running_version();

    // "win-native-1.10" is 1.10.0, "win-native-1.9.1" is 1.9.1; nothing for a
    // tag of another form
    [[nodiscard]] std::optional<release_version> release_version_from_tag(std::string_view tag);

    struct published_release
    {
        release_version version;
        std::string date;        // the day it was published, as it was at this PC: "2026-11-12"; empty when not known
        std::string page_url;    // its page on GitHub
        std::string package_url; // the zip to download
        std::uint64_t package_bytes = 0; // 0 when not known

        bool operator==(published_release const &) const = default;
    };

    // The release in the JSON document GitHub answers with.  Addresses that do
    // not lead to the project's own pages are replaced by the fixed addresses
    // of its newest release: nothing from the answer is opened unseen.
    [[nodiscard]] outcome<published_release> parse_release(std::string_view json);

    // Asks GitHub now.  Blocks for as long as the request takes (bounded).
    [[nodiscard]] outcome<published_release> newest_release(std::function<bool()> cancelled);

    // Whether Windows counts the current connection as one that is paid by
    // volume; nothing when it cannot tell.  COM must be initialized on the
    // calling thread.
    [[nodiscard]] std::optional<bool> metered_connection();

    enum class update_phase
    {
        unknown,         // never checked
        checking,
        up_to_date,
        newer_available,
        failed           // the check the user asked for got no answer
    };

    struct update_state
    {
        update_phase phase = update_phase::unknown;
        // of the last check that got an answer (after a restart: its version
        // only, with the fixed addresses of the newest release)
        std::optional<published_release> newest;
        std::optional<std::chrono::system_clock::time_point> checked; // when that was
        std::string problem; // why a check failed (English, as logged)
        bool automatic = true;

        bool operator==(update_state const &) const = default;
    };

    struct update_events
    {
        // The state changed.  by_request: through something the user did
        // (check_now, set_automatic).  Called on the checker's thread or on
        // the caller's; must not block.
        std::function<void(update_state const &state, bool by_request)> on_change;
        // asked before an automatic check; false skips it this time
        std::function<bool()> automatic_allowed;
    };

    class update_checker
    {
    public:
        // Starts the checker with what earlier runs stored.  Never fails: a
        // checker whose settings cannot be read starts from the defaults.
        [[nodiscard]] static std::unique_ptr<update_checker> create(update_events events);

        // waits for a check in progress (bounded by the patience of its request)
        ~update_checker();
        update_checker(update_checker const &) = delete;
        update_checker &operator=(update_checker const &) = delete;

        [[nodiscard]] update_state current() const;

        // any thread; a check already under way is not repeated
        void check_now();

        // stored for later runs
        void set_automatic(bool enabled);

    private:
        struct state;

        explicit update_checker(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
