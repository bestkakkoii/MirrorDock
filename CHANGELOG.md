# Changelog

All releases of MirrorDock. Up to 1.6 the program was published as *UxPlay for Windows (native)*; the tags keep the `win-native-` prefix.

Mirroring over Wi-Fi and over USB was verified with an iPhone 16 Pro Max (iOS 26) up to version 1.5. From 1.6 on, every entry ends with how far that release was tested.

## 1.8 — 2026-11-11

- 🌐 **Ten interface languages**, chosen from the Windows display language: English, Traditional Chinese, Simplified Chinese, Japanese, Korean, Spanish, German, French, Vietnamese and Thai (`-ui` forces one).
- 📖 The README and the user guide in the same ten languages; the zip carries all guides in the folder `user_guide`.
- 🎨 New README with logo, feature overview and diagrams; this changelog.
- 🖱️ **Right-click menu**: a right click anywhere on the picture opens the program's menu.
- ✂️ **Fixed: the automatic crop did not come back after a change of film.** Every change counted as a wrong crop and delayed the next attempt, up to 30 seconds. A crop that has lasted now stays through the change; when something remains in a bar, the whole picture is back within about a second, and the crop returns as soon as the bars do.
- 🔧 **Fixed: version 1.7 could fail to take over the settings of the old name** ("access denied" on some PCs). 1.8 takes over every setting that has not been set again since.
- ⏱️ **Fixed: *Performance* mode could overflow its picture queue with an iPhone**, whose timestamps run about 85 ms ahead of its pictures, and then showed pictures by arrival instead of by timestamp. Its queue is now as deep as in *Balanced*; a new stream no longer starts with extra delay.
- 📝 **Correction:** 1.6 announced lower memory while mirroring. That holds for streams that keep few pictures, such as the test stream; an iPhone 16 Pro Max declares more, and its decoder surface pool is as large as before. The comparison has been removed from the README.
- 🔍 Log: the 10-second lines show how early pictures and sound arrive relative to their timestamps; one line per session tells whether the phone offers media remote control.
- *Tested:* compiled with warnings as errors; not yet run with a phone.

## 1.7 — 2026-11-11

- 🏷️ **The product is now called MirrorDock**: executable `mirror_dock.exe`, default receiver name `MirrorDock@your-PC`, repository `bestkakkoii/MirrorDock`.
- Settings, the "Start with Windows" entry and the log folder of the old name are taken over at the first start. `.uxplayrc` and the pairing key files keep their names.
- *Tested:* released after compiling only. The owner then used it with an iPhone 16 Pro Max over Wi-Fi (picture, sound, automatic crop, modes), which turned up the defects fixed in 1.8.

## 1.6 — 2026-11-11

- ✂️ **Crop black bars**: automatically (key C, `-crop auto`) or framed by hand (key X).
- 🎛️ **Quality / Balanced / Performance** modes (`-mode`, menu).
- 🪟 Landscape streams open at half the screen width; aspect-locked resizing; size and position remembered per orientation.
- 🔒 **"Wi-Fi only" is enforced**: own mDNS host name, queries answered only from the advertised link, connections on the USB link refused.
- 🧠 Decoder surface pool sized from the stream and everything freed at the end of a session (lower memory for streams that keep few pictures; see the correction in 1.8), audio stream stopped when idle, high-resolution frame pacing.
- 🛡️ Single instance per receiver name, start with Windows, always on top, start-failure dialog, timestamped log, crash record; the RTSP server survives socket errors; stream and metadata lengths validated; the mDNS goodbye is really sent at exit.
- Source files are UTF-8 with BOM and CRLF.
- *Tested:* build, an engine test with a recorded stream and independent code review; not run with a phone before release.

## 1.5 — 2026-11-03

- 🌐 The interface follows the Windows display language: Traditional Chinese, Simplified Chinese or English (`-ui`).

## 1.4 — 2026-11-03

- 🧹 Clean Windows-only tree: no GLib (own main event loop), no GStreamer-era renderer layer, HLS / QuickTime-USB / BLE and non-Windows code removed.
- Options of other UxPlay builds are accepted and ignored with a warning.
- Warnings are errors on all owned code; the media engine in `win_native/` is MIT.

## 1.3.1 — 2026-11-03

- 📦 Single statically linked executable: no bundled DLLs.

## 1.3 — 2026-11-03

- 🎚️ Player-style control bar over the video (link switch, mute, menu, full screen), fading with mouse movement.
- Soft limiter for decoded audio peaks; audio statistics in the log.

## 1.2 — 2026-11-02

- 🔁 **Prefer USB / Wi-Fi only** switch that never touches the USB device, so the phone keeps charging.
- One-click recovery when the iPhone's network adapter was disabled.

## 1.1 — 2026-11-02

- 🔌 Plugging and unplugging the cable switches between USB and Wi-Fi while the program runs.
- 📵 Mobile data protection: detects when the PC's internet traffic leaves through the phone and offers a one-click fix.
- The window shows the link that is really in use.

## 1.0 — 2026-11-01

- 🎉 First Windows-native release: AirPlay mirroring and audio over Wi-Fi and over the iPhone's USB network link, Direct3D 11 + FFmpeg D3D11VA video, WASAPI audio, borderless full screen, C++26 modules.
