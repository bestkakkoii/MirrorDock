// SPDX-License-Identifier: MIT
// Automatic crop: finds the black bars around the content of a mirrored
// picture (a 16:9 film on a 19.5:9 phone screen, a letterboxed film, a game
// that does not fill the screen) and decides which part to show, so that the
// window hugs the content on all four sides.
//
// The rules are deliberately conservative: the window must not resize because
// of one dark scene.
//   * A bar is a run of rows or columns, from an edge of the picture inwards,
//     that is black over its whole length.
//   * Bars count per axis only as a pair: both sides present and about the same
//     size (content centred, as players and the phone itself place it).
//   * A crop starts only after the same bars were seen for a while and the
//     picture inside them changed meanwhile (moving content, not a still
//     screen); tightening an existing crop takes longer.
//   * Content inside a bar ends the crop.  While the crop is new that
//     happens at once: it was probably wrong, and each such correction delays
//     the next attempt longer.  A crop that has lasted was right for the film
//     that played, and what crosses its bars is most likely the change to the
//     next film: it is kept for a short grace, so that the window does not
//     open up and close in again at every change.  When the content is still
//     there after the grace, the crop ends (or follows the smaller bars of
//     the next film); a stream that went still meanwhile is covered by
//     grace_deadline / expire.  Nothing stays hidden.
//
// Pure logic without any dependency on the GPU or the window; one detector is
// used from one thread.
export module uxplay_crop_detector;

import std;
import uxplay_media_types;

export namespace uxplay
{
    // Black bars around the content of one picture, in pixels from each edge.
    struct content_margins
    {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        // the picture holds no content at all (a fade, a black screen): the
        // margins say nothing
        bool blank = false;

        friend constexpr bool operator==(content_margins const &, content_margins const &) = default;
    };

    // Measures one picture (stateless).
    [[nodiscard]] content_margins measure_black_margins(luma_view const &picture);

    // The bars of `measured` that count as a letterbox/pillarbox (see above);
    // zero margins when there is none.
    [[nodiscard]] content_margins letterbox_margins(content_margins const &measured, int picture_width,
                                                    int picture_height);

    class crop_detector
    {
    public:
        // Examines one picture taken at time_ns (same clock for every call;
        // meant to be called a few times per second).  Returns the region to
        // show from now on when that changes - an empty region is the whole
        // picture - and nothing when what is shown stays.
        [[nodiscard]] std::optional<picture_region> examine(luma_view const &picture, std::int64_t time_ns);

        // Forgets everything: the next pictures are judged afresh, and the
        // caller shows the whole picture again.
        void reset();

        // the region in force (empty: the whole picture)
        [[nodiscard]] picture_region region() const;

        // Set while content crosses a bar of a crop that has lasted (the
        // grace described above).  While it is set every picture should be
        // examined: the first one after the grace settles the matter.  The
        // time is the grace plus the time within which a running stream
        // brings its next picture; if none was examined by then, expire()
        // must be called, because the stream has gone still with content
        // hidden in a bar.
        [[nodiscard]] std::optional<std::int64_t> grace_deadline() const;

        // No picture settled the matter by grace_deadline(): the whole
        // picture is shown.  Returns the region when that changes it (nothing
        // before that time).
        [[nodiscard]] std::optional<picture_region> expire(std::int64_t time_ns);

    private:
        struct candidate
        {
            content_margins margins; // the smallest bars seen while it was watched
            std::int64_t since_ns = 0;
            std::int64_t last_seen_ns = 0;
            // mean brightness of an 8 x 8 grid of cells inside the content at
            // the last examination: tells moving content from a still picture
            std::array<std::uint8_t, 64> cells{};
            int content_changes = 0;
        };

        // content in a bar ended the crop, or loosened it to smaller bars
        [[nodiscard]] picture_region release(content_margins const &measured, std::int64_t time_ns, bool settled);

        int picture_width = 0;
        int picture_height = 0;
        content_margins applied;          // zero: the whole picture is shown
        // content has been crossing a bar of a settled crop since then
        std::optional<std::int64_t> content_in_bar_since_ns;
        std::int64_t applied_since_ns = 0;
        std::optional<candidate> pending;
        std::int64_t hold_off_until_ns = 0;
        std::int64_t penalty_ns = 0;      // 0: not yet initialized
    };
}
