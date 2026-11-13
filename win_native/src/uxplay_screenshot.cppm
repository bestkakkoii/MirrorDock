// SPDX-License-Identifier: MIT
// A screenshot of the mirrored picture: a PNG file in the user's Pictures
// folder and the same picture on the clipboard.
//
// The file goes to "<Pictures>\MirrorDock\MirrorDock <date> <time>.png" (the
// folder Windows calls Pictures, wherever the user keeps it); a file is never
// overwritten.  The clipboard gets the picture twice: as a device-independent
// bitmap, which every program takes, and as PNG, which those that know it
// prefer.  Nothing of it leaves the PC.
export module uxplay_screenshot;

import std;
import uxplay_status;

export namespace uxplay
{
    struct saved_screenshot
    {
        std::filesystem::path file;
        // false: the file was written, but the clipboard could not be had
        // (another program kept it open); `clipboard_problem` says why
        bool on_clipboard = false;
        std::string clipboard_problem;
    };

    // pixels: width x height, 4 bytes a pixel (blue, green, red, unused), rows
    // from the top without padding.  clipboard_owner: the HWND the clipboard
    // is opened for (a window of this process).  Blocks while the PNG is made
    // (some tenths of a second for a large picture): not for the UI thread.
    // A failure when the file could not be written; then nothing is put on
    // the clipboard either.
    [[nodiscard]] outcome<saved_screenshot> save_screenshot(int width, int height,
                                                            std::span<std::uint8_t const> pixels,
                                                            void *clipboard_owner);
}
