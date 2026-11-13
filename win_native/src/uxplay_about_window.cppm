// SPDX-License-Identifier: MIT
// The "About" window: a window of its own, with a thread of its own, so that
// it neither waits for the video window nor holds it up.
//
// Three pages:
//   About       version and release date, the update check (ask now, the
//               newest release, automatic checks on / off), the score of this
//               PC, links (opened with the default browser or the program
//               Windows has for the file)
//   This PC     what of the system, hardware, drivers and software matters
//               to the program, row by row with a rating, and the scores
//               (uxplay_system_report); "Copy report" puts it on the clipboard
//   Components  what the program is built from, with versions, dates and
//               licences, and when each version of the program was published
//               (uxplay_build_info)
//
// The window draws itself with Direct2D and DirectWrite (software rasterizer:
// it must not compete with the video for the GPU), follows the light or dark
// app colours of Windows and the DPI of its monitor, and scrolls a page that
// is taller than the window.  Keyboard: Tab and Shift+Tab move between its
// links and buttons, Enter or Space uses one, Ctrl+Tab changes the page, the
// arrow keys, Page Up / Down, Home and End scroll, Ctrl+C copies the report,
// Esc closes.
//
// It only shows and asks: what it shows is handed in (set_update, set_facts),
// and what the user wants is reported through about_events.  All public
// members may be called from any thread.
export module uxplay_about_window;

import std;
import uxplay_status;
import uxplay_update_check;
import uxplay_system_report;

export namespace uxplay
{
    enum class about_page : int
    {
        overview,
        this_pc,
        components
    };

    // what the window says about the program itself
    struct about_identity
    {
        std::wstring version;      // "1.11.0"
        std::wstring release_date; // "2026-11-13"
        std::wstring core_version; // the UxPlay release the protocol core derives from
        std::wstring project_url;  // https://...
        // offered as links when they are not empty
        std::filesystem::path log_file;
        std::filesystem::path user_guide; // the guide in the interface language
        std::filesystem::path licences;   // the folder with the licence texts
    };

    // called on the window's thread; must not block
    struct about_events
    {
        std::function<void()> on_check_for_updates;
        std::function<void(bool enabled)> on_automatic_updates;
        // "Check again" on the page "This PC"
        std::function<void()> on_measure_again;
    };

    class about_window
    {
    public:
        [[nodiscard]] static std::unique_ptr<about_window> create(about_identity identity, about_events events);

        // closes the window when it is open
        ~about_window();
        about_window(about_window const &) = delete;
        about_window &operator=(about_window const &) = delete;

        // Opens the window on `page`, or brings the open one to the front and
        // to that page.  topmost: it has to stay above a window that is
        // "always on top".  A failure: the window could not be created.
        [[nodiscard]] status show(about_page page, bool topmost);

        [[nodiscard]] bool is_open() const;

        // Closes the window and returns when it is gone: from then on nothing
        // is reported through about_events (until show() opens it again).
        // The object stays usable; set_update and set_facts are kept for the
        // next window.  Not from a callback of about_events.
        void close();

        void set_update(update_state const &state);
        // the page "This PC" shows "checking" until the first facts arrive
        void set_facts(system_facts facts);

    private:
        struct state;

        explicit about_window(std::unique_ptr<state> created_state);

        std::unique_ptr<state> impl;
    };
}
