# Changelog

All releases of MirrorDock. Up to 1.6 the program was published as *UxPlay for Windows (native)*; the tags keep the `win-native-` prefix.

Mirroring over Wi-Fi and over USB was verified with an iPhone 16 Pro Max (iOS 26) up to version 1.5. From 1.6 on, every entry ends with how far that release was tested.

## 1.11 — 2026-11-13

- 🪟 **A new About window**, a window of its own with three pages. *About*: version, release date, the update check and links that open in the default browser. *This PC*: what of the system, hardware, drivers and software matters to MirrorDock, row by row with a rating and, where something is not in order, what to do about it; a score per group and overall; **Copy report** for bug reports. *Components*: every library and tool the program is built from, with version, date and licence, and when each version of MirrorDock was published. It follows the light or dark colours of Windows.
- 🔄 **Update check.** **Check for updates…** in the menu asks GitHub for the newest release; **Download** opens its zip in the browser. Once a day it checks by itself and says when a newer release is out; one switch turns that off. One request to `api.github.com` that carries nothing but the program's name and version; no automatic check over a metered connection or through the iPhone's mobile data. Nothing is installed by the program.
- 🖱️ **The window's own commands come first while the iPhone is controlled.** A double-click switches full screen on and off also in control mode (with **Alt** held it is a double tap on the phone), and **Esc** leaves full screen whatever the phone is doing: until now the double-click went to the phone, and an Esc held a little longer reached the phone through its auto-repeat after full screen was left. A key that is down on the phone is now always released there.
- 🧾 **One list of what the program is built from**: `win_native/build_scripts/build_inputs.tsv` is what `fetch_deps.sh` fetches from and what the About window shows; the build puts in the versions it really used.
- 🌐 All new texts in the eleven interface languages; README and user guide updated in all eleven.
- *Tested:* built with warnings as errors. The PC was in use, so the About window was driven and pictured on a second desktop that is never shown: its three pages in dark and light colours, in seven of the eleven languages (Traditional Chinese, English, Russian, German, Japanese, Thai, French); tabs, the switch and "Check now" with the mouse; "Copy report" on a window station of its own (30 lines of plain English). The update check asked GitHub for real: it found 1.10.0 and its package, said "up to date" as 1.11.0 and "newer available" as a build numbered 1.9.9, and ran by itself 30.5 s after the start of an idle run. "This PC" described this PC correctly (the one row it marked was a monitor running at 50 Hz) and showed the iPhone 16 Pro Max on the cable (iOS 27.2.1, Developer Mode on). The full-screen commands were sent to the engine test's window: double-click on, Esc off, its auto-repeat and release without effect, double-click on and off again. *Not yet seen:* the double-click and Esc while the iPhone is controlled (that needs a mirroring session with the phone; the code path differs from the tested one in one condition), Alt + double-click, the notification of a newer release, a link opening in the browser, a firewall that is switched on, and the other four languages on screen. All translations were written by a language model; the interface texts of six languages and three user guides by translation workers that were stopped half-way, everything else of the translations directly.

## 1.10 — 2026-11-12

- 🌐 **Russian** is the eleventh language of the interface, the README and the user guide (`-ui ru`).
- ⬆️ **Everything the program is built from is current** as of 2026-11-12: FFmpeg, OpenSSL and libplist at the heads of their main branches, llhttp 9.4.3, and the compiler toolchain whose runtime is linked in (clang, libc++ and libunwind 23.1.5, mingw-w64 r598). The Developer Disk Image (27B52), Apple's driver package (Apple Mobile Device Support 19.4.0.10) and the Windows SDK shader compiler were the newest already.
- 🔁 **The protocol core follows upstream UxPlay again** (up to its commit `05b0275` of 2026-11-06). Of the five changes made there since this tree branched off, the four that touch code of this tree are in: the AirPlay server's run state is guarded by a mutex that is now actually created (it was locked and unlocked but never initialized), a session pointer is cleared after its session is destroyed, a correction to how a short first packet of a mirror stream is decrypted (third-party senders), and a corrected H.265 unit-type read.
- *Tested:* built from scratch with the new toolchain and libraries, warnings as errors; the engine test decoded and showed all 480 pictures of the H.265 test stream at 60 fps; the console probe (same code, same libraries) reached the phone's services over the cable. *Not yet seen:* 1.10 itself in a mirroring session, and the Russian texts on screen (they were written by a language model, with Apple's and Microsoft's Russian terms looked up).

## 1.9.1 — 2026-11-12

- 🔧 **Fixed: control stopped answering after a while without input.** The phone checks a silent connection with keep-alive probes, and the program's own TCP did not answer them, so after two to three quiet minutes the phone reset the connection. The next click then went nowhere while MirrorDock connected again (under a second). The probes are answered now; a control session that sat idle for 200 seconds stayed up.
- 🔍 A session now notices at once when the phone ends one of its two connections, instead of at the next click.
- *Tested:* found in the owner's first session with 1.9 (three resets in twelve minutes, each followed by an automatic reconnect) and reproduced with the console probe: a connection that says nothing for 180 seconds was reset by the phone before the fix and answers after it. With the fix, a whole control session stayed ready through 200 idle seconds. Otherwise as 1.9.

## 1.9 — 2026-11-12

- 🖱️ **Control the iPhone with mouse and keyboard.** Press **F8**: the left button is a finger (tap, long press, swipe), the wheel scrolls, the middle button is Home, the keyboard types on the phone (Ctrl is ⌘), Page Down / Page Up and Alt + arrow keys flick to the next or previous page or video. Needs iOS 27, the USB cable and Developer Mode; the picture may still come over Wi-Fi.
- 🧩 It is built into `mirror_dock.exe`: a new part of the program, `phone_control/` (C++26 modules, GPL-3.0-or-later), speaks to the developer services of iOS directly — Apple's driver service, lockdown, the developer tunnel with an IPv6/TCP implementation of its own (no network adapter, no administrator rights), RemoteXPC and the phone's input service. No helper program, no Python, no jailbreak.
- 📦 A phone that lacks Apple's Developer Disk Image gets it installed on first use (and after an iOS update): the image is downloaded from a pinned commit of the DeveloperDiskImage repository and checked against pinned hashes, Apple's signing service signs it for the phone (over HTTPS that accepts Apple's root certificate only), and the phone's own installer takes it. A notification says what the wait is for.
- 🪟 While control is on the window title says so, the control bar stays out of the way and F8, F11, Alt+Enter and Alt+F4 keep working on the window. When control cannot be had, a notification names the reason (cable, trust, Developer Mode, iOS version, image).
- 🌐 All new texts in the ten interface languages; README and user guide updated in all ten.
- 🔧 Build: the failure and log modules are a target of their own (`uxplay_base`), so that `phone_control` does not depend on the engine that uses it; the executable is now about 12 MB (TLS towards the phone).
- *Tested:* with an iPhone 16 Pro Max (iOS 27.2.1) on the cable, through a console probe built from the same code: the connection, the control session (ready after 1.0 s; touch and keyboard reports accepted, the touch report being byte for byte the one the reference implementation builds; the keyboard registered), and the installation of the Developer Disk Image on a phone that lacked it (2.3 s with the image cached), including Apple's signature. With an interim build that spoke the same protocol through a Python helper, the owner clicked, scrolled and typed on the phone. *Not yet seen:* a click arriving in an app through the built-in code in MirrorDock itself, the flicks, a phone held sideways, and the failure messages.

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
