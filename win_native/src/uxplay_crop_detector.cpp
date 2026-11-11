// SPDX-License-Identifier: MIT
module uxplay_crop_detector;

import std;
import uxplay_media_types;

namespace uxplay
{
    namespace
    {
        constexpr std::int64_t ns_per_ms = 1'000'000;

        // Luma up to this value is black.  Bars are drawn at video black (16
        // in limited range, 0 in full range); the value leaves room for coding
        // noise and is the customary limit of crop detection (24 of 255).
        constexpr int black_limit = 24;
        // a row or column still counts as black with this share of brighter
        // samples (single noisy samples): 1 / 400
        constexpr int noise_divisor = 400;
        // samples examined per row / per column (the whole edge is covered,
        // only the spacing between samples grows with the picture)
        constexpr int samples_per_row = 1024;
        constexpr int samples_per_column = 512;

        // a bar is at least 2 % of the edge it shortens, and at least 8 pixels
        constexpr int minimum_bar_permille = 20;
        constexpr int minimum_bar_pixels = 8;
        // the two bars of a pair differ by at most 2 % of that edge (6 pixels)
        constexpr int symmetry_permille = 20;
        constexpr int symmetry_pixels = 6;
        // what remains is at least a fifth of the picture in each direction
        constexpr int smallest_content_divisor = 5;

        // measuring the same bar twice gives the same edge within this
        constexpr int edge_jitter_pixels = 4;
        // the shown region stays this far inside the measured content edge:
        // the edge itself is blurred by scaling and coding
        constexpr int edge_inset_pixels = 2;

        // how long the same bars must be seen before the crop starts, and
        // before an existing crop is tightened
        constexpr std::int64_t first_crop_stability_ns = 1500 * ns_per_ms;
        constexpr std::int64_t tighter_crop_stability_ns = 8000 * ns_per_ms;
        // ... during which the content inside must have changed this often
        constexpr int required_content_changes = 3;
        // examinations further apart than this do not belong to one observation
        constexpr std::int64_t longest_gap_ns = 1500 * ns_per_ms;
        // A crop that has lasted this long was right for what played.
        constexpr std::int64_t settled_crop_ns = 5000 * ns_per_ms;
        // Content crossing a bar of a settled crop is tolerated this long: the
        // change from one film to the next takes a fraction of a second, while
        // controls or a menu that appear stay for seconds.
        constexpr std::int64_t transition_grace_ns = 1200 * ns_per_ms;
        // A stream that still runs brings its next picture within this time
        // (4 pictures per second and more); one that does not has gone still.
        constexpr std::int64_t still_stream_ns = 250 * ns_per_ms;
        // after content showed up inside a bar of a crop that was not settled
        // (a wrong crop): no new crop for this long, doubling with every such
        // correction, back to the start once a crop has settled
        constexpr std::int64_t first_penalty_ns = 3000 * ns_per_ms;
        constexpr std::int64_t longest_penalty_ns = 30'000 * ns_per_ms;

        // content change is judged on the mean brightness of an 8 x 8 grid of
        // cells: a cell changed when its mean moved by this much, the content
        // changed when this many cells did (coding noise of a still picture
        // stays below both)
        constexpr int signature_cells = 8;
        constexpr int cell_samples = 4;
        constexpr int cell_change = 6;
        constexpr int changed_cells_required = 2;

        [[nodiscard]] std::uint8_t const *row_start(luma_view const &picture, int y)
        {
            return picture.data + static_cast<std::ptrdiff_t>(y) * picture.row_stride;
        }

        [[nodiscard]] int sample(luma_view const &picture, int x, int y)
        {
            return row_start(picture, y)[static_cast<std::ptrdiff_t>(x) * picture.sample_stride];
        }

        [[nodiscard]] constexpr bool has_bars(content_margins const &margins)
        {
            return margins.left > 0 || margins.top > 0 || margins.right > 0 || margins.bottom > 0;
        }

        // every edge of `second` within `tolerance` of the same edge of `first`
        [[nodiscard]] constexpr bool same_edges(content_margins const &first, content_margins const &second,
                                                int tolerance)
        {
            auto const near = [tolerance](int a, int b)
            {
                return (a > b ? a - b : b - a) <= tolerance;
            };
            return near(first.left, second.left) && near(first.top, second.top) && near(first.right, second.right) &&
                   near(first.bottom, second.bottom);
        }

        // Mean brightness of an 8 x 8 grid of cells inside the content, packed
        // one byte per cell.  Comparing two of them tells moving content from
        // a still picture.
        using content_cells = std::array<std::uint8_t, signature_cells * signature_cells>;

        [[nodiscard]] content_cells measure_cells(luma_view const &picture, content_margins const &margins)
        {
            content_cells cells{};
            int const left = margins.left;
            int const top = margins.top;
            int const width = picture.width - margins.left - margins.right;
            int const height = picture.height - margins.top - margins.bottom;
            if (width < signature_cells * cell_samples || height < signature_cells * cell_samples)
            {
                return cells;
            }
            for (int cell_y = 0; cell_y < signature_cells; ++cell_y)
            {
                for (int cell_x = 0; cell_x < signature_cells; ++cell_x)
                {
                    int sum = 0;
                    for (int inner_y = 0; inner_y < cell_samples; ++inner_y)
                    {
                        // sample positions spread evenly over the cell
                        int const y = top + (cell_y * cell_samples + inner_y) * height / (signature_cells * cell_samples);
                        for (int inner_x = 0; inner_x < cell_samples; ++inner_x)
                        {
                            int const x =
                                left + (cell_x * cell_samples + inner_x) * width / (signature_cells * cell_samples);
                            sum += sample(picture, x, y);
                        }
                    }
                    cells[static_cast<std::size_t>(cell_y * signature_cells + cell_x)] =
                        static_cast<std::uint8_t>(sum / (cell_samples * cell_samples));
                }
            }
            return cells;
        }

        [[nodiscard]] bool content_changed(content_cells const &before, content_cells const &now)
        {
            int changed = 0;
            for (std::size_t index = 0; index < before.size(); ++index)
            {
                int const difference = static_cast<int>(before[index]) - static_cast<int>(now[index]);
                if ((difference < 0 ? -difference : difference) >= cell_change && ++changed >= changed_cells_required)
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] constexpr int round_up_to_even(int value)
        {
            return (value + 1) & ~1;
        }

        // the region to show for bars measured as `margins`
        [[nodiscard]] picture_region region_for(content_margins const &margins, int picture_width, int picture_height)
        {
            if (!has_bars(margins))
            {
                return {};
            }
            auto const inset = [](int bar)
            {
                return bar > 0 ? bar + edge_inset_pixels : 0;
            };
            // 4:2:0 chroma: even coordinates and sizes
            int const left = round_up_to_even(inset(margins.left));
            int const top = round_up_to_even(inset(margins.top));
            int const right = round_up_to_even(inset(margins.right));
            int const bottom = round_up_to_even(inset(margins.bottom));
            picture_region region;
            region.left = left;
            region.top = top;
            region.width = (picture_width - left - right) & ~1;
            region.height = (picture_height - top - bottom) & ~1;
            return region.empty() ? picture_region{} : region;
        }
    }

    content_margins measure_black_margins(luma_view const &picture)
    {
        content_margins margins;
        if (picture.data == nullptr || picture.width < 2 * signature_cells * cell_samples ||
            picture.height < 2 * signature_cells * cell_samples || picture.sample_stride <= 0)
        {
            margins.blank = true; // nothing that could be judged
            return margins;
        }

        int const step_x = std::max(1, picture.width / samples_per_row);
        int const row_noise = ((picture.width + step_x - 1) / step_x) / noise_divisor;
        auto const row_is_black = [&picture, step_x, row_noise](int y)
        {
            std::uint8_t const *const row = row_start(picture, y);
            int bright = 0;
            for (int x = 0; x < picture.width; x += step_x)
            {
                if (row[static_cast<std::ptrdiff_t>(x) * picture.sample_stride] > black_limit && ++bright > row_noise)
                {
                    return false;
                }
            }
            return true;
        };
        int top = 0;
        while (top < picture.height && row_is_black(top))
        {
            ++top;
        }
        if (top == picture.height)
        {
            margins.blank = true;
            return margins;
        }
        int bottom = 0;
        while (bottom < picture.height - top - 1 && row_is_black(picture.height - 1 - bottom))
        {
            ++bottom;
        }

        // columns are judged over the rows that hold content
        int const first_row = top;
        int const end_row = picture.height - bottom;
        int const step_y = std::max(1, (end_row - first_row) / samples_per_column);
        int const column_noise = ((end_row - first_row + step_y - 1) / step_y) / noise_divisor;
        auto const column_is_black = [&picture, first_row, end_row, step_y, column_noise](int x)
        {
            int bright = 0;
            for (int y = first_row; y < end_row; y += step_y)
            {
                if (sample(picture, x, y) > black_limit && ++bright > column_noise)
                {
                    return false;
                }
            }
            return true;
        };
        int left = 0;
        while (left < picture.width && column_is_black(left))
        {
            ++left;
        }
        if (left == picture.width)
        {
            // rows and columns are sampled on different grids: a few bright
            // samples seen by one and not by the other are not content
            margins.blank = true;
            return margins;
        }
        int right = 0;
        while (right < picture.width - left - 1 && column_is_black(picture.width - 1 - right))
        {
            ++right;
        }
        margins.left = left;
        margins.top = top;
        margins.right = right;
        margins.bottom = bottom;
        return margins;
    }

    content_margins letterbox_margins(content_margins const &measured, int picture_width, int picture_height)
    {
        content_margins result;
        if (measured.blank || picture_width <= 0 || picture_height <= 0)
        {
            return result;
        }
        auto const is_pair = [](int first, int second, int extent)
        {
            int const minimum = std::max(minimum_bar_pixels, extent * minimum_bar_permille / 1000);
            int const tolerance = std::max(symmetry_pixels, extent * symmetry_permille / 1000);
            int const difference = first > second ? first - second : second - first;
            return first >= minimum && second >= minimum && difference <= tolerance;
        };
        if (is_pair(measured.left, measured.right, picture_width))
        {
            result.left = measured.left;
            result.right = measured.right;
        }
        if (is_pair(measured.top, measured.bottom, picture_height))
        {
            result.top = measured.top;
            result.bottom = measured.bottom;
        }
        // a small bright spot in a dark scene is not a picture with bars
        if ((picture_width - result.left - result.right) * smallest_content_divisor < picture_width ||
            (picture_height - result.top - result.bottom) * smallest_content_divisor < picture_height)
        {
            return {};
        }
        return result;
    }

    void crop_detector::reset()
    {
        picture_width = 0;
        picture_height = 0;
        applied = {};
        applied_since_ns = 0;
        content_in_bar_since_ns.reset();
        pending.reset();
        hold_off_until_ns = 0;
        penalty_ns = 0;
    }

    std::optional<std::int64_t> crop_detector::grace_deadline() const
    {
        if (!content_in_bar_since_ns)
        {
            return std::nullopt;
        }
        return *content_in_bar_since_ns + transition_grace_ns + still_stream_ns;
    }

    std::optional<picture_region> crop_detector::expire(std::int64_t time_ns)
    {
        if (!content_in_bar_since_ns || time_ns - *content_in_bar_since_ns < transition_grace_ns + still_stream_ns)
        {
            return std::nullopt;
        }
        // The last pictures had content in a bar and no later one cleared it:
        // the stream went still that way.  Everything is shown; the crop is
        // judged afresh when pictures come again.
        content_in_bar_since_ns.reset();
        pending.reset();
        applied = {};
        hold_off_until_ns = time_ns;
        penalty_ns = first_penalty_ns;
        return picture_region{};
    }

    picture_region crop_detector::release(content_margins const &measured, std::int64_t time_ns, bool settled)
    {
        pending.reset();
        // Still a letterbox, only with smaller bars (another film, or a crop
        // that had been tightened during a dark shot): show up to the bars of
        // this picture.  Nothing stays hidden - they are black in it - and the
        // window changes once instead of opening up and closing in again.
        content_margins const smaller = letterbox_margins(measured, picture_width, picture_height);
        bool const loosened = has_bars(smaller) && smaller.left <= applied.left + edge_jitter_pixels &&
                              smaller.top <= applied.top + edge_jitter_pixels &&
                              smaller.right <= applied.right + edge_jitter_pixels &&
                              smaller.bottom <= applied.bottom + edge_jitter_pixels;
        if (loosened)
        {
            applied.left = std::min(applied.left, smaller.left);
            applied.top = std::min(applied.top, smaller.top);
            applied.right = std::min(applied.right, smaller.right);
            applied.bottom = std::min(applied.bottom, smaller.bottom);
            applied_since_ns = time_ns;
        }
        else
        {
            applied = {};
        }
        if (settled)
        {
            // the crop was right while it lasted: what follows is judged
            // afresh, without delay
            hold_off_until_ns = time_ns;
            penalty_ns = first_penalty_ns;
        }
        else
        {
            // the crop (or its tightening) was wrong: the next attempt waits,
            // longer each time
            hold_off_until_ns = time_ns + penalty_ns;
            penalty_ns = std::min(penalty_ns * 2, longest_penalty_ns);
        }
        return region_for(applied, picture_width, picture_height);
    }

    picture_region crop_detector::region() const
    {
        return region_for(applied, picture_width, picture_height);
    }

    std::optional<picture_region> crop_detector::examine(luma_view const &picture, std::int64_t time_ns)
    {
        if (picture.data == nullptr || picture.width <= 0 || picture.height <= 0)
        {
            return std::nullopt;
        }
        if (picture.width != picture_width || picture.height != picture_height)
        {
            // another stream (rotation, new connection): what was learned
            // belongs to the old picture
            bool const was_cropped = has_bars(applied);
            reset();
            picture_width = picture.width;
            picture_height = picture.height;
            if (was_cropped)
            {
                return picture_region{};
            }
        }
        if (penalty_ns == 0)
        {
            penalty_ns = first_penalty_ns;
        }

        content_margins const measured = measure_black_margins(picture);
        if (measured.blank)
        {
            // a fade or a black screen says nothing about the bars, and
            // nothing is in them either
            content_in_bar_since_ns.reset();
            return std::nullopt;
        }

        if (has_bars(applied))
        {
            // content inside a bar that is cut off
            bool const content_in_bar = measured.left < applied.left - edge_jitter_pixels ||
                                        measured.top < applied.top - edge_jitter_pixels ||
                                        measured.right < applied.right - edge_jitter_pixels ||
                                        measured.bottom < applied.bottom - edge_jitter_pixels;
            bool const settled = time_ns - applied_since_ns >= settled_crop_ns;
            if (content_in_bar)
            {
                pending.reset();
                if (settled)
                {
                    // most likely a passing transition: the crop stays for a
                    // moment (see transition_grace_ns)
                    if (!content_in_bar_since_ns)
                    {
                        content_in_bar_since_ns = time_ns;
                    }
                    if (time_ns - *content_in_bar_since_ns < transition_grace_ns)
                    {
                        return std::nullopt;
                    }
                }
                content_in_bar_since_ns.reset();
                return release(measured, time_ns, settled);
            }
            // the bars are black (again)
            content_in_bar_since_ns.reset();
            if (settled)
            {
                penalty_ns = first_penalty_ns;
            }
        }

        content_margins const bars = letterbox_margins(measured, picture_width, picture_height);
        if (!has_bars(bars) || same_edges(bars, applied, edge_jitter_pixels))
        {
            // nothing to cut, or exactly what is cut already
            pending.reset();
            return std::nullopt;
        }

        // other bars than the ones in force: watch them
        content_cells const cells = measure_cells(picture, bars);
        if (pending && time_ns - pending->last_seen_ns <= longest_gap_ns &&
            same_edges(pending->margins, bars, edge_jitter_pixels))
        {
            // the same bars again: keep the smallest of each (the most
            // content), and note whether the content inside moved
            pending->margins.left = std::min(pending->margins.left, bars.left);
            pending->margins.top = std::min(pending->margins.top, bars.top);
            pending->margins.right = std::min(pending->margins.right, bars.right);
            pending->margins.bottom = std::min(pending->margins.bottom, bars.bottom);
            if (content_changed(pending->cells, cells))
            {
                ++pending->content_changes;
            }
            pending->cells = cells;
            pending->last_seen_ns = time_ns;
        }
        else
        {
            pending = candidate{bars, time_ns, time_ns, cells, 0};
        }

        std::int64_t const stability = has_bars(applied) ? tighter_crop_stability_ns : first_crop_stability_ns;
        if (time_ns < hold_off_until_ns || time_ns - pending->since_ns < stability ||
            pending->content_changes < required_content_changes)
        {
            return std::nullopt;
        }
        applied = pending->margins;
        applied_since_ns = time_ns;
        pending.reset();
        return region_for(applied, picture_width, picture_height);
    }
}
