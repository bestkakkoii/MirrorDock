# README_AI — UxPlay Windows-native build (authority entry)

This file is the entry point for maintainers and agents working on this
tree. `README.md` is the user-facing overview; this file records the
architecture, invariants, build inputs and verification state.

## What this tree is

UxPlay 1.74 (AirPlay mirroring / audio receiver) reduced to a single
Windows-native program for the iPhone → Windows AirPlay use case. The tree
builds **only** this program (MSYS2 CLANG64; the top-level CMakeLists stops
on other hosts). It is **runtime-adaptive to any Windows 10/11 x64 PC** (see
"Adaptivity & portability"), and tuned/verified on:

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
  used (fallback 10 ms IAudioClient); endpoint changes are followed live.
* **CPU:** binary and dependencies are built for the portable `x86-64-v2`
  baseline (SSE4.2/POPCNT — every Win10/11 x64 machine, Intel or AMD);
  FFmpeg/OpenSSL still pick AVX2 hot paths at runtime via CPU dispatch.

## Program structure

* `uxplay.cpp` parses options (`~/.uxplayrc` first; `~` = `HOME`, else
  `USERPROFILE`), starts the engine (`native_media_start`), the mDNS responder
  and the RAOP server, then runs **session cycles**: `main_event_loop::run`
  until a reset/teardown (relaunch) or an exit request, then tears the session
  down and starts the next cycle.
* **Main event loop** (`main_event_loop` in `uxplay.cpp`; GLib is not used):
  tasks posted from other threads (engine events: window closed, tray exit,
  link preference; Ctrl+C) run on the main thread; client feedback is checked
  every second (`-reset n`); RAOP threads request a reset with
  `request_reset()`, which ends the run after a 100 ms grace so the requesting
  handler (e.g. TEARDOWN) finishes before main() removes connections — the same
  ordering the old 100 ms GLib poll gave.
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
| `uxplay_video_window` | Win32 window + UI thread, borderless fullscreen toggle (double-click/F/F11/Alt+Enter, Esc), tray icon/menu |
| `uxplay_video_overlay` | player-style control bar: layered, non-activating popup owned by the video window, Direct2D/DirectWrite (software rasterizer), Segoe Fluent/MDL2 icon glyphs; fades in on real pointer movement, out after 3 s idle (stays while hovered); link switch, mute, menu, fullscreen, alert chips |
| `uxplay_video_decoder`, `uxplay_video_frame` | FFmpeg H.264/HEVC, D3D11VA, shader-readable decoder textures (zero copy) |
| `uxplay_video_presenter` | flip-model swap chain, frame-latency waitable (max latency 1), timestamp-scheduled presentation |
| `uxplay_audio_decoder`, `uxplay_audio_output` | AAC-ELD/ALAC/AAC-LC/LPCM -> float32 at device rate; WASAPI IAudioClient3 low-latency period, per-sample scheduling with ≤0.3 % cubic-resampling drift correction, mute, soft limiter |
| `uxplay_media_engine` | the AirPlay pipeline and the shared presentation timeline |
| `uxplay_media_types` | media source identity (AirPlay) |
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
* **System side effects:** UxPlay changes the PC only through two opt-in,
  UAC-confirmed actions on the iPhone's network adapter — the cellular-data
  guard (netsh) and re-enabling a disabled adapter (pnputil); see the USB
  section for exact values and how to undo. It also stores one per-user
  setting (`HKCU\Software\UxPlay\prefer_usb_link`). A declined prompt or a
  failed tool is logged with its code and reported in a balloon; the tray
  item stays available to retry. Test instances launched from an elevated
  shell run these tools without a prompt: do not exercise them on the owner's
  machine without asking.
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
  `NotifyUnicastIpAddressChange`, `NotifyRouteChange2`, 5 s poll) and drives
  the tray: a USB status line, a "turn on Personal Hotspot" balloon when the
  iPhone is attached for 8 s without a link, and "USB connected".
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

## Options specific to this build

`-gpu auto|integrated|discrete|software` (aliases `intel`, `nvidia`, `warp`),
`-ui auto|zh-TW|zh-CN|en` (interface language),
`-h264` / `-h265` (by default H.265 is offered only with HEVC hardware
decode). Display geometry/refresh are advertised from the monitor unless
`-s`/`-fps` are given. `-ca <file>` writes cover art to a file (on-screen
cover art needs GStreamer and is not available).
