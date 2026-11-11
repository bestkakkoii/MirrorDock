# README_AI — UxPlay Windows-native build (authority entry)

This file is the entry point for maintainers and agents working on this
tree. `README.md` is the user-facing overview; this file records the
architecture, invariants, build inputs and verification state.

## What this tree is

UxPlay 1.74 (AirPlay mirroring / audio receiver) reduced to a single
Windows-native program for the iPhone → Windows AirPlay use case. The tree
builds **only** this program (MSYS2 CLANG64; the top-level CMakeLists stops
on other hosts). The program has its own version (`win_native/include/
uxplay_version.h`, currently 1.6.0: the one place it is written — log,
"About" and the version resource read it); `UXPLAY_CORE_VERSION` there is the
upstream UxPlay release the protocol core derives from. It is
**runtime-adaptive to any Windows 10/11 x64 PC** (see "Adaptivity &
portability"), and tuned/verified on:

* Host: ASUS laptop, i7-6700HQ (Skylake), Intel HD Graphics 530 (drives the
  displays: MSI MP341CQ 3440x1440 over HDMI at 50 Hz, cloned with the 60.02 Hz
  built-in panel), GTX 960M (no outputs; Optimus), 16 GB, Windows 11 26200,
  Intel AC 7265 Wi-Fi.
* Sender: iPhone 16 Pro Max (A18 Pro, USB-C, iOS 26.6), Wi-Fi AirPlay and
  AirPlay over the USB cable (Personal Hotspot link).

## Repository layout and licences

| path | content | licence |
|---|---|---|
| `uxplay.cpp` | the program: options, AirPlay (RAOP) callbacks, main event loop | GPL-3.0-or-later (upstream UxPlay) |
| `lib/` | AirPlay protocol core (RAOP/RTSP, pairing, FairPlay, NTP, RTP, mirror stream) | upstream: LGPL-2.1+ / GPL-3.0 (`crypto.c`, `playfair/`), MIT (`llhttp/`, `srp.c`) — see each file |
| `lib/mdnsd/` | embedded mDNS responder (replaces Bonjour/Avahi), USB-link following | per file header |
| `win_native/` | C++26 media engine, C API, build scripts, resources, tests | **MIT** (`win_native/LICENSE`, `SPDX-License-Identifier: MIT` in every file) |
| `LICENSE` | GPL-3.0 text | — |

The program as a whole is GPL-3.0 (it links GPL-3.0 code). `win_native/`
alone may be reused under MIT. `package_zip.py` ships every applicable
licence text in `licenses/`.

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
  as `uxplay.previous.log`; a start that fails before it became the instance
  writes `uxplay.start-failure.log` instead of touching `uxplay.log`.
* **Failures the user must see:** every start-up error goes through
  `startup_message()` (logged as an error and kept as the reason);
  `startup_exit()` / `cleanup(non-zero)` show it in a dialog that names the log
  file when there is no terminal (`native_media_show_start_failure`). This
  covers option errors, an RTSP server that cannot listen (`raop_start_httpd`
  results are checked at start, on relaunch and by the watchdog) and an engine
  that could not start (`NATIVE_MEDIA_EVENT_START_FAILED`).
* **Log:** started from a terminal or with redirected output the log goes
  there; otherwise to `%LOCALAPPDATA%\UxPlay\uxplay.log`. Every line carries
  the local time and passes one lock; lines written before the single-instance
  check are held in memory, the previous run is kept as `uxplay.previous.log`,
  and the file starts over at 8 MB (`LOG_FILE_LIMIT`).
* **Crash record** (`uxplay_crash_report`): an unhandled exception or
  `std::terminate` writes `uxplay_crash.txt` (exception code, faulting address
  as module+offset, thread) and `uxplay_crash.dmp` (dbghelp `MiniDumpWriteDump`,
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
  most 2 s).
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
| `uxplay_video_window` | Win32 window + UI thread: placement per orientation, aspect-locked resizing, borderless fullscreen toggle (double-click/F/F11/Alt+Enter, Esc), always on top (T), dark title bar, keep-awake while a stream plays, tray icon/menu (modes, link, mute, start with Windows, log, about), second-launch and session-end messages |
| `uxplay_settings` | per-user settings in `HKCU\Software\UxPlay` (read/write with explicit outcomes) and the "start with Windows" Run entry |
| `uxplay_crash_report` | unhandled-exception / terminate record and minidump next to the log |
| `uxplay_video_overlay` | player-style control bar: layered, non-activating popup owned by the video window, Direct2D/DirectWrite (software rasterizer), Segoe Fluent/MDL2 icon glyphs; fades in on real pointer movement, out after 3 s idle (stays while hovered); link switch, mute, menu, fullscreen, alert chips |
| `uxplay_video_decoder`, `uxplay_video_frame` | FFmpeg H.264/HEVC, D3D11VA, shader-readable decoder textures (zero copy); surface pool sized from the stream (below); falls back to the software decoder by itself and replays the units since the last parameter sets; `luma_reader` (CPU view of a picture's luma for the crop detector) |
| `uxplay_video_presenter` | flip-model swap chain, frame-latency waitable (max latency 1), timestamp-scheduled presentation on a high-resolution waitable timer, per-mode drop policy, crop region (`set_crop`), frees every per-stream GPU object and the swap chain when nothing is shown, reports a lost device |
| `uxplay_crop_detector` | pure logic: black-bar measurement of a luma plane and the cautious policy that turns it into a crop region (below) |
| `uxplay_audio_decoder`, `uxplay_audio_output` | AAC-ELD/ALAC/AAC-LC/LPCM -> float32 at device rate; WASAPI IAudioClient3 low-latency period, per-sample scheduling with ≤0.3 % cubic-resampling drift correction, mute, soft limiter; the stream runs only while a source has audio queued (stopped after 3 s of silence, endpoint released 2 s later when no source is in use), endpoint failures are retried with back-off |
| `uxplay_media_engine` | the AirPlay pipeline and the shared presentation timeline |
| `uxplay_media_types` | media source identity (AirPlay), `performance_mode` |
| `uxplay_ui_text` | interface text table (Traditional Chinese, Simplified Chinese, English; completeness checked at compile time), Windows display-language detection, DirectWrite locale |
| `uxplay_usb_link` | iPhone USB network link (AirPlay over the Personal Hotspot cable): attach/link detection, tray status and hints, link classification for the window label, cellular-data guard |
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
  | presenter queue / delay limit | 13 pictures / 220 ms | 8 / 135 ms | 5 / 85 ms |
  | stream size asked from the phone | monitor | monitor (1920x1080 box without a hardware decoder) | 1920x1080 box |

  The delay limit is the queue expressed in frame intervals of a 60 fps
  stream: a larger delay (after a network stall) would only make the
  presenter evict pictures unseen, i.e. freeze the picture while sound goes
  on. For the same reason no picture waits longer than that limit from the
  moment it is decoded (`present = min(pts + delay, ready + limit)`), also
  when pictures are ready earlier than their timestamps say. A stream starts at `min(limit, max(resting, min(50 ms, resting + 28
  ms)))`, close enough to its resting delay that easing down stays within
  what the audio output follows without a skip (0.3 % correction, hard resync
  at 30 ms). A switch to quality in mid-stream gets its deeper queue with the
  next stream (the surface pool is sized when a stream starts).

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
    takes 8 s. Content inside a bar is acted on at the next examination (every
    100 ms while a crop is in force): when the picture still has a pair of
    smaller bars the crop is loosened to them, otherwise the whole picture is
    shown; either way the next attempt to crop or tighten is delayed (3 s,
    doubling to 30 s, reset after a crop lasted a minute). The shown region stays 2 px inside the measured
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
  (queue limit + 3: 8 / 11 / 16 for performance / balanced / quality), where
  it used to be a fixed 23 surfaces (170 MB at 3130x1440). libavcodec's
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
  `HKCU\Software\UxPlay` (`prefer_usb_link`, `performance_mode`,
  `always_on_top`, the window placement values, two "hint shown" flags) and,
  only when the user ticks "Start with Windows", the value `UxPlay` under
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
  `GetUserPreferredUILanguages` — zh-TW/HK/MO and `zh-Hant` → Traditional,
  other `zh` → Simplified, anything else → English (detection failure is
  logged and falls back to English). All user-visible strings come from
  `ui_string(ui_text)`; the control bar measures label widths, and its
  DirectWrite locale (zh-TW / zh-CN / en-US) selects the matching CJK font.
  Log messages and `-h` stay English.
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
python -I win_native/build_scripts/package_zip.py <build_dir> <uxplay_src> <msys2>/clang64 <src_root> dist/uxplay_native_win64.zip
```

Warnings are errors on all owned code: `win_native/` (`-Wall -Wextra
-Wpedantic -Wshadow -Wold-style-cast …`), `uxplay.cpp`, `lib/` and
`lib/mdnsd/` (`-Wall -Wextra`; `-Wsign-compare` off in `lib/` because the
upstream socket code keeps Windows `SOCKET` handles in `int`). The only
remaining build warnings come from the toolchain's own `std.cppm` /
`std.compat.cppm` (`-Wreserved-module-identifier`).

Pinned inputs (observed 2026-11-02):

| input | source | branch | commit / version |
|---|---|---|---|
| FFmpeg | github.com/FFmpeg/FFmpeg | master | a88838af482100fa22dd22c94be592c35642de46 (libavcodec 63) |
| OpenSSL | github.com/openssl/openssl | master | aad00a888ccb47ec49afeb7545f2dddf9397fbb5 (4.2.0-dev) |
| libplist | github.com/libimobiledevice/libplist | master | fe3dc34dc3484006e12b403f7ceb06f0ad40b6f4 (2.8.0) |
| clang / libc++ / lld | MSYS2 CLANG64 | — | 23.1.4 |
| CMake / Ninja | MSYS2 CLANG64 | — | 4.4.5 / 1.13.2 |
| fxc | Windows SDK | — | 10.0.28000.0 |

FFmpeg options: `--disable-everything --disable-autodetect` plus decoders
h264, hevc, aac, alac, pcm_s16le, pcm_f32le; parsers h264, hevc, aac;
hwaccels h264/hevc d3d11va (+va2); swresample; w32threads.

**Linking (single executable):** everything is linked statically
(`-static`): the pinned FFmpeg/OpenSSL/libplist builds, libc++, libunwind and
winpthreads (MSYS2 CLANG64 runtime, recorded exception: toolchain runtime, not
a main-branch build). `uxplay.exe` imports only Windows system DLLs, so the
package bundles no DLL. The UCRT (`api-ms-win-crt-*` → `ucrtbase.dll`) stays
dynamic: the MinGW/clang UCRT target cannot link it statically (MSVC `/MT`
can), but it is part of Windows 10/11, so deployment is the same single file.
Licence texts of every statically linked library ship in `licenses/`; LGPL
relinking is possible from the bundled source and build scripts.

## Source encoding

Owned source files are **UTF-8 with BOM and CRLF**: `uxplay.cpp`, `lib/*.c|h`,
`lib/mdnsd/`, everything under `win_native/` (C++ modules, headers, tests,
`uxplay.rc`, Python scripts) and the three `CMakeLists.txt`. `.gitattributes`
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

`win_native/package/user_guide_zh_tw.txt` ships with BOM and CRLF so that
every Windows editor shows it correctly.

## Code protection (assessment)

Considered per change (what strings, cross-references and decompiled control
flow expose): the program is distributed under GPL-3.0 **with its complete
corresponding source in the same zip**, and the FairPlay/pairing code in
`lib/` is unmodified public upstream code. Virtualization or string
protection would not raise the effort of anyone who wants to read or alter
the logic, would add start-up and runtime cost on the decode/present path and
would make crash records (module+offset) useless. No VMProtect markers are
used; the pairing keys the program generates stay in the user's profile
(`.uxplay.pem`) and are never logged.

## Options specific to this build

`-gpu auto|integrated|discrete|software` (aliases `intel`, `nvidia`, `warp`),
`-mode quality|balanced|performance` (default: the mode saved by the menu,
else balanced), `-ui auto|zh-TW|zh-CN|en` (interface language),
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
