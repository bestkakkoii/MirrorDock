# README_AI — MirrorDock (authority entry)

This file is the entry point for maintainers and agents working on this
tree. `README.md` is the user-facing overview; this file records the
architecture, invariants, build inputs and verification state.

## What this tree is

**MirrorDock**: UxPlay 1.74 (AirPlay mirroring / audio receiver) reduced to a
single Windows-native program for the iPhone → Windows AirPlay use case. The tree
builds **only** this program (MSYS2 CLANG64; the top-level CMakeLists stops
on other hosts). The program has its own name and version (`win_native/include/
uxplay_version.h`, currently MirrorDock 1.11.0 (with `UXPLAY_RELEASE_DATE`, the day it was published): the one place they are written
— log, "About", the version resource and the settings/log locations read it;
only `uxplay.manifest`, `package_zip.py` and the CMake `OUTPUT_NAME` repeat
them); `UXPLAY_CORE_VERSION` there is the upstream UxPlay release the
protocol core derives from. It is
**runtime-adaptive to any Windows 10/11 x64 PC** (see "Adaptivity &
portability"), and tuned/verified on:

* Host: ASUS laptop, i7-6700HQ (Skylake), Intel HD Graphics 530 (drives the
  displays: MSI MP341CQ 3440x1440 over HDMI at 50 Hz, cloned with the 60.02 Hz
  built-in panel), GTX 960M (no outputs; Optimus), 16 GB, Windows 11 26200,
  Intel AC 7265 Wi-Fi.
* Sender: iPhone 16 Pro Max (A18 Pro, USB-C, iOS 26.6), Wi-Fi AirPlay and
  AirPlay over the USB cable (Personal Hotspot link).

## Names

Up to 1.6 the program was published as "UxPlay for Windows (native)"; since
1.7 the product is **MirrorDock** (owner's choice, 2026-11-11; repository
`bestkakkoii/MirrorDock`, formerly `bestkakkoii/UxPlay-USB`).

* **Renamed (what a user sees):** the executable (`mirror_dock.exe`, CMake
  `OUTPUT_NAME` of target `uxplay`), the default receiver name on the iPhone
  (`MirrorDock@<computer>`; `-n` still overrides), window/dialog titles, the
  version resource and manifest, the settings key (`HKCU\Software\MirrorDock`),
  the "start with Windows" Run value, the log folder
  (`%LOCALAPPDATA%\MirrorDock`) and its files (`mirror_dock.log`,
  `mirror_dock.previous.log`, `mirror_dock.start-failure.log`,
  `mirror_dock_crash.txt/.dmp`), the package (`mirror_dock_win64.zip`, folder
  `MirrorDock/`), README and user guide.
* **Not renamed (internal or shared with UxPlay):** module, file, namespace
  and CMake target names (`uxplay_*`, `uxplay`, `uxplay_native`), the
  `UXPLAY_*` macros, the single-instance mutex / window class / message names
  (`uxplay_native_*`), the startup file `.uxplayrc`, the pairing key and
  register files (`.uxplay.pem`, `.uxplay.register`), `SALT_PK` in
  `lib/crypto.c` (changing it would invalidate stored pairings), the
  environment guard `UXPLAY_RESTARTED_AT`. "UxPlay" in comments and log text
  refers to the upstream project or its protocol core.
* **Taken over once from the earlier name:** `settings::adopt_legacy_settings`
  copies every DWORD value of `HKCU\Software\UxPlay` that
  `HKCU\Software\MirrorDock` lacks (a value the user has set since under the
  new name stays; the old key is left for an earlier version), marks that with
  the value `legacy_settings_adopted`, and turns an "UxPlay" Run value into
  one for this executable. (1.7 used `RegCopyTreeW`, which was refused with
  "access denied" on the owner's PC: the settings were not carried over. The
  first start of 1.8 makes up for it.) `native_setup_output` renames
  `%LOCALAPPDATA%\UxPlay` to the new folder when the new one does not exist
  (it stays where it is while an earlier version has its log open; the new
  version then starts its own folder).

## Repository layout and licences

| path | content | licence |
|---|---|---|
| `uxplay.cpp` | the program: options, AirPlay (RAOP) callbacks, main event loop | GPL-3.0-or-later (upstream UxPlay) |
| `lib/` | AirPlay protocol core (RAOP/RTSP, pairing, FairPlay, NTP, RTP, mirror stream) | upstream: LGPL-2.1+ / GPL-3.0 (`crypto.c`, `playfair/`), MIT (`llhttp/`, `srp.c`) — see each file |
| `lib/mdnsd/` | embedded mDNS responder (replaces Bonjour/Avahi), USB-link following | per file header |
| `win_native/` | C++26 media engine, C API, build scripts, resources, tests | **MIT** (`win_native/LICENSE`, `SPDX-License-Identifier: MIT` in every file) |
| `phone_control/` | C++26 remote control of the iPhone over its developer services (see "Remote control of the iPhone") | **GPL-3.0-or-later** (`SPDX-License-Identifier` in every file) |
| `LICENSE` | GPL-3.0 text | — |

The program as a whole is GPL-3.0 (it links GPL-3.0 code). `win_native/`
alone may be reused under MIT. `phone_control/` is original code, but its
wire formats follow what the GPL-3.0 project pymobiledevice3 found out about
Apple's private protocols, so it is kept apart from the MIT part and under
the GPL. `package_zip.py` ships every applicable licence text in `licenses/`.

## Adaptivity & portability

Nothing about the target hardware is hard-coded; the engine inspects the host
at startup (and on change) and configures itself:

* **GPU:** `gpu_device` selects the D3D11 adapter that physically scans out the
  display (QueryDisplayConfig LUID) — integrated-only, discrete, Intel, AMD or
  NVIDIA alike; `-gpu auto|integrated|discrete|software` overrides
  (vendor-neutral DXGI GPU preference; `intel`/`nvidia`/`warp` are aliases).
  No hardware decoder for a codec → automatic software-decode fallback.
* **Codec offer:** H.265 is advertised to the iPhone only if the selected GPU
  decodes HEVC Main in hardware (probed via `CheckVideoDecoderFormat`);
  otherwise H.264 only, so a weak/old GPU still mirrors smoothly. `-h265`
  forces it on (software HEVC), `-h264` off.
* **Display:** monitor geometry and the DWM composition clock are queried at
  startup and **re-queried live on `WM_DISPLAYCHANGE`** (attach/detach a
  monitor, change resolution/refresh) → the presenter is re-paced without a
  restart (`video_presenter::set_refresh_period`).
* **Audio:** the default WASAPI endpoint's own minimum IAudioClient3 period is
  used (fallback 10 ms IAudioClient); endpoint changes are followed live. The
  latency the output reports is what it keeps queued (the endpoint buffer,
  which a low-latency stream reports larger than its period) plus the stream
  latency; the engine's audio buffering floor follows it, also when the
  default endpoint changes without a format change.
* **CPU:** binary and dependencies are built for the portable `x86-64-v2`
  baseline (SSE4.2/POPCNT — every Win10/11 x64 machine, Intel or AMD);
  FFmpeg/OpenSSL still pick AVX2 hot paths at runtime via CPU dispatch.

## Program structure

* `uxplay.cpp` parses options (`~/.uxplayrc` first; `~` = `HOME`, else
  `USERPROFILE`), claims the single instance, opens the log, starts the engine
  (`native_media_start`), the mDNS responder and the RAOP server, then runs
  **session cycles**: `main_event_loop::run` until a reset/teardown (relaunch)
  or an exit request, then tears the session down and starts the next cycle.
  `exit_requested` is set once and never cleared, so an exit that coincides
  with a reset is never turned into a relaunch.
* **Start order in `main()`:** `native_setup_output` (decides only *where* the
  log goes) → `native_media_install_crash_report` → options → receiver name →
  `claim_single_instance` → `open_log_file` → engine → display query (waits,
  bounded, for the engine; an engine that failed ends the start here, before
  anything is advertised) → `choose_stream_geometry` → mDNS → RAOP → register.
* **Single instance:** one receiver per name and Windows session (named mutex
  `Local\uxplay_native_<FNV-1a of the name>`, owned until exit). A second
  start asks the running one to show itself (registered window message carrying
  a hash of the name, sent with a time limit; the window answers for its own
  name → "Already running" balloon), then waits 1.5 s for the mutex:
  an instance that is just ending releases it and the new start takes over;
  otherwise the new start exits 0 without touching the log. An instance that
  has no window yet (still starting) is asked again for about 6 s; one that
  neither answers nor ends is reported in a dialog (exit 1). At exit the log
  file is closed before the mutex is released, so the next instance keeps it
  as `mirror_dock.previous.log`; a start that fails before it became the instance
  writes `mirror_dock.start-failure.log` instead of touching `mirror_dock.log`.
* **Failures the user must see:** every start-up error goes through
  `startup_message()` (logged as an error and kept as the reason);
  `startup_exit()` / `cleanup(non-zero)` show it in a dialog that names the log
  file when there is no terminal (`native_media_show_start_failure`). This
  covers option errors, an RTSP server that cannot listen (`raop_start_httpd`
  results are checked at start, on relaunch and by the watchdog) and an engine
  that could not start (`NATIVE_MEDIA_EVENT_START_FAILED`).
* **Log:** started from a terminal or with redirected output the log goes
  there; otherwise to `%LOCALAPPDATA%\MirrorDock\mirror_dock.log`. Every line carries
  the local time and passes one lock; lines written before the single-instance
  check are held in memory, the previous run is kept as `mirror_dock.previous.log`,
  and the file starts over at 8 MB (`LOG_FILE_LIMIT`).
* **Crash record** (`uxplay_crash_report`): an unhandled exception or
  `std::terminate` writes `mirror_dock_crash.txt` (exception code, faulting address
  as module+offset, thread) and `mirror_dock_crash.dmp` (dbghelp `MiniDumpWriteDump`,
  loaded on demand) next to the log; the next start logs that a record exists.
* **Main event loop** (`main_event_loop` in `uxplay.cpp`; GLib is not used):
  tasks posted from other threads (engine events, Ctrl+C) run on the main
  thread; every second it checks client feedback (`-reset n`) and that the
  RTSP server thread is alive (when it ended by itself the cycle ends and the
  relaunch block stops audio and video and starts it again; more than 5 such
  restarts within a minute end the program with the reason);
  RAOP threads request a reset with `request_reset()`, which ends the run after
  a 100 ms grace so the requesting handler (e.g. TEARDOWN) finishes before
  main() removes connections — the same ordering the old 100 ms GLib poll gave.
  A reset requested during the wait shortens that wait. **No request is
  lost:** one made between two runs is kept and ends the next run; after each
  run main() acknowledges the requests (`acknowledge_resets`) and takes the
  session flags (`relaunch_video`, `reset_httpd`, `connection_handed_over`,
  `close_window`) with `exchange`, so a flag set later belongs to the next
  cycle.
* **Winsock** is initialized once at the start of `main()` and stays so:
  `raop_destroy()` ends with `WSACleanup`, and without that extra reference it
  closed every socket of the process — the mDNS responder's too — before the
  goodbye was sent.
* **Engine events** (`native_event_callback`): window closed, exit, link
  preference, performance mode changed and display changed (both recompute the
  advertised stream geometry), start failed, device lost (the Direct3D device
  cannot be re-created in place: the program starts itself again with the same
  command line; a second loss within a minute of such a restart ends with a
  dialog instead, `UXPLAY_RESTARTED_AT` in the child's environment), session
  ending (Windows shutdown/sign-out: RAOP and mDNS are stopped, then
  `native_media_network_stopped()` releases the window thread, which waits at
  most 2 s), update installed (a newer release was put in place of the
  running program: the program ends and starts
  `native_media_restart_target()` with the same command line; when it was
  ending anyway, it is not started again).
* **Connection replacement:** when the AirPlay core replaces the connected
  client inside `conn_request` ("nohold", or the hand-over of the same client
  address whose old connection has been silent for `RAOP_HANDOVER_IDLE_SECS`
  = 5 s), `video_reset(RESET_TYPE_NOHOLD)` sets `connection_handed_over` and
  the relaunch that follows does **not** call `raop_remove_known_connections`
  (that would drop the connection that was just accepted).
* AirPlay video/audio callbacks call the engine's C API directly
  (`native_media_airplay_video_*`, `native_media_airplay_audio_*`,
  `native_media_set_volume`); there is no renderer abstraction layer.
* Options of other UxPlay builds (GStreamer pipelines/sinks, HLS, MP4,
  flip/rotate, BLE beacon, Raspberry Pi, D-Bus, macOS p2p) are accepted and
  ignored with a warning (`skip_unsupported_option`), so a shared startup file
  keeps working; `-vs 0` / `-as 0` keep their meaning (no video / no audio).
  Unknown options still stop with exit code 1.
* `lib/` serves only RAOP connections (RTSP/1.0 with CSeq). The HLS player
  subsystem (`airplay_video.c`, `http_handlers.h`, `fcup_request.h`), the
  reverse-HTTP (PTTH/EVENT) channel, the Bluetooth-LE beacon request path and
  the non-Windows/macOS-p2p branches were removed; without an HLS player they
  were unreachable (raop rejected every request without CSeq).

Engine modules (`win_native/src`, C++26 named modules, `import std;`):

| module | role |
|---|---|
| `uxplay_status`, `uxplay_log`, `uxplay_clock` | explicit failure outcomes, log sink, QPC-ns timeline |
| `uxplay_system_tuning` | EcoQoS/power-throttling opt-out, MMCSS, priority, DPI |
| `uxplay_gpu_device` | D3D11 device on the GPU that physically scans out the monitor (QueryDisplayConfig LUID), DWM composition clock |
| `uxplay_video_window` | Win32 window + UI thread: placement per orientation, aspect-locked resizing, borderless fullscreen toggle (double-click/F/F11/Alt+Enter, Esc), always on top (T), dark title bar, keep-awake while a stream plays, tray icon/menu (modes, link, mute, screenshot, start with Windows, log, update check, about), the same menu on a right click anywhere on the picture (`WM_CONTEXTMENU`; a right click that cancels a frame opens none), second-launch and session-end messages; **F9** asks for a screenshot (a key of the window's own, also while the phone is controlled); `notice()` shows a line of text on the picture |
| `uxplay_settings` | per-user settings in `HKCU\Software\MirrorDock` (read/write with explicit outcomes) and the "start with Windows" Run entry |
| `uxplay_crash_report` | unhandled-exception / terminate record and minidump next to the log |
| `uxplay_video_overlay` | player-style control bar: layered, non-activating popup owned by the video window, Direct2D/DirectWrite (software rasterizer), Segoe Fluent/MDL2 icon glyphs; fades in on real pointer movement, out after 3 s idle (stays while hovered); link switch, screenshot, mute, menu, fullscreen, alert chips. A second window of the same kind is the notice: a pill of text over the picture that goes away by itself (click-through, 1.8 s, then a fade) |
| `uxplay_video_decoder`, `uxplay_video_frame` | FFmpeg H.264/HEVC, D3D11VA, shader-readable decoder textures (zero copy); surface pool sized from the stream (below); falls back to the software decoder by itself and replays the units since the last parameter sets; `luma_reader` (CPU view of a picture's luma for the crop detector) |
| `uxplay_video_presenter` | flip-model swap chain, frame-latency waitable (max latency 1), timestamp-scheduled presentation on a high-resolution waitable timer, per-mode drop policy, crop region (`set_crop`), frees every per-stream GPU object and the swap chain when nothing is shown, reports a lost device; `capture()` hands back the picture that is on screen in its own resolution (the render thread draws it once more, with the same shader, into an off-screen texture and reads that back) |
| `uxplay_crop_detector` | pure logic: black-bar measurement of a luma plane and the cautious policy that turns it into a crop region (below) |
| `uxplay_audio_decoder`, `uxplay_audio_output` | AAC-ELD/ALAC/AAC-LC/LPCM -> float32 at device rate; WASAPI IAudioClient3 low-latency period, per-sample scheduling with cubic-resampling rate correction (at most 1.2 %, so that it can follow the timeline's glide while the phone is controlled; in step with the clock it is a few parts in a million), mute, soft limiter; the stream runs only while a source has audio queued (stopped after 3 s of silence, endpoint released 2 s later when no source is in use), endpoint failures are retried with back-off |
| `uxplay_media_engine` | the AirPlay pipeline and the shared presentation timeline |
| `uxplay_media_types` | media source identity (AirPlay), `performance_mode` |
| `uxplay_ui_text` | interface text table in eleven languages (completeness and `{}` placeholder counts checked at compile time), Windows display-language detection, DirectWrite locale |
| `uxplay_usb_link` | iPhone USB network link (AirPlay over the Personal Hotspot cable): attach/link detection, tray status and hints, link classification for the window label, cellular-data guard |
| `uxplay_update_check` | asks GitHub for the newest release (one request, see "About window" below) and compares it with the running version; `update_checker` checks on request and once a day on a thread of its own; it also names the release's package and its signature file for the installer |
| `uxplay_release_signature` | SHA-256, and the check of a release's signature (ECDSA P-256 through Windows CNG) against the public key the program carries |
| `uxplay_zip_archive` | reads a zip that is in memory: stored and deflated entries (its own inflate), every file checked against its CRC-32; anything else a zip can be is refused |
| `uxplay_update_install` | puts a newer release in place of the running program when the user asks for it (see "Installing an update" below) |
| `uxplay_build_info` | data only: the inputs of the build with the versions it really used, and the program's own versions with their dates; both written by `win_native/CMakeLists.txt` when the build is configured |
| `uxplay_system_report` | "This PC": measures what matters to the program (facts), judges it into rated rows with a score, words the rows in an interface language, and gives the plain-text report; `report_collector` measures on a thread of its own; a row may name a `report_action`, the repair offered beside it |
| `uxplay_system_repair` | carries out a `report_action`: a firewall rule (`netsh`) or Apple's service (`sc`) through `uxplay_elevation`, or the page of Windows' Settings where the matter is settled |
| `uxplay_elevation` | starts one of Windows' own tools with administrator rights behind the consent prompt (`ShellExecuteExW`, "runas") and waits for its exit code; no code of the program runs elevated. Used by the USB link's adapter switch and by the repairs |
| `uxplay_screenshot` | writes a picture as a PNG file (WIC) into `Pictures\MirrorDock` and puts it on the clipboard (a bitmap, and PNG for the programs that know it) |
| `uxplay_about_window` | the About window: a window and a thread of its own, three pages drawn with Direct2D/DirectWrite (software rasterizer), light or dark as Windows is set, per-monitor DPI, scrolling, mouse and keyboard; it shows what it is handed (since 1.12 also how an update goes) and reports what the user wants (also a repair of a row, "Update now", "Restart now") |
| `native_media_bridge.cpp` | C API (`win_native/include/native_media.h`) used by `uxplay.cpp` |

## Invariants

* **Timeline:** every local time is `raop_ntp_get_local_time()` = QPC in ns
  (`lib/raop_ntp.c`); DXGI and WASAPI QPC positions are on the same timeline.
* **Sync:** `present_time = media_time + delay`; audio and video share one
  media-time mapping and one delay, so lip sync holds by construction. The
  delay tracks the 95th percentile of **video** lateness (decode completion)
  plus a margin, with a fixed audio buffering floor; it is clamped to ≤220 ms
  and eases back toward target (~1/8 of the gap per 100 ms, recovers in
  ~1–2 s). Audio lateness does **not** drive the delay (a mid-stream audio
  cold-start would otherwise spike it); audio stays in sync because it is
  scheduled against the same delay and re-anchors its own queue.
  **While the phone is controlled (since 1.12) the delay may go below zero.**
  An iPhone stamps pictures and sound ahead of the moment they arrive
  (measured over Wi-Fi with 1.11, 3130x1440 HEVC at 30 fps: pictures were
  decoded 82 ms before their timestamp on average and no later than about
  50 ms before it, rare outliers apart; sound arrived 184 ms before its
  timestamp), and at rest all of that is waited for, which is what makes
  mirroring smooth. A hand on the mouse wants the picture sooner:
  `media_engine::set_control_ready(true)` makes the timeline *follow the
  lead*. The target is then the 95th percentile of the picture lateness
  over the last 90 pictures within 30 s (at least 20 of them) plus the
  margin, not below -150 ms, and not below what the sound needs (the median
  of its arrival lateness plus 30 ms for its scatter plus the buffering
  floor, once 40 packets were seen). The delay glides there, and back when
  control ends, by 1 ms per 100 ms; a picture that comes late still raises
  it at once. Sound and pictures keep one delay, so lips and voice stay
  together; the audio output follows the glide by playing up to 1.2 %
  faster or slower. The log's `sync delay` shows the value (negative while
  following).
* **GPU:** decode + present on the display-owning adapter (Intel here).
  Measured on this host (2868x1320@60, synthetic streams): Intel H.264 37 ms /
  HEVC 28 ms sync delay, 0–4 drops; GTX 960M H.264 70 ms, HEVC 122 ms with 70
  drops (`-gpu nvidia` keeps it selectable).
* **Verified on device (2026-11-02):** iPhone 16 Pro Max, iOS 26.6, Wi-Fi
  AirPlay mirroring, HEVC 664×1440 decoded by Intel HD 530 (D3D11VA). Sync
  delay ~14.7 ms over 8000+ presented frames, ~0 new drops; AAC audio in sync;
  lock/unlock recovers cleanly.
* **Threads:** main thread (`main_event_loop`), UI thread (window, tray,
  control bar), render thread (MMCSS "Playback"), decode thread, WASAPI thread
  (MMCSS "Pro Audio"), engine start thread, USB link monitor thread
  (`uxplay_usb_link`: IP Helper change callbacks only mark it dirty; it
  evaluates, publishes state to the tray and runs the elevated netsh guard, so
  a pending UAC prompt never blocks the UI), mDNS responder thread
  (`mdns_thread`: queries + 2 s link check), RAOP httpd/RTP/NTP threads. The
  D3D11 immediate context is guarded by one recursive mutex shared with
  FFmpeg's D3D11VA lock callbacks.
* **Performance modes** (`performance_mode`: quality / balanced / performance;
  menu radio items, `-mode`, persisted as `performance_mode`; balanced is the
  default and is the adaptive behaviour of earlier versions):

  | | quality | balanced | performance |
  |---|---|---|---|
  | video delay margin | refresh + 4 ms | refresh/2 + 4 ms | refresh/2 + 2 ms |
  | audio buffering floor | max(latency + 12 ms, 3 refresh) | latency + 12 ms | latency + 8 ms |
  | late frames | shown in order, dropped when > 2 periods late | in order, > 1 period | newest wins |
  | presenter queue / delay limit | 13 pictures / 220 ms | 8 / 135 ms | 8 / 135 ms |
  | stream size asked from the phone | monitor | monitor (1920x1080 box without a hardware decoder) | 1920x1080 box |

  The delay limit is the queue expressed in frame intervals of a 60 fps
  stream: a larger delay (after a network stall) would only make the
  presenter evict pictures unseen, i.e. freeze the picture while sound goes
  on. For the same reason no picture waits longer than that limit from the
  moment it is decoded (`present = min(pts + delay, ready + limit)`), also
  when pictures are ready earlier than their timestamps say. A stream starts
  at its resting delay; late pictures raise it at once. A switch to quality
  in mid-stream gets its deeper queue with the next stream (the surface pool
  is sized when a stream starts).

  **The sender's timestamps lead** (owner's iPhone 16 Pro Max, 1.7 log,
  2026-11-11): pictures are decoded 75 to 95 ms *before* their timestamps
  (`decoded min -94 mean -78 ms after its timestamp`), so at the resting
  delay every picture waits about 100 ms in the presenter queue (5 to 7
  pictures at 60 fps) whatever the mode. That is why performance mode no
  longer has a shorter queue and limit: with 5 pictures / 85 ms (1.6, 1.7) the
  queue overflowed and pictures were shown by arrival instead of by
  timestamp; and why a stream no longer starts above its resting delay (46 ms
  of start delay plus the lead exceeded the queue: `overflow +36` in the
  first interval). The modes now differ in margins, late-frame policy and
  stream size. Not done: letting the timeline run ahead of the timestamps
  (a negative delay from the lateness tails of both streams) would take about
  70 ms off the delay and most of the queue; it needs the audio lateness
  figures the 10 s audio line now logs (`arrived min/mean/max ... ms after
  their timestamps`).

  The engine part (`media_engine::apply_performance_mode` → presenter) applies
  at once. The stream size (`choose_stream_geometry` in `uxplay.cpp`,
  `raop_set_plist`) is read by the client when it connects, so it applies to
  the next connection; the balloon says so when a client is connected. `-s`
  and `-fps` given by the user are never overridden by a mode.
* **Crop** (`crop_mode`: off / automatic / manual; menu radio items, keys C
  and X, `-crop auto|off`, persisted as `crop_mode` 0/1 — a hand-framed part
  is never stored). The phone mirrors its whole screen: a 16:9 film on a
  19.5:9 screen arrives with black bars that are part of the stream.
  * *Automatic:* the decode thread hands one decoded picture every 200 ms to
    `luma_reader` (hardware pictures: one staging copy + map, kept only while
    this mode is on) and `crop_detector::examine`. A bar is a run of rows or
    columns from an edge that is black (luma ≤ 24) over its whole length; bars
    count per axis only as a pair (both ≥ 2 % of that edge, sizes within 2 %);
    what remains must be ≥ 1/5 of the picture each way. A crop starts after
    the same bars were seen for 1.5 s **and** the content inside changed three
    times (moving content, not a still screen); tightening an existing crop
    takes 8 s. Content inside a bar ends the crop (examined every 100 ms
    while a crop is in force): when the picture still has a pair of smaller
    bars the crop is loosened to them, otherwise the whole picture is shown.
    *When* depends on the age of the crop. Younger than 5 s it was probably
    wrong: it ends at once and the next attempt is delayed (3 s, doubling to
    30 s, back to 3 s once a crop has lasted 5 s). A crop that has lasted 5 s
    was right for the film that played, and what crosses its bars is most
    likely the change to the next film: it is tolerated for 1.2 s (every
    picture is examined meanwhile; a black picture ends the tolerance, nothing
    is in the bars then), after which the crop ends without any delay for the
    next one. When no picture comes for 250 ms after those 1.2 s the stream
    has gone still with content in a bar: the decode thread's wait times out
    (`crop_detector::grace_deadline` / `expire`) and the whole picture is
    shown. (Up to 1.7 every change of film counted as a wrong crop: the
    delay doubled to 30 s and was reset only by a crop that lasted a minute,
    so with short films the crop seemed never to come back - reported by the
    owner.) The shown region stays 2 px inside the measured
    edge and is even (4:2:0). The engine applies a decision under the video
    pipeline lock after re-checking the session, and tells the presenter again
    when what it shows differs from the detector's region at two examinations
    in a row. Known limit: content that appears in a cut-off bar within the
    last 100 ms before the phone's screen goes still is examined only with the
    next picture (the picture belongs to the presenter after `submit`).
  * *Manual:* the window's framing mode (`tray_command::crop_select`) reports
    the dragged rectangle as fractions of the visible picture
    (`window_callbacks::on_crop_selected`); the engine converts it to picture
    pixels (`apply_crop_selection`). It lasts until the stream changes (the
    presenter drops a region when the picture size changes) or a standing mode
    is chosen again.
  * The presenter shows only the region (`video_presenter::set_crop`), sizes
    its viewport from the region's aspect ratio and reports it as
    `presented_stream::visible`; the engine sizes the window, the title and
    the control-bar label from the visible part, so the window hugs the
    content. Mode and region change together under `crop_mutex`.
* **GPU memory:** the D3D11VA surface pool is sized as *pictures the stream
  keeps + 4* (H.264: `refs + has_b_frames`; H.265: `sps_max_dec_pic_buffering
  - 1` from the active SPS, else libavcodec's 17) *+ the presenter's hold*
  (queue limit + 3: 11 / 11 / 16 for performance / balanced / quality), where
  it used to be a fixed 23 surfaces (170 MB at 3130x1440). **This saves
  memory only for streams that keep few pictures.** The synthetic test stream
  keeps 2 (pool 17); the owner's iPhone declares far more (1.7 log: `26
  surfaces of 768x1536 (15 for the decoder, 11 for presentation)` upright,
  `22 surfaces of 3200x1536 (11 for the decoder, 11 for presentation)`
  sideways), so a real landscape stream gets about the pool it had before.
  libavcodec's
  D3D11VA cannot grow a pool, so it is right-sized, not dynamic; the log line
  `D3D11VA surface pool: N surfaces of WxH (X for the decoder, Y for
  presentation)` shows the result. At the end of a session the presenter
  frees its views, copy/upload textures and the swap chain, and the decode
  thread frees the decoder with its pool.
* **Presentation timing:** the render thread sleeps on a high-resolution
  waitable timer until the head of the queue is due and is woken early only
  when the head changes; the queue lock is never held across a draw or
  Present. Process priority (above normal) and the power-throttling opt-out
  apply only while a stream is shown (`system_tuning::set_session_active`).
* **Window placement:** a portrait stream opens at 0.9 of the work-area
  height, a landscape one at 0.5 of the work-area width (not below 960 DIP,
  never above 0.95 of the work area). `WM_SIZING` keeps the stream's aspect
  ratio; the size the user leaves (as a fraction of the work area) and the
  window centre are remembered per orientation (`window_landscape_permille`,
  `window_portrait_permille`, `window_center_x/y`) and reused on rotation and
  on the next start. A rotation while fullscreen or maximized is applied when
  that state is left.
* **System side effects:** UxPlay changes the PC only through two opt-in,
  UAC-confirmed actions on the iPhone's network adapter — the cellular-data
  guard (netsh) and re-enabling a disabled adapter (pnputil); see the USB
  section for exact values and how to undo. Per-user state: the values under
  `HKCU\Software\MirrorDock` (`prefer_usb_link`, `performance_mode`,
  `crop_mode`, `always_on_top`, the window placement values, two "hint shown"
  flags, `legacy_settings_adopted`) and, only when the user ticks "Start with
  Windows", the value `MirrorDock` under
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` (the quoted path of the
  running exe, refreshed at start so a moved copy keeps working). A declined
  prompt or a failed tool is logged with its code and reported in a balloon;
  the tray item stays available to retry. Test instances launched from an
  elevated shell run these tools without a prompt: do not exercise them on the
  owner's machine without asking.
* **Server robustness (`lib/`):** a failed `accept`/`select` no longer ends
  the RTSP server thread (retry, back-off after 3 failures in a row, give up
  after 30 — `raop_is_running` is then 0 and the main loop's watchdog starts
  it again); the mirror data socket is non-blocking (a silent client cannot
  delay a stop); lengths taken from the mirror stream are validated (payload
  ≤ 32 MiB, parameter-set sizes inside the payload) and a violation resets the
  connection (`conn_reset` reason 2) instead of ending the process; the mirror
  data port accepts only the session's own client address; the host's
  `conn_init`/`conn_destroy` count RAOP clients only.
* **Shutdown:** server threads are woken through a loopback socket
  (`netutils_wakeup_*`, used by httpd) and a lost-wakeup-free NTP stop. Ctrl+C
  posts an exit task to the main loop; measured 0.05 s from Ctrl+C to process
  exit (exit code 0, 2026-11-03). Startup to advertised: 476 ms (second run
  of the packaged exe; the first run of a new file took 1687 ms while
  Defender scanned it). On console close the handler waits (≤4 s)
  for the orderly shutdown so mDNS goodbyes are sent. Startup failures exit
  with code 1.
* **Interface language:** chosen once in `native_media_start`, before the
  engine thread creates the window, tray menu and control bar:
  `native_media_settings::ui_language` (`-ui`) or else the first entry of
  `GetUserPreferredUILanguages`. Eleven languages (`ui_language`, in this
  order, which is also the order of the texts in every table row and, plus
  one, the numbering of the C API): Traditional Chinese (zh-TW/HK/MO and
  `zh-Hant`), Simplified Chinese (other `zh`), English, Japanese, Korean,
  Spanish, German, French, Vietnamese, Thai, Russian — chosen by the primary
  language
  subtag; anything else → English (a detection failure is logged and falls
  back to English). All user-visible strings come from `ui_string(ui_text)`;
  two `static_assert`s make a missing text, or a text whose number of `{}`
  placeholders differs from the English one, a compile error. The control
  bar measures label widths, and its DirectWrite locale (zh-TW, zh-CN, en-US,
  ja-JP, ko-KR, es-ES, de-DE, fr-FR, vi-VN, th-TH, ru-RU) selects matching
  fonts;
  scripts the UI font lacks (Thai, for example) come from DirectWrite's font
  fallback. Log messages and `-h` stay English.
* **Translated documents:** `README.md` (English) is the master of
  `README.<lang>.md` (zh-TW, zh-CN, ja, ko, es, de, fr, vi, th, ru), and
  `win_native/package/user_guide/user_guide_en.txt` the master of the ten
  other guides there; `package_zip.py` refuses to build a package without all
  eleven guides. A translation is the same document in another language: same
  sections, tables, lists, code blocks, links and emoji (checked with a
  structure comparison when they were written), menu items quoted exactly as
  in the interface texts of that language. Change the master first, then
  every translation in the same commit. The table in `uxplay_ui_text.cpp` is
  maintained by hand; this file (README_AI.md) and CHANGELOG.md exist in
  English only.
* **Failures** are explicit `uxplay::outcome` values with stage, original
  code and system/library message; nothing fails silently.
* **Audio output:** UxPlay's own mute (control bar / tray) is engine-owned and
  ramped like a volume change; a soft limiter (unity below -1 dBFS, tanh knee
  above) catches decoded overs — AirPlay AAC-ELD peaks of +2.5 dBFS were
  measured, which the endpoint would otherwise hard-clip. The 10 s statistics
  line reports audio packets, empty/failed decodes, decoded peak, played
  frames, volume and mute, so a silent session shows where sound stops (a "no
  sound" report on 2026-11-03 was traced this way to a mute outside UxPlay).
* **UI stack (recorded exception to the owner's desktop-GUI default of a
  webview stack such as C++26 + saucer):** the only GUI is the video window,
  its control bar and the tray menu, built with Win32 + Direct2D/DirectWrite
  inside the C++26 module engine. Reasons: the video is a D3D11 flip-model
  swap chain whose lowest-latency path (independent flip in fullscreen) must
  stay free when the controls are hidden — a separate layered popup does that,
  a web view over the video would need permanent composition or
  DirectComposition interop with a browser surface; a WebView2 runtime would
  add a multi-process browser to a ~11 MB player; the controls are seven
  buttons. No Qt anywhere.

## AirPlay over the USB cable

The iPhone's QuickTime screen-capture USB protocol is not used: Apple disabled
it from iOS 16 on (verified on iOS 26.6: the capture endpoint sends zero
bytes after a successful lockdown/TLS session), so that code and libusb were
removed. The working USB path is **AirPlay over the iPhone's USB network
link**, automatic, no flag. With the iPhone on Apple's driver and Personal
Hotspot on, Windows exposes an "Apple Mobile Device Ethernet" NIC
(172.20.10.0/28; iPhone at .1, ping <1 ms); the mirror is local USB traffic.
`install_apple_usb_support.cmd` winget-installs the Apple driver.

* **Advertising follows the cable live** (`lib/mdnsd/mdnsd.c`:
  `mdns_pick_ipv4`, `mdns_follow_link`, every 2 s, time-based): a preferred
  172.20.10.x address wins (USB first), else the LAN adapter with a gateway
  and the lowest interface metric (Ethernet/Wi-Fi only; Hyper-V, WSL, VM
  adapters and tunnels such as Tailscale are skipped). On a switch the
  responder sends a goodbye on the old link, moves its IPv4 multicast
  membership (`IP_DROP/ADD_MEMBERSHIP`) and `IP_MULTICAST_IF`, announces on the
  new link and repeats once after 1 s (RFC 6762 §8.3). An open AirPlay session
  pins the advertisement (`dnssd_set_session_active`, from
  `conn_init`/`conn_destroy`) until it ends, unless its link vanished. The
  membership is re-asserted on every check: Windows answers a duplicate join
  with WSAEINVAL (probed), so a successful re-join means the OS had dropped it
  (link flap, resume) and triggers a re-announce.
* **Link preference** (control bar / tray "USB first" / "Wi-Fi only",
  persisted as `HKCU\Software\UxPlay\prefer_usb_link`, DWORD, default 1).
  Leaving USB mode must not touch the USB device: on the owner's setup
  (iPhone charged through an externally powered cable) disabling the "Apple
  Mobile Device Ethernet" adapter stopped the charging until the power was
  replugged. "Wi-Fi only" therefore only changes UxPlay's own choice:
  `dnssd_set_usb_link_allowed(0)` makes the responder skip the USB link
  (applied within 250 ms, overriding the session pin), and
  `on_link_preference_changed` in `uxplay.cpp` ends a session that runs on the
  other link through the reset path (`reset_httpd`), because the iPhone does
  not reconnect by itself. The choice is applied before the first
  advertisement at startup.
* **"Wi-Fi only" is enforced, not only advertised** (1.6; owner report: with
  "Wi-Fi only" the mirror still ran over the cable until the Apple device was
  disabled). Root cause, confirmed with an mDNS probe on this host: the SRV
  record pointed at `<computer>.local`, and **Windows' own mDNS responder
  answers that name on every adapter** (A and AAAA, also on the Apple USB
  NIC), so the iPhone resolved the receiver over the cable no matter where
  UxPlay advertised. Four layers now:
  1. the SRV target is UxPlay's own host name, `<computer>-mirror.local`
     (`dnssd_make_host_name`), which only UxPlay's responder answers — with the
     one address of the chosen link (probe: no other responder for that name);
  2. queries are answered only when their source is on the advertised
     link's subnet (`mdns_source_on_advertised_link`, RFC 6762 §11);
  3. the RAOP server asks the host before it sets up a connection
     (`raop_callbacks_t::conn_admit`, local and remote address): in "Wi-Fi
     only" `conn_admit` in `uxplay.cpp` refuses a connection whose local
     address is on the iPhone USB link (`native_media_is_usb_link_address`,
     IPv4 172.20.10.x or an IPv6 address of that adapter) and logs it; httpd
     closes the socket;
  4. a Wi-Fi adapter that holds a 172.20.10.x address (the PC joined the
     hotspot over Wi-Fi) is not treated as the USB link (`IfType`).
  State: compiled; **not yet exercised with the iPhone** (the 1.5 process of
  the owner was running during the work).
* **Disabled adapter:** `uxplay_usb_link` reports an iPhone whose Net-class
  function has `CM_PROB_DISABLED` (`link_adapter_disabled`); in "USB first" the
  tray offers to re-enable it: one elevated
  `pnputil /enable-device "<instance id>"`, the ID checked against
  `[A-Za-z0-9&_\.-]`. pnputil enables exactly that function.
* **Window label** comes from the adapter that owns the connection's local
  address (`raop` `conn_transport(local_address)` →
  `native_media_set_airplay_local_address` → `is_usb_link_address`):
  172.20.10.x on a non-802.11 adapter is "USB"; a PC joined to the hotspot
  over Wi-Fi is labelled Wi-Fi.
* **`uxplay_usb_link`** watches the iPhone (CM device list,
  `USB\VID_05AC&PID_12*`) and its link (`NotifyIpInterfaceChange`,
  `NotifyUnicastIpAddressChange`, `NotifyRouteChange2`, `WM_DEVICECHANGE`
  from the window) and drives the tray: a USB status line, a "turn on Personal
  Hotspot" balloon when the iPhone is attached for 8 s without a link (once
  per run), and "USB ready". Notifications drive it; it polls every 5 s only
  while an attached iPhone has no link yet (the end of the grace period comes
  without a notification) and otherwise every 60 s as a safety net.
* **Cellular-data guard.** The iPhone gives the PC global IPv6 (observed
  `2001:b400:…/64`, carrier space) while this home Wi-Fi has **no IPv6**: with
  only the IPv4 metric raised, every IPv6-capable connection of the PC still
  went out through the phone's mobile data. The monitor checks
  `GetBestRoute2` toward public IPv4/IPv6 addresses; when internet leaves
  through the iPhone link while another network has a gateway, the tray item
  and a clickable balloon offer the guard (the click gives the foreground, so
  UAC opens directly): one elevated `cmd /c netsh …` (system32 paths, numeric
  arguments only) sets, persistently, IPv4 `metric=9999
  ignoredefaultroutes=enabled` and IPv6 `metric=9999 routerdiscovery=disabled
  ignoredefaultroutes=enabled` on that interface, then re-checks the routes.
  The link keeps carrying the mirror (on-link 172.20.10.0/28) but never
  internet, even if Wi-Fi drops; a deliberate tether (no other network) is
  never guarded. To tether over USB again:
  `netsh interface ipv4 set interface <idx> ignoredefaultroutes=disabled`,
  `netsh interface ipv6 set interface <idx> routerdiscovery=enabled ignoredefaultroutes=disabled`,
  `Set-NetIPInterface -InterfaceIndex <idx> -AutomaticMetric Enabled`.
* **Verified end-to-end (2026-11-02):** with Personal Hotspot on, UxPlay
  advertised `UxPlay.local → 172.20.10.9` and the iPhone mirrored over the
  cable — `Local 172.20.10.9 / Remote 172.20.10.1`, H.265 hardware decode, 0
  drops, sync delay 14.7 ms; the IPv4 default route stayed on Wi-Fi. **Not yet
  verified on the device:** plugging in while UxPlay runs (membership move),
  the hotspot hint and the guard's UAC flow.

## Remote control of the iPhone (`phone_control/`, since 1.9)

AirPlay mirroring has no channel for input. (Its media remote, DACP, is
offered by the phone while it mirrors - one line per session in the log says
so - but it only knows play/pause/next/volume and is not used. A Bluetooth
keyboard/mouse emulated by the PC needs the LE peripheral role, which the
test PC's adapter lacks.) Control therefore uses the **developer services of
iOS 27**, the ones Xcode's Device Hub uses to operate a real device. It needs
iOS 27, the USB cable, a paired ("trusted") PC and Developer Mode; the
picture keeps coming by AirPlay, over Wi-Fi or USB.

**Modules** (each a `.cppm` interface and a `.cpp` unit; nothing in them
depends on the engine):

| module | what |
|---|---|
| `uxplay_byte_stream` | the stream interface every layer is written against; a read can be given a patience |
| `uxplay_property_list` | property lists over libplist |
| `uxplay_usbmux` | usbmuxd (Apple Mobile Device Service, 127.0.0.1:27015): device list, connect to a port of the phone, pairing record |
| `uxplay_device_stream` | one connection to the phone, plain or TLS (OpenSSL, the pairing's certificate and key; the phone's side needs security level 0) |
| `uxplay_lockdown` | lockdown: session with the pairing, values, starting services; property-list messages |
| `uxplay_device_tunnel` | the lockdown service CoreDeviceProxy as a point-to-point IPv6 link, with this program's own IPv6 + TCP (no network adapter, no administrator rights); UDP towards a "discard port" is counted and dropped |
| `uxplay_remote_xpc` | RemoteXPC: HTTP/2 framing with flow control, XPC objects, file transfers on their own streams |
| `uxplay_core_device` | the service directory (RSD), the CoreDevice call envelope, the offer that starts a stream of the screen |
| `uxplay_cryptex` | cryptexd: what is installed, the chip's identity, a nonce, installing a cryptex |
| `uxplay_http_client` | one HTTP request through WinHTTP; optionally with one root certificate as the only trusted one |
| `uxplay_developer_image` | the Developer Disk Image: fetch, verify, cache, Apple's signature, installation |
| `uxplay_remote_control` | the session: a worker thread that connects, serves and ends; the only module the engine imports |

**Session** (`remote_control::state::connect`, worker thread): usbmuxd lists
the phone on the cable → lockdown session (TLS) → iOS version (below 27:
`unsupported_ios`) → Developer Mode (`com.apple.security.mac.amfi` /
`DeveloperModeStatus`; when off, the lockdown service `com.apple.amfi.lockdown`
is asked to show the switch in Settings and the session ends with
`developer_mode_off`) → tunnel → service directory → when the display and
input services are missing: install the Developer Disk Image (state
`preparing`) and read the directory again → `startmediastream` on
`com.apple.coredevice.displayservice` → 0.5 s → connection to
`com.apple.coredevice.hid.universalhidservice` → the phone's orientation →
`ready`.

* **The screen stream is a gate, not a picture.** iOS delivers HID reports to
  apps only while a media stream of the display service runs. Its RTP goes to
  a port of the tunnel that drops it (the picture the user sees is AirPlay's).
  The gate belongs to the phone, not to the connection: `stopmediastream` with
  `stopAll` (what a session sends at its end, so that nothing of a crashed
  session lingers and keeps the phone's camera and microphone blocked) ends
  the streams of **every** client. Two controllers on one phone therefore
  disturb each other; a probe that ends with `stopAll` cuts a running
  session's input without that session noticing.
* **Input.** Touch: 58-byte reports to the surface with service identifier
  257 (report 0x09; up to five contacts of `[state][x][y]`, 0..65535 on the
  phone's own upright panel; every finger still down is in every report; a
  48-bit nanosecond timestamp). Keyboard: a virtual keyboard is registered
  with the first key report (`createService`, the way Universal Control lends
  a Mac's keyboard), then 39-byte reports with one bit per HID usage.
  Buttons: `IndigoButtonEvent` on `com.apple.coredevice.hid.indigo` (a
  connection per press). Wheel scrolling and the flicks are a fifth finger of
  the program's own whose position follows the clock, not the number of
  reports. The engine speaks in 0..65535 across the picture as mirrored;
  `to_panel` turns that by the interface orientation SpringBoard reports
  (asked once a second and when the picture changes shape).
* **Developer Disk Image.** On iOS 27 it is a "Cryptex1": Xcode installs it
  through `com.apple.security.cryptexd.remote` with persistence "until the
  next software update", and so does `ensure_developer_image`. The image
  (17 MB) and its companions are not part of the program: they are fetched
  from the DeveloperDiskImage repository at a pinned commit, checked against
  pinned SHA-256 values and kept in
  `%LOCALAPPDATA%\MirrorDock\developer_disk_image`. Apple's signing service
  (`gs.apple.com/TSS/controller`) signs them for the phone: the request names
  the chip model, the phone's unique chip number and a nonce the phone made.
  That server's certificate comes from Apple's own authority, which Windows
  does not trust; the request therefore goes out over HTTPS with **Apple Root
  CA as the only accepted root** (embedded; checked in WinHTTP's
  "sending request" callback, before anything is sent). The reference
  implementation uses plain HTTP there.

**Invariants**

* Nothing that identifies the phone or the pairing is logged or shown: not
  the serial number or unique chip number, not the nonce or the ticket, not
  the pairing record's keys. Logged are the model, the iOS version and sizes.
* The worker thread owns the whole session; the public members only queue
  commands. What arrives before `ready` is dropped (it would be stale), a run
  of moves of the same fingers keeps only the newest, and a queue that
  overflows (the phone takes nothing) is dropped as a whole and followed by
  "release everything", so that nothing stays pressed.
* Every wait for the phone is bounded: lockdown and its services answer
  within 10 s or the read fails, a write that is not taken for 20 s fails,
  a connection inside the tunnel is made within 8 s, every call has its
  patience. The destructor therefore returns: at once normally, after the
  stage the phone hangs in otherwise.
* The tunnel's TCP has no retransmission and no congestion control: the link
  below it is TLS over the USB connection, which neither loses nor reorders.
  This is the reason control is USB-only so far. It does answer every
  segment that does not begin where the next byte is expected: that is how
  the phone probes a silent connection (keep-alive), and it resets a
  connection whose probes go unanswered (1.9 did not answer; see "State of
  1.9.1").
* A failure of the touch path ends the session (`connection_lost`); the
  engine starts a new one, three times in a row at most. A keyboard the
  phone will not register, a button service that is missing or an
  orientation the phone does not report are logged and do not end it.
* Every connection inside one tunnel presents the same identity: sixteen
  bytes from the pairing's HostID.

**Engine and window.** `media_engine` keeps a control thread
(`control_loop`): a session exists while the user wants control (F8, menu;
remembered as `remote_control`) and a stream is shown. `control_status` turns
the session's reports into the title ("connecting…", "control"), a
notification the first time control is ready, one while the phone is being
prepared, and one with the reason when control cannot be had. A new USB
device triggers another attempt after `no_device` / `not_trusted`. The window
forwards input only while control is ready (`controlling()`): left button,
wheel, middle button, keys as HID usages (`keyboard_usage` maps scan codes),
Page Up/Down and Alt+arrows as flicks; F8, F11, Alt+Enter, Alt+F4 and Esc in
full screen stay with the window.

**Not implemented:** control over Wi-Fi (the phone's RemotePairing tunnel:
another handshake, and a real TCP), the media remote as a fallback without
Developer Mode, phones with more than one screen (the touch surface would
have to be chosen by display), restarting the screen stream when another
client stopped it.

## The About window, the update check and "This PC" (since 1.11)

**Who owns what.** `native_media_bridge.cpp` creates four objects once the
engine exists (`create_about`): the `about_window` (no window yet), the
`update_checker`, the `report_collector` and (since 1.12) the
`update_installer`. The window only shows and asks: `set_update` /
`set_facts` / `set_install` hand it what to show, `about_events` reports
what a user can want (check now, automatic on/off, measure again; since
1.12 a repair of a row, "Update now", "Restart now").
`show_about` runs on the video window's thread (menu command, click on the
notification): it opens the window or brings it to the front, above a video
window that is "always on top", and requests the facts when the window was
not open. If the window cannot be created, a plain message box says version
and project address.

`native_media_stop` ends them in this order: the About **window** first
(`about_window::close`: its buttons call the others), then the checker, the
installer (the checker may hand it a release) and the collector (their
threads hand results to the About object, which still exists, and read the
link monitor and the engine), then the About object, then the link monitor
and the engine. None of them makes the end of the program wait for the
network or the phone: the HTTP requests and the question to the iPhone run
on helper threads that own nothing but a shared block, and a stop abandons
them (they log nothing). A repair runs on a thread of its own as well
(behind it may be a consent prompt nobody answers); what it does when it
is over it does only while the bridge says the engine is still there.

**What leaves the PC.** The update request: `GET
https://api.github.com/repos/<owner>/<project>/releases/latest` with
`User-Agent: MirrorDock/<version>` and `Accept: application/vnd.github+json`
(`uxplay_version.h` names the address, the tag prefix and the package name).
Nothing else is fetched until the user clicks "Update now": then two more
requests with the same `User-Agent` fetch the release's signature file and
its package from the project's release page (GitHub redirects them to its
file server; WinHTTP follows). The answer is parsed with libplist's
JSON reader (`property_list::parse_json`); of the addresses in it only those
below `UXPLAY_PROJECT_URL` are kept, anything else is replaced by the fixed
`/releases/latest` addresses, and the About window hands the shell nothing
but `https://` addresses and files of this PC. "This PC" sends nothing, and
no serial number, device name or network address gets into its facts: the
iPhone's identifier is used to open the lockdown session it is asked
through, and device instance identifiers (they contain that identifier) to
find Apple's drivers; neither is kept, shown or logged.

**When it checks.** At once when asked (menu "Check for updates…" or the
button). By itself once a day while "automatic" is on: 30 s after the start
at the earliest, 24 h after the last check that got an answer (kept across
runs), again after 1 h when a check was skipped or failed. An automatic check
is skipped while Windows calls the connection metered
(`INetworkCostManager`) or while the PC's internet goes through the iPhone
(`usb_link_state`); a failed automatic check changes nothing the user sees.
A newer release found by an automatic check is announced once per release
with a notification (clicking it opens the About window). Settings in
`HKCU\Software\MirrorDock`: `update_check_automatic` (default 1),
`update_checked_at` (seconds since 1970), `update_newest_version` (packed),
`update_notified_version`.

**How "This PC" judges.** `measure_system` and `measure_phone` only collect
facts (`system_facts`); `build_report` turns them into rows for one
interface language. A row is rated good, attention or problem and has a
weight, or it is information only. Score = share of the weight reached (good
counts in full, attention half, problem nothing), per group and overall;
rows without a rating do not count. The weights and thresholds are the
constants at the top of `uxplay_system_report.cpp`:

| row | weight | good | attention | problem |
|---|---|---|---|---|
| Windows | 6 | build 17763 (1809) or newer | older | |
| Display | 10 | every monitor that shows the desktop at 59 Hz or more | slower, or the desktop duplicated on monitors with different rates | |
| Power | 4 | mains, or battery without energy saver | energy saver on | |
| Network | 10 | wired or Wi-Fi with a gateway, 50 Mbit/s or more, not "public" | none, public, or slower | |
| Windows Firewall | 10 | off, or an allow rule for this executable | no rule for it | a block rule, or "block all" |
| Processor | 12 | 4 threads or more | 2 or 3 | 1 |
| Memory | 8 | 7.5 GiB or more | 3.5 GiB or more | less |
| Graphics card | 10 | a hardware adapter | | Windows' software renderer |
| Video decoding | 25 | H.265 in hardware | H.264 only | none |
| Sound output | 5 | a default output device | | none |
| Graphics driver (card in use) | 15 | found | | |
| Apple USB support | 10 | its service answers | installed, no answer; or not installed | |
| USB link | 5 | | adapter disabled while USB is preferred | |
| This program | 8 | newest release | a newer one exists | |

Information only: a second graphics card and its driver, Apple's two device
drivers, the stream on screen, the USB link otherwise, and the iPhone (it is
no part of this PC; what control still needs of it is said beside it). The
firewall verdict comes from the rules of Windows Firewall for exactly this
executable in the active profiles; another firewall product is not seen.
"Copy report" is `report_text`: the same rows in English.

**Repairs on "This PC" (since 1.12).** A row that is not in order may name
a `report_action`; the About window shows it as a button under the row's
advice, and `uxplay_system_repair::repair` carries it out:

| action | offered when | what it does |
|---|---|---|
| `allow_through_firewall` | the firewall is on and has no rule for this executable, or a rule that blocks it | elevated `netsh advfirewall firewall add rule name="MirrorDock" dir=in action=allow program="<exe>" enable=yes profile=<the active kinds of network>`; with a blocking rule instead `set rule name=all dir=in program="<exe>" new action=allow enable=yes` |
| `open_firewall_settings` | the firewall blocks every incoming connection (`firewall_verdict::shut`: no rule can help) | `windowsdefender://network/` |
| `start_apple_service` | Apple's USB support is installed and its service does not answer | elevated `sc start "Apple Mobile Device Service"` |
| `install_apple_support` | Apple's USB support is not installed | the packaged `install_apple_usb_support.cmd`; without that file the Store page of "Apple Devices" |
| `enable_usb_adapter` | the iPhone's network adapter is disabled while USB is preferred | the link monitor's own elevated switch (as in the menu) |
| `install_update` | a newer release exists | the update, as "Update now" |
| `open_display_settings`, `open_network_settings`, `open_power_settings`, `open_sound_settings`, `open_windows_update` | the row's matter is the user's own choice (refresh rate, kind of network, energy saver, output device, an old Windows or graphics driver) | `ms-settings:display-advanced`, `:network`, `:batterysaver`, `:sound`, `:windowsupdate` |

Whether a repair helped is never assumed: the PC is measured again when it
is over, and again when the user comes back to the About window (3 s after
the last measurement at the earliest), because a page of Settings gives no
sign when the user is done there. A consent prompt that is declined is no
failure; a tool that ends with an error is logged with its exit code and
told in a notification.

**Installing an update (since 1.12).** "Update now" asks the checker
afresh (what is installed is what is the newest *now*); when the answer
names a newer release, `update_installer::prepare` works on a thread of its
own:

1. the release must carry a signature file (`mirror_dock_win64.zip.sig`):
   one without is offered for download in the browser only;
2. `.update` in the program's folder is made (a folder that cannot be
   written to ends it here, before anything is fetched);
3. the signature file (at most 4096 bytes), then the package (at most
   64 MiB, its size as the release says), with the progress shown;
4. `check_release_signature`: ECDSA P-256 over the SHA-256 of the zip
   against the public key in `uxplay_release_signature.cpp`;
5. the package is unpacked with the program's own zip reader into
   `.update\new`. Every name must lie below `MirrorDock/`, have no `.` or
   `..` part, none of the characters Windows reserves, no part that ends
   with a dot or a space, and at most 200 characters;
6. the version resource of the unpacked `mirror_dock.exe` must be the
   version the release claims, and that must be newer than the running
   program (an old signed package cannot be passed off as a new one).

Then the release is *ready*. `apply()` puts it in place: every top-level
item the release has is moved from the program's folder into `.update\old`
(Windows lets a running executable be renamed) and the new one into its
place; a step that fails moves everything back. Files the release does not
have are not touched (settings are in the registry). A note,
`.update\installed`, names the version. It is applied at once when no
picture is on screen; otherwise when the stream ends
(`native_media_airplay_video_stop`) or on "Restart now". Only one caller
applies (the three can coincide), and nothing is applied while Windows ends
the session (a process ended half-way would leave files of two versions).
After it the bridge emits `NATIVE_MEDIA_EVENT_UPDATE_INSTALLED`, and the
host ends the session cycles and starts `native_media_restart_target()`:
the release's executable name in the program's folder, whatever the
running copy was called, with the same command line. The new process waits
for the single-instance mutex as any second start does. When the program
was ending anyway (the user's "Exit" ended the stream the release waited
for), it is not started again: the next start is the new program.

At its start `update_installer::create` looks for the note. With it, this
is the first start after an update: the user is told once (the note is
removed at once), and a thread deletes `.update` (it tries for 10 s: the
replaced program may still hold its own file). Without it, what was
unpacked and never applied is deleted, and files in `.update\old` stay:
they are what an apply could not put back. The next update clears the
folder before it starts.

**Signing a release.** The key pair is made once with
`win_native/build_scripts/sign_release.ps1 -KeyFile <key.pem> -NewKey`
(PowerShell 7.2 or newer): the private key is a PKCS#8 PEM file outside the
tree, readable by the maintainer's account only, and it is never put into
the repository, a package or a log; the public key is the 64 bytes (X, Y)
of `release_key`. Every release: package, then
`sign_release.ps1 -KeyFile <key.pem> -Package dist/mirror_dock_win64.zip`,
which writes `mirror_dock_win64.zip.sig` (128 hexadecimal digits: r, then
s) and checks it; both files are uploaded as assets of the release. A
release without the `.sig` cannot be installed by the program, only
downloaded. Without the private key no further release can be installed by
the programs in the field (they would need one manual download of a
release built with a new key), so the file needs a backup that is as
private as the file. A fork that publishes releases of its own needs a key
of its own and its own `UXPLAY_PROJECT_URL`.

**The lists on "Components".** `win_native/build_scripts/build_inputs.tsv`
is the one list of the inputs: `fetch_deps.sh` clones from it, and
`uxplay_generate_build_info` (in `win_native/CMakeLists.txt`) turns it into
`uxplay_build_inputs.inc`. For a row with a "check" key the version this
configuration really uses wins (pkg-config for the three libraries, the
compiler, CMake, Ninja, the fxc path, `llhttp.h`, `uxplay_version.h`); when
it differs from the recorded one, CMake warns and the recorded date and
revision are left out. The program's own versions come from the `## <version>
— <date>` headings of `CHANGELOG.md` (`uxplay_release_history.inc`); a
heading without both stops the configuration. When an input changes, edit
the list (version, revision, date) together with README_AI's table.

**Interface.** The window is drawn by the program itself, so every text is
in `uxplay_ui_text` and nothing is taken from a dialog template. Its window
procedure handles `WM_PRINTCLIENT`, so a capture tool gets its picture.

## Build (MSYS2 CLANG64)

```sh
# dependencies (pinned main-branch commits, static, portable -march=x86-64-v2)
bash win_native/build_scripts/fetch_deps.sh <src_root>
MSYSTEM=CLANG64 bash -lc 'bash win_native/build_scripts/build_deps.sh <src_root> <prefix> libplist openssl ffmpeg'
# UxPlay
MSYSTEM=CLANG64 bash -lc 'bash win_native/build_scripts/build_uxplay_native.sh <uxplay_src> <build_dir> <prefix> Release'
# tests
<build_dir>/win_native/usb_link_tests.exe
<build_dir>/win_native/ui_text_tests.exe
<build_dir>/win_native/engine_smoke_test.exe <annexb stream> h264|h265 60 [auto|intel|nvidia] [quiet|loud] [zh-TW|zh-CN|en]
# package (exe + guide + licences + complete corresponding source)
python -I win_native/build_scripts/package_zip.py <build_dir> <uxplay_src> <msys2>/clang64 <src_root> dist/mirror_dock_win64.zip
# sign it (writes dist/mirror_dock_win64.zip.sig; see "Signing a release")
pwsh -File win_native/build_scripts/sign_release.ps1 -KeyFile <key.pem> -Package dist/mirror_dock_win64.zip
```

CMake targets of the owned C++: `uxplay_base` (failure outcomes and the log:
`uxplay_status`, `uxplay_log`) ← `phone_control` ← `uxplay_native` (the
engine) ← `uxplay` (the executable) and the tests. `uxplay_base` exists so
that `phone_control` does not depend on the engine that uses it. All of them
compile with `CXX_EXTENSIONS OFF`: a consumer with another language mode
makes CMake build a second flavour of every module interface and of the std
module, and with more than one library in the chain the two flavours of
`std` collide ("Disagreement of the location of the 'std' module").

Warnings are errors on all owned code (`uxplay_strict_warnings` in the top
`CMakeLists.txt`): `win_native/` and `phone_control/` (`-Wall -Wextra
-Wpedantic -Wshadow -Wold-style-cast …`), `uxplay.cpp`, `lib/` and
`lib/mdnsd/` (`-Wall -Wextra`; `-Wsign-compare` off in `lib/` because the
upstream socket code keeps Windows `SOCKET` handles in `int`). The only
remaining build warnings come from the toolchain's own `std.cppm` /
`std.compat.cppm` (`-Wreserved-module-identifier`).

Pinned inputs (observed 2026-11-12, each the newest of its source on that
day). The list the build and the program read is
`win_native/build_scripts/build_inputs.tsv` (it also carries the dates and
licences); this table explains it:

| input | source | branch | commit / version |
|---|---|---|---|
| FFmpeg | github.com/FFmpeg/FFmpeg | master | 7b00cbd7edf64c439209fb473dd21943c49a7178 (libavcodec 63.28.100) |
| OpenSSL | github.com/openssl/openssl | master | df281a12096f3ac80272444b128135d065ac3e4d (4.2.0-dev) |
| libplist | github.com/libimobiledevice/libplist | master | 3cfe22475244cc1e924b6fd189234c130d6c5914 (2.8.1) |
| protocol core (`lib/`, `uxplay.cpp`) | github.com/FDH2/UxPlay | master | branched off at 234b407e2e7a8a9a33adf293f0e56fd539658b1e (2026-11-01); the changes up to 05b0275cb1b726592700e47a6e0307544b3d3a52 (2026-11-06) that touch files of this tree are carried over |
| llhttp (`lib/llhttp/`, as published) | github.com/nodejs/llhttp | tag `release/v9.4.3` | 0e815792b167a9bd8ace259b95b7da953776c288 (9.4.3) |
| clang / libc++ / libunwind / lld / compiler-rt | MSYS2 CLANG64 | — | 23.1.5 |
| mingw-w64 crt / headers / winpthreads | MSYS2 CLANG64 | — | 14.0.0.r598.ge3d18511f |
| CMake / Ninja | MSYS2 CLANG64 | — | 4.4.5 / 1.13.2 |
| fxc | Windows SDK | — | 10.0.28000.0 |

Not built in, but what the program installs or relies on at run time: the
Developer Disk Image and Apple's root certificate (table further down), and
Apple's USB driver package, which `install_apple_usb_support.cmd` installs
through winget (`Apple.AppleMobileDeviceSupport`: always the version winget
offers; 19.4.0.10 with driver 538.0.0.0 on 2026-11-12).

Upstream's five commits after the branch point: `5812f84` (the HTTP
server's run mutex is created and destroyed; the data socket's session
pointer is cleared), `b1be2bd` (a short first packet in `mirror_buffer`),
`7aaad62` (H.265 unit type) are carried over as upstream wrote them;
`d4b7373` (a callback of the Raspberry Pi profile options) and `05b0275` (a
GStreamer renderer) concern code this tree does not have. The run mutex
matters here too: `httpd.c` locked and unlocked it everywhere but never
created it, and winpthreads refuses to lock a mutex that was not
initialized, so the lock protected nothing until now.

Fetched at run time, not part of the build or the package (observed
2026-11-12; constants in `phone_control/src/uxplay_developer_image.cpp`):

| input | source | pinned |
|---|---|---|
| Developer Disk Image (Cryptex1, build 27B52: `Image.dmg`, `.trustcache`, `.cryptex_info`, `.root_hash`, `BuildManifest.plist`) | github.com/doronz88/DeveloperDiskImage, branch `main`, directory `PersonalizedImages/Xcode_iOS_DDI_Cryptex` | commit a1020f81b6429f5ce40408cbe21c690512b613b5, and the SHA-256 of each file |
| Apple Root CA (the only root accepted for `gs.apple.com`) | https://www.apple.com/appleca/AppleIncRootCertificate.cer | SHA-256 b0b1730ecbc7ff4505142c49f1295e6eda6bcaed7e2c68c5be91b5a11001f024, valid until 2035-02-09 |

A newer iOS may need a newer image: then the commit and the hashes are
updated together (the build identity "Developer Disk Image Cryptex" of the
new `BuildManifest.plist` is what `load_developer_image` reads).

FFmpeg options: `--disable-everything --disable-autodetect` plus decoders
h264, hevc, aac, alac, pcm_s16le, pcm_f32le; parsers h264, hevc, aac;
hwaccels h264/hevc d3d11va (+va2); swresample; w32threads.

**Linking (single executable):** everything is linked statically
(`-static`): the pinned FFmpeg/OpenSSL (libcrypto, and libssl for the TLS
towards the phone)/libplist builds, libc++, libunwind and
winpthreads (MSYS2 CLANG64 runtime, recorded exception: toolchain runtime, not
a main-branch build). `mirror_dock.exe` imports only Windows system DLLs, so the
package bundles no DLL. The UCRT (`api-ms-win-crt-*` → `ucrtbase.dll`) stays
dynamic: the MinGW/clang UCRT target cannot link it statically (MSVC `/MT`
can), but it is part of Windows 10/11, so deployment is the same single file.
Licence texts of every statically linked library ship in `licenses/`; LGPL
relinking is possible from the bundled source and build scripts.

## Source encoding

Owned source files are **UTF-8 with BOM and CRLF**: `uxplay.cpp`, `lib/*.c|h`,
`lib/mdnsd/`, everything under `win_native/` (C++ modules, headers, tests,
`uxplay.rc`, Python and PowerShell scripts) and `phone_control/`, and the
`CMakeLists.txt` files. `.gitattributes`
and `.editorconfig` keep it that way; `python -I
win_native/build_scripts/source_encoding.py check <files>` verifies it (valid
UTF-8, BOM, CRLF only) and `convert` repairs it.
Recorded exceptions, each observed with the consuming tool on 2026-11-11:

| files | form | reason |
|---|---|---|
| `*.sh` | UTF-8, LF, no BOM | MSYS2 bash: a BOM makes line 1 `﻿#!/usr/bin/env: No such file or directory`; with CRLF a backslash line continuation does not continue (the next line ran as its own command) |
| `*.hlsl` | UTF-8, CRLF, no BOM | fxc 10.0.28000: `error X3000: Illegal character in shader file` at (1,1) with a BOM; CRLF is accepted |
| `*.cmd` | ASCII, CRLF, no BOM | they run on the user's cmd.exe: this Windows 11 build skipped a BOM, but cmd.exe of other Windows 10/11 builds is reported to read it as part of the first command, and that cannot be tested here |
| `lib/llhttp/`, `lib/playfair/` | as published | vendored third-party code, not edited |
| `win_native/build_scripts/build_inputs.tsv` | UTF-8, LF, no BOM | `fetch_deps.sh` reads it line by line (a CR would end up in the last field, a BOM in the first), and CMake reads it as a list of lines |

The user guides (`win_native/package/user_guide/user_guide_*.txt`) ship with
BOM and CRLF so that every Windows editor shows them correctly. The READMEs
and CHANGELOG.md are Markdown: UTF-8 without BOM.

## Code protection (assessment)

Considered per change (what strings, cross-references and decompiled control
flow expose): the program is distributed under GPL-3.0 **with its complete
corresponding source in the same zip**, and the FairPlay/pairing code in
`lib/` is unmodified public upstream code. Virtualization or string
protection would not raise the effort of anyone who wants to read or alter
the logic, would add start-up and runtime cost on the decode/present path and
would make crash records (module+offset) useless. No VMProtect markers are
used; the pairing keys the program generates stay in the user's profile
(`.uxplay.pem`) and are never logged. The same holds for `phone_control/`:
its protocols are documented in public GPL code, the only secrets it touches
(the keys of the phone's pairing record, which Apple's driver service hands
out) are used for the TLS session and never stored or logged, and the two
pinned values in it (image hashes, Apple's root certificate) are public.
The update's signature check (1.12) was looked at the same way: what an
analyst finds is a public key and a call into Windows' cryptography, both
in the published source. Hiding them would protect nothing: whoever can
change the program on a PC needs no update to run their code there, and
whoever cannot change it is stopped by the signature, which rests on the
private key alone. That key never enters the tree, a build or a package.

## Options specific to this build

`-gpu auto|integrated|discrete|software` (aliases `intel`, `nvidia`, `warp`),
`-mode quality|balanced|performance` (default: the mode saved by the menu,
else balanced), `-crop auto|off`, `-ui auto|en|zh-TW|zh-CN|ja|ko|es|de|fr|vi|th|ru`
(interface language),
`-h264` / `-h265` (by default H.265 is offered only with HEVC hardware
decode). Display geometry/refresh are advertised from the monitor (per mode)
unless `-s`/`-fps` are given. `-ca <file>` writes cover art to a file
(on-screen cover art needs GStreamer and is not available).

## State of 1.6 (2026-11-11)

What was observed, and what was not:

* **Builds:** the whole tree (program, the three test programs, shaders,
  resource script) builds with warnings as errors on all owned code; the only
  warnings are the toolchain's own `std.cppm` ones.
* **Measured once** (engine smoke test, the same HEVC 2872x1320 60 fps
  stream, balanced mode, hardware decode on the Intel HD 530, one run of each
  version on this host; 1.6 is the released build):

  | | 1.5 | 1.6 |
  |---|---|---|
  | working set | 268.5 MB | 227.2 MB |
  | private bytes | 256.3 MB | 221.3 MB |
  | GPU shared memory of the process | 175.0 MB | 122.9 MB |
  | decoded / presented / dropped | 480 / 479 / 2 | 480 / 480 / 1 |
  | sync delay, rate | 14.7 ms, 59.9 fps | 17.8 ms, 60.0 fps |

  About 54 MB of both memory rows is the test's own copy of the stream. The
  1.6 log shows `D3D11VA surface pool: 17 surfaces of 2944x1408 (6 for the
  decoder, 11 for presentation)` where 1.5 allocated 23, and `audio output:
  stream restarted, first buffer 1.0 ms after the audio was queued`. The
  resting sync delay is 3.1 ms higher than in 1.5 on purpose: the audio
  output now reports what it really keeps queued (the 280-frame endpoint
  buffer, 5.8 ms, not the 128-frame period), and the audio buffering floor
  is that latency + 12 ms.
* **Measured once, idle** (the program itself with a test receiver name,
  waiting for a connection, 4-7 s after start): working set 43.4 MB in 1.5,
  46.2 MB in 1.6; private bytes 20.2 MB and 22.4 MB. Idle memory did not go
  down (the program grew by its new functions); what went down is the memory
  of a running stream. Both versions ended with exit code 0 within 0.04 s of
  Ctrl+C.
* **Reviewed, not run:** the host program, the window/crop/bridge/settings
  code and the engine's use of presenter, decoder and audio output were read
  for defects by three reviewers who had not written them (38 findings), and
  each reviewer then checked the fixes for its own findings. Every confirmed
  finding is fixed except the ones accepted below.
* **Accepted, not fixed:**
  * `raop_set_plist` (main loop, on a mode or display change) is not atomic
    against the `/info` handler: one connection could be negotiated with a
    mixed old/new size. The window is a few assignments wide.
  * A crop framed by hand in the very instant the phone rotates can stay on
    the new picture while the menu shows the standing mode (the presenter's
    `set_crop` does not carry the picture size it belongs to); choosing a
    crop mode again clears it.
  * Content that appears in a cut-off bar within the last 100 ms before the
    phone's screen goes still is examined only with the next picture.
  * The crash filter runs on the faulting thread: after a stack overflow
    there may be no room for the record, and a minidump written inside a
    damaged process can hang. The text record is written first.
  * A switch to quality in mid-stream gets its deeper queue with the next
    stream.
* **Not exercised at all** (no iPhone session and no interactive use of this
  build): "Wi-Fi only" enforcement over the real USB link, the window sizing
  and its memory, the crop (automatic and by hand), quality / performance
  mode on a real stream, the single-instance hand-over, start with Windows,
  the start-failure dialog, the crash record, the restart after a lost
  graphics device, the Windows-shutdown goodbye, the RTSP watchdog and the
  connection hand-over, the audio endpoint release while idle. The first
  device session should be checked against the log: the pool size line, the
  `automatic crop:` lines, `refused an AirPlay connection ... "Wi-Fi only"`,
  and in the 10 s video line `decoded min/mean ... ms after its timestamp`
  (how early pictures are ready) next to `dropped ... overflow`.

## State of 1.7 (2026-11-11)

1.7 is 1.6 under its new name (see "Names"): no change to the media path.
Observed: the whole tree builds with warnings as errors, and the packaged
executable carries the new version resource. **Not run:** the renamed
program itself, including the one-time take-over of the settings key, the
"start with Windows" entry and the log folder from the UxPlay name. The
measurements and the list of things never exercised on a device in "State of
1.6" apply unchanged.

## State of 1.8 (2026-11-11)

1.8 adds seven interface languages (ten in all), the README and the user
guide in all ten, the repository front (logo, badges, changelog), the menu on
a right click anywhere on the picture, and the fixes that the owner's first
session with 1.7 called for.

**The owner's 1.7 session** (2026-11-11, iPhone 16 Pro Max, iOS 26.6, Wi-Fi
only, read from `%LOCALAPPDATA%\MirrorDock\mirror_dock.log` and the process
counters while it ran; nothing was started or stopped for it):

* The program ran under its new name: connections, HEVC hardware decoding,
  sound, automatic crop, mode changes. Resting sync delay 17.8 ms (balanced)
  and 13.8 ms (performance).
* The settings of the earlier name were **not** taken over (`RegCopyTreeW`
  failed with code 5) - fixed, see "Names". The log folder was taken over.
* The automatic crop stopped coming back after changes of film - reported by
  the owner; cause and fix under "Crop".
* Performance mode overflowed its queue, and the first seconds of a stream
  did so in every mode - cause and fix under "Performance modes" (the
  sender's timestamps lead by about 85 ms).
* Memory with the landscape stream (3200x1536 coded, automatic crop on):
  working set 312.8 MB, private bytes 288.9 MB, GPU shared memory of the
  process 198.1 MB, 14 threads. The figures noted for 1.5 in an earlier
  device session were 271 MB, 311 MB, 243 MB and 20 threads, but window size
  and content were not the same, so this is no like-for-like comparison. What
  the log does show is that the surface pool of a real stream is no smaller
  than before (see "GPU memory"): **"lower memory while mirroring", as the
  1.6 notes put it, holds for the test stream, not for this phone.** The
  README no longer carries that comparison.

**Observed for 1.8:** the whole tree builds with warnings as errors (which
includes the compile-time checks of the text table); the translated READMEs
match the master's structure (`readme_consistency.py`, 0 differences each).

**Not run:** 1.8 itself. In particular the crop across changes of film (the
fix follows from reading the log and the detector, not from a repeat of the
owner's session), the take-over of settings by value, the changed start delay
and performance queue, the right-click menu, and the program in any of the
seven new languages - how the texts fit the control bar and the menu has not
been looked at, and the translations were written and cross-read by language
models, not by native speakers. Whether iOS offers media remote control
(DACP) while mirroring is still unknown: the log line `the client offers
media remote control` appears once per session if it does. Everything listed
as untested for 1.6 and 1.7 still applies, except what the session above
showed working.

## State of 1.9 (2026-11-12)

1.9 adds remote control of the iPhone (`phone_control/`, see "Remote control
of the iPhone"), its texts in the ten languages and its sections in the
READMEs and user guides. The sender is the same iPhone 16 Pro Max
(`iPhone17,2`), now with **iOS 27.2.1**, Developer Mode on.

**How it came about.** Control was first tried with a helper process that
used the Python library pymobiledevice3 (never published). With it the owner
used the left button, the wheel, the middle button and typing on 2026-11-12.
The owner then asked for a version without Python; `phone_control/` is that
version, written against the same wire formats, and the helper is gone from
the tree.

**Observed with the phone on the cable** (a console probe built from the same
`phone_control` sources; nothing identifying was printed):

* Lockdown session after 45 ms, tunnel after 90 ms, service directory (85
  services) after about 0.12 s.
* The input service lists five surfaces: 257 touch screen, 512 keyboard, 1026
  main screen buttons, 1280 and 1281 (gesture surfaces). SpringBoard reports
  the orientation.
* **The control session object** (`remote_control`, as the engine uses it):
  ready after 1.0 s (of which 0.5 s is the settling pause after the stream
  start); the keyboard registration is answered with the service identifier
  that was asked for; a touch report and a keyboard report are taken (the
  touch report, written to the log at debug level, is byte for byte what
  pymobiledevice3 builds for the same contact, apart from the timestamp); the
  screen stream delivers 300 to 500 datagrams a second, which the tunnel
  drops; after twelve seconds the session is still ready; ending it (stream
  stopped, everything closed) takes about 50 ms.
* **Developer Disk Image:** removed with the reference tool, the phone's
  directory shrinks to 64 services and the display, input and button services
  are gone (this is how `connect` tells that the image is missing).
  `ensure_developer_image` then installed it in 2.3 s with the image in the
  cache (reading and checking it 0.8 s, Apple's signature 0.6 s, transfer and
  installation 0.8 s); the directory, read again at once, lists 85 services.
  A part missing from the cache is fetched through WinHTTP and checked (seen
  with the trust cache file).
* **Apple's signing service** answers the request with a ticket of 2977 bytes
  over HTTPS with Apple Root CA as the only accepted root; the same check
  refuses a server whose chain ends elsewhere (`CERT_E_UNTRUSTEDROOT`, nothing
  sent).
* The whole tree builds with warnings as errors; the translated READMEs match
  the master's structure (0 differences each), the ten user guides have the
  same twelve sections.

**An incident worth knowing.** A probe run that ended with `stopAll` while
the owner's MirrorDock (then still with the Python helper) was controlling
the phone very probably cut that session's input, silently. Hence the note
about the gate under "Remote control of the iPhone": never run a second
controller, and no stream-stopping probe, while a session is in use.

**Not observed:** a click or a key arriving in an app *through the native
code* with MirrorDock itself (the session, the gate and the reports were
seen being accepted; their effect on the screen was not looked at, and the
report layouts are the ones the Python helper used). The flicks (Page
Down / Page Up, Alt + arrows). A phone held sideways or upside down with the
native code (sideways with the home indicator on the right worked with the
helper; the other way round never ran). The failure paths: no cable, no
trust, Developer Mode off and the request to show its switch, an iOS older
than 27, camera or microphone in use, the cable pulled in mid-use and the
automatic new session. The download of the 17 MB image through the program
(only the small file was fetched that way; the image came from the cache),
an installation with the phone locked, and whether the installed image
survives a restart of the phone or an iOS update (the persistence asked for
is "until the next software update"). The program in the seven languages
whose texts were written by language models, not native speakers.
Everything listed as untested for 1.6 to 1.8 still applies.

## State of 1.9.1 (2026-11-12)

**The owner's first session with 1.9** (2026-11-12 03:15 to 03:27, picture
over Wi-Fi, control over the cable; read from the log, nothing was started or
stopped for it): control was ready 1.1 s after the stream appeared, a
keyboard was registered when the first key was pressed, the phone was turned
sideways and the session followed. Three times the log says "the connection
to the phone was lost (... reset by the phone); connecting again", each time
at a moment of input after a pause, and each time a new session was ready in
under a second.

**Cause and fix.** The phone probes a connection that has been silent with
TCP keep-alive segments (no data, one byte before the next expected one) and
resets it when nobody answers. `device_tunnel`'s TCP answered only segments
that carried data or a FIN. It now answers every segment that does not begin
where the next byte is expected with an acknowledgement, as TCP asks.
Reproduced with the probe (`idle 180`: two connections that send nothing):
before the fix the phone had reset them after 180 s, with the fix they
answer. A control session held for 200 s without input stayed ready.
`remote_xpc_connection::alive()` lets the session notice a connection the
phone ended without waiting for the next report.

**Also seen:** with a still picture the screen stream delivers no datagrams
at all (0 in every 5-second window for three minutes, with the phone awake
and unlocked), so a silent stream says nothing about the gate. In one of the
probe sessions the tunnel and the SpringBoard connection ended at the same
moment after 139 s, with the cable in place; the cause is not known (the
engine starts a new session in that case).

**Not explained:** the log of the run that ended at 03:03 (an interim build)
was found as 604 797 zero bytes followed by its last three lines: something
shortened the file to nothing between that run's last statistics line and
its exit, and the run's own handle then wrote at its old position. The exit
of 1.9 at 03:27 left its log whole. Nothing in the exit path opens the log
again; a second start that takes over truncates only after waiting for the
file. Worth watching when a log is needed after an exit.

## State of 1.10 (2026-11-12)

1.10 adds Russian as the eleventh language (interface texts, README, user
guide; `-ui ru`) and brings every input to the newest version of its source:
the three dependency libraries, llhttp, the protocol core's upstream changes
and the toolchain whose runtime is linked in (see "Pinned inputs").

**Observed:** a build from scratch with clang 23.1.5 and the new libraries,
warnings as errors. The engine test with the 2872x1320 H.265 stream: 480 of
480 pictures decoded in hardware and presented at 60.0 fps, sync delay 17.8
ms. The console probe built with the same toolchain and libraries against
the phone on the cable: lockdown session (TLS), tunnel, service directory
(85 services), the input surfaces and the orientation. The program itself
started under a test name and asked to exit ended orderly (exit code 0 after
0.05 s): the HTTP server starts and stops with its mutex in place. The eleven
READMEs match the master's structure (0 differences each); the eleven user
guides have twelve sections.

**Not observed:** MirrorDock 1.10 itself with a phone - a mirroring session,
and with it the HTTP server's restart after an error now that its mutex
really locks (the paths were written for a working mutex and read again for
this release: none takes it twice, and the thread is joined either without
the lock or when it no longer needs it). The Russian texts on screen; they
and the Russian documents were written by a language model with Apple's and
Microsoft's Russian pages as the source for the terms, not by a native
speaker. Everything listed as not observed for 1.9 and 1.9.1 still applies.

## State of 1.11 (2026-11-13)

1.11 adds the About window (About, This PC, Components), the update check
and the one list of build inputs, and gives the window's own commands
priority while the phone is controlled (see "The About window, the update
check and "This PC"" above and the header of `uxplay_video_window.cppm`).

**How the interface was tested without touching the owner's desktop.** The
owner worked at this PC, mirroring to and controlling the phone with the
released 1.10, while 1.11 was written: no test window may appear on their
desktop, and no key or click may be injected there. A script outside the
tree starts a test instance (`-n MirrorDockAboutCapture -p 38400`, output
redirected to a file, so the log folder is not touched) on a second desktop
of the window station (`CreateDesktopW`, `STARTUPINFO.lpDesktop`), which is
never shown; opens the About window through the program's own menu with
posted messages (the tray callback message, then arrow keys and Enter to the
menu); clicks, scrolls and changes pages with posted messages; takes
pictures with PrintWindow; and ends the instance with "Exit". For "Copy report" it uses a window station of its own, which has a
clipboard of its own. It also puts the "start with Windows" entry back: an
instance that starts while the entry exists points it at itself (existing
behaviour), so a test instance from the build folder would leave the user's
entry pointing at the build.

**Observed (2026-11-13, this PC, the owner's iPhone 16 Pro Max on the
cable):**
- eight builds, warnings as errors, none with a diagnostic;
- the three pages in dark and in light colours, and in Traditional Chinese,
  English, Russian, German, Japanese, Thai and French: nothing overlaps or
  runs out of its card, long labels wrap;
- with the mouse: the second tab, the switch of the automatic check (off and
  on again; the checker logged both), "Check now" (the status "checking",
  then the answer);
- the update check against GitHub: 1.10.0 with its package of 5,636,637
  bytes; "up to date" as 1.11.0; "newer available" with "Download" and
  "What's new" for a build numbered 1.9.9; in an idle run the automatic
  check 30.5 s after the start;
- "This PC" for this PC: 96 of 100. The one row marked was the display (the
  monitor ran at 50 Hz; earlier that day, with the desktop duplicated on the
  built-in panel at 60 Hz and that monitor at 50 Hz, the row named both and
  gave the advice for duplicated monitors). Firewall off, Apple Mobile
  Device Support 19.4.0.10 with its service, both graphics drivers with
  their dates, the iPhone as "iPhone17,2 · iOS 27.2.1 · Developer Mode on";
- "Copy report": 30 lines of plain English;
- the full-screen commands on the engine test's window (no phone, so not in
  control mode): double-click on, Esc off, the auto-repeat and the release of
  that Esc without effect, double-click on and off again;
- every test instance ended with exit code 0; ready 0.57 s after launch on
  the second start of a binary (4.5 to 5.5 s on the first start of a newly
  built one while it is scanned, as before);
- the ten translated READMEs match the master's structure (0 differences
  each); the eleven user guides have thirteen sections, BOM and CRLF.

**Not observed:**
- control mode itself with the new priority: a double-click and Esc while F8
  is on, and Alt + double-click as a double tap. It needs a mirroring
  session with the phone; the branch differs from the tested one in
  `controlling()` only;
- the notification of a newer release found by the automatic check, and the
  click on it;
- a link opening in the browser, and the links to the user guide and to the
  licence folder (the tests ran from the build folder, which has neither;
  a clicked link would have opened a tab in the owner's browser);
- a firewall that is on (this PC's is off for its network), a PC without
  Apple's USB support, without hardware decoding, on battery, with a second
  instance of another receiver name;
- Simplified Chinese, Korean, Spanish and Vietnamese on screen.

**Translations.** All by a language model. Five translation workers were
started for the nine non-master languages and stopped half-way at the
owner's request: they had written the interface texts of ja, ko, de, vi, th
and ru, the READMEs of zh-CN, ja, ko, de, vi, th and ru, and the user guides
of ja, de and ru. The rest (interface texts and READMEs of es and fr; user
guides of zh-CN, ko, es, fr, vi and th) was written directly, with the
finished READMEs as the source of each language's wording. Every heading
underline of the guides was set to the heading's width on screen.

Everything listed as not observed for 1.9, 1.9.1 and 1.10 still applies.

## State of 1.12 (2026-11-13)

1.12 adds the screenshot, the shorter delay while the phone is controlled,
the repairs of "This PC" and the update that installs itself (see
"Invariants", "Repairs on "This PC"", "Installing an update" and "Signing a
release" above).

**How it was tested.** As for 1.11 the owner was using the PC (mirroring to
and controlling the phone with 1.11), so nothing was shown on the desktop
in use, no input was injected there, and its clipboard was left alone.
Scripts outside the tree ran the engine test and the program on a second
desktop that is never shown, or on a window station of its own (it has a
clipboard of its own). Four things about that environment cost time:

- A desktop that is not displayed paces the presenter: the engine test
  there ends with 436 of 480 pictures decoded and `sync delay 135.0 ms`
  (the delay's upper limit), and reports FAIL. A build of 2026-11-12
  behaves the same there; on a window station of its own, where nothing is
  composed at all, the same test passes with 17.8 ms. Timing can only be
  judged on the latter, or on a real screen.
- The engine test's output reaches a redirected file in blocks, so a
  script that waits for a line may see it only at the end.
- A layered window (the control bar, the notice) paints nothing itself:
  `PrintWindow` gives a black picture unless it asks the compositor
  (`PW_RENDERFULLCONTENT`).
- A program that restarts itself does not pass redirected output on; the
  new one writes its log file. A test sets `LOCALAPPDATA` for the first
  one, so that the log lands in a folder of the test's and not in the
  user's.

**Observed (2026-11-13, this PC):**
- four builds of the 1.12 code, warnings as errors, the last three without
  a diagnostic (the first stopped at the compile-time check of the texts,
  see the changelog);
- the screenshot with the engine test and a 2868x1320 HEVC stream: F9
  wrote `MirrorDock <date> <time>.png` (2868 x 1320, 669,713 bytes) into
  `Pictures\MirrorDock` and put a bitmap of the same size (24 bits a pixel)
  and the PNG on the clipboard; the picture is the test pattern, upright,
  with its colours. With the clipboard held open by the test, the file was
  written all the same, the log said why the clipboard was not
  (`OpenClipboard failed (code 0x5)`), and the notice read "Screenshot
  saved" instead of "... and copied";
- the notice (a dark pill with white text) and the control bar with its
  new camera button, pictured through the compositor;
- the zip reader and the signature check, built unchanged into a console
  probe outside the tree: the published 1.11 package unpacked to 212 files
  of 16,491,411 bytes, every file's SHA-256 equal to what Python's
  `zipfile` gives; the test signature of that package fits, the package
  with one byte changed does not, the signature with one digit changed
  does not, a file that is no signature is called that;
- the download as the installer does it (the program's HTTP client, same
  request): the 1.11 package from GitHub, 5,901,151 bytes in 1.9 s through
  the redirection, 742 progress reports, the SHA-256 of the release;
- **the update end to end, after the release** (2026-11-13 22:54): a build
  of the same sources numbered 1.11.9, in a folder unpacked from its own
  package, found 1.12.0 ("signed"); "Update now" was pressed with the
  keyboard; it fetched the 6,054,180 bytes (13 s that time; the same
  download took 2 s and 9 s in two other tries), logged the signature as
  fitting with the SHA-256 of the published package, unpacked 225 files
  into `.update\new` (7.5 s, mostly the writing of the files), put the 6
  top-level items in place, and the host ended the program and started the
  new one 2.4 s later. The new program logged "MirrorDock 1.12.0", "this
  is the first start of 1.12.0 after an update", and 1.2 s later that the
  leftovers were cleared away; `.update` was gone, and "Exit" ended it with
  code 0. The About window showed "Downloading 1.12.0..." with the bytes
  meanwhile;
- the start of the program with made-up leftovers in `.update`: a note
  naming the running version (said "first start after an update", cleared
  everything), a note naming another version (cleared, said nothing), only
  unpacked files (cleared), and files in `old` without a note (kept, with a
  warning in the log);
- the About window of 1.12.0 against the published 1.11: "up to date", the
  log names the release as "not signed"; "This PC" with the button "Open
  display settings" under the display row (the monitor runs at 50 Hz); 96
  of 100 as before. No repair button was clicked: each opens a page of
  Settings or a consent prompt on the desktop in use;
- the ten translated READMEs match the master (0 differences each), the
  eleven user guides have their thirteen sections, BOM and CRLF, and the
  interface table has 241 texts in eleven languages (checked at compile
  time).

**Not observed:**
- of the update: a release that waits for the end of a stream and is put
  in place then, "Restart now", an "Exit" while it waits, the end of the
  Windows session while it waits, the notification after an update (a test
  instance on a desktop without a shell shows none), and a step that fails
  with real files (the roll-back was read, not run);
- the shorter delay with a phone: the timeline's arithmetic was written
  from the measurements above, and `sync delay` in the log of a control
  session will show whether it settles near -56 ms as expected;
- a repair carried out (firewall rule, Apple's service), a firewall that
  is on or shut, the buttons of the other rows;
- F9 while the phone is controlled, the camera button clicked, the menu
  item;
- the new texts on screen in the languages other than Traditional Chinese
  and English.

Everything listed as not observed for 1.9 to 1.11 still applies, except
that the update check now also tells whether a release is signed.
