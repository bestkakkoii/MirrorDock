// SPDX-License-Identifier: MIT
// Installs a newer release of the program in place of the running one, when
// the user asks for it.
//
// prepare()  fetches the release's package and its signature from the
//            project's own pages on GitHub, checks the signature, and unpacks
//            the package beside the program (".update" in its folder).
// apply()    moves the running program's files aside (Windows lets a running
//            executable be renamed) and the new ones into their place.  The
//            host then ends the program and starts the new one.
//            It leaves a note in ".update" that names the version.
// At the next start the installer finds the note, says "updated" once, and
// deletes what was moved aside.
//
// Only what the project's owner published is ever installed: every package is
// signed with a key that is kept off the internet, and the program carries
// the public half (uxplay_release_signature).  A release without a signature,
// or with one that does not fit, is not installed - whoever got hold of the
// download server or of the project's account cannot make the program run
// their code.  The version of
// the executable in the package must be the version the release claims, so
// that an old, signed package cannot be passed off as a new one.  Nothing is
// installed without the user's click, and a failure at any step leaves the
// running program as it was.
export module uxplay_update_install;

import std;
import uxplay_status;
import uxplay_update_check;

export namespace uxplay
{
    enum class install_phase
    {
        idle,
        downloading, // the package comes in (received of expected bytes)
        checking,    // its signature and its contents are checked, the files are unpacked
        ready,       // everything stands beside the program: apply() puts it in place
        installed,   // it is in place: the program has to end and start again
        failed
    };

    // why it failed, as far as the user can do something about it
    enum class install_problem
    {
        none,
        no_signature, // the release carries none: it cannot be checked
        download,     // the package or its signature did not arrive
        not_genuine,  // the signature does not fit the package
        package,      // the package is damaged, or not what a release contains
        folder,       // the program's folder cannot be written to
        other
    };

    struct install_state
    {
        install_phase phase = install_phase::idle;
        release_version version;    // the release in question
        std::uint64_t received = 0; // bytes of the package so far
        std::uint64_t expected = 0; // 0: not known
        install_problem problem = install_problem::none;
        std::string detail; // why it failed: English, as logged

        bool operator==(install_state const &) const = default;
    };

    struct install_events
    {
        // The state changed.  Called on the installer's thread (for apply():
        // on the caller's); must not block.  It may call apply() and
        // current(), not prepare().
        std::function<void(install_state const &state)> on_change;
    };

    class update_installer
    {
    public:
        // Also clears away, on a thread of its own, what an update left in
        // the program's folder: the files it replaced (the program that was
        // replaced may take a moment to let go of its own), or an update that
        // was unpacked and never put in place.  Files of the program that an
        // update moved aside and could not put back are kept.
        [[nodiscard]] static std::unique_ptr<update_installer> create(install_events events);

        // A preparation in progress is given up (a download does not hold
        // the end of the program up).  What was unpacked stays for the next
        // start to clear away.
        ~update_installer();
        update_installer(update_installer const &) = delete;
        update_installer &operator=(update_installer const &) = delete;

        [[nodiscard]] install_state current() const;

        // this run of the program is the first after an update that was put in place
        [[nodiscard]] bool updated_at_start() const;

        // Fetches, checks and unpacks `release`.  Returns at once; the state
        // says how it goes.  Does nothing while a preparation runs, and
        // nothing when that release is ready already.
        void prepare(published_release const &release);

        // Puts the prepared files in place of the running ones (phase ready
        // only).  After a success the program must end and start again: the
        // executable to start is restart_target().  A failure leaves every
        // file where it was.  One caller puts it in place: another that asks
        // at the same moment gets a failure and changes nothing.
        [[nodiscard]] status apply();

        // the program's executable in its folder, as a release names it
        [[nodiscard]] static std::filesystem::path restart_target();

    private:
        struct state;

        explicit update_installer(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
