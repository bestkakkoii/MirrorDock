# README_AI — UxPlay Windows-native build (authority entry)

This file is the entry point for maintainers and agents working on this
tree. Upstream UxPlay documentation (README.md / README.txt / README.html)
is unchanged and still describes the generic (GStreamer) build.

## What this tree is

UxPlay 1.74 (AirPlay mirroring / audio receiver, GPL-3.0) specialised for the
iPhone → Windows AirPlay use case. It is **runtime-adaptive to any Windows
x64 PC** (see "Adaptivity & portability" below), and tuned/verified on:

* Host: ASUS laptop, i7-6700HQ (Skylake), Intel HD Graphics 530 (drives the
  displays: MSI MP341CQ 3440x1440 over HDMI at 50 Hz, cloned with the 60.02 Hz
  built-in panel), GTX 960M (no outputs; Optimus), 16 GB, Windows 11 26200,
  Intel AC 7265 Wi-Fi.
* Sender: iPhone 16 Pro Max (A18 Pro, USB-C), Wi-Fi AirPlay and USB.

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
  baseline (SSE4.2/POPCNT — every Win10/11 x64 machine, Intel or AMD), with
  FFmpeg/OpenSSL still pick AVX2 hot paths at runtime via CPU dispatch. (Earlier
  builds used `-march=native`/`skylake`, which could fault on other CPUs.)

On Windows (`UXPLAY_WIN_NATIVE`, default ON when the host is Windows) the
GStreamer renderers are replaced by `win_native/`, a media engine written as
C++26 named modules (`import std;`, clang 23 + libc++ module support):

| module | role |
|---|---|
| `uxplay_status`, `uxplay_log`, `uxplay_clock` | explicit failure outcomes, log sink, QPC-ns timeline |
| `uxplay_system_tuning` | EcoQoS/power-throttling opt-out, MMCSS, priority, DPI |
| `uxplay_gpu_device` | D3D11 device on the GPU that physically scans out the monitor (QueryDisplayConfig LUID), DWM composition clock |
| `uxplay_video_window` | Win32 window + UI thread, borderless fullscreen toggle (double-click/F/F11/Alt+Enter, Esc), tray icon/menu, USB arrival notifications |
| `uxplay_video_overlay` | player-style control bar: layered, non-activating popup owned by the video window, Direct2D/DirectWrite (software rasterizer), Segoe Fluent/MDL2 icon glyphs; fades in on real pointer movement, out after 3 s idle (stays while hovered); link switch, mute, menu, fullscreen, alert chips |
| `uxplay_video_decoder`, `uxplay_video_frame` | FFmpeg H.264/HEVC, D3D11VA, shader-readable decoder textures (zero copy) |
| `uxplay_video_presenter` | flip-model swap chain, frame-latency waitable (max latency 1), timestamp-scheduled presentation, source arbitration |
| `uxplay_audio_decoder`, `uxplay_audio_output` | AAC-ELD/ALAC/AAC-LC/LPCM -> float32 at device rate; WASAPI IAudioClient3 low-latency period, per-sample scheduling with ≤0.3 % cubic-resampling drift correction |
| `uxplay_media_engine` | per-source pipelines and the shared presentation timeline |
| `uxplay_qt_protocol`, `uxplay_usb_mirror` | legacy QuickTime screen-capture protocol over the hidden USB configuration (libusb; iOS ≤15, `-usb`) |
| `uxplay_usb_link` | iPhone USB network link (AirPlay over the Personal Hotspot cable): attach/link detection, tray status and hints, link classification for the window label, cellular-data guard |
| `native_media_bridge.cpp`, `renderer_compat.c` | C API (`win_native/include/native_media.h`) used by `uxplay.cpp`; old renderer API mapped onto it |

## Invariants

* **Timeline:** every local time is `raop_ntp_get_local_time()` = QPC in ns
  (changed from CLOCK_REALTIME on Windows in `lib/raop_ntp.c`); DXGI and
  WASAPI QPC positions are on the same timeline.
* **Sync:** `present_time = media_time + delay(source)`; audio and video of a
  source share one media-time mapping and one delay, so lip sync holds by
  construction. The delay tracks the 95th percentile of **video** lateness
  (decode completion) plus a margin, with a fixed audio buffering floor; it is
  clamped to ≤220 ms and eases back toward target (~1/8 of the gap per 100 ms,
  recovers in ~1–2 s). Audio lateness does **not** drive the delay: a
  mid-stream audio cold-start would otherwise spike the shared delay toward the
  cap and (with the old 1 ms/250 ms decay) never recover in-session; audio
  stays in sync because it is scheduled against the same delay and the audio
  output re-anchors its own queue. Verified on device (see below).
* **Priority:** `media_source::usb` (0) beats `media_source::airplay` (1) for
  both picture and sound; AirPlay keeps decoding in the background so losing
  USB falls back without waiting for a key frame.
* **GPU:** decode + present on the display-owning adapter (Intel). Measured on
  this host (2868x1320@60, synthetic streams): Intel H.264 37 ms / HEVC 28 ms
  sync delay, 0–4 drops; GTX 960M H.264 70 ms, HEVC 122 ms with 70 drops
  (`-gpu nvidia` keeps it selectable).
* **Verified on device (2026-11-02):** iPhone 16 Pro Max, **iOS 26.6**, Wi-Fi
  AirPlay mirroring, HEVC 664×1440 decoded by Intel HD 530 (D3D11VA). Sync
  delay stabilised at ~14.7 ms over 8000+ presented frames with ~0 new drops;
  audio (AAC 44.1 kHz) stayed in sync and no longer disrupts video on start;
  lock/unlock recovers cleanly. Startup-to-advertised ~0.2 s, exit ~0.05 s.
* **Threads:** UI thread (window, tray, control bar), render thread (MMCSS "Playback"),
  one decode thread per source, WASAPI thread (MMCSS "Pro Audio"), USB worker,
  engine start thread, USB link monitor thread (`uxplay_usb_link`: IP Helper
  change callbacks only mark it dirty; it evaluates, publishes state to the
  tray and runs the elevated netsh guard, so a pending UAC prompt never blocks
  the UI), mDNS responder thread (`mdns_thread`: queries + 2 s link check). The
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
  (`netutils_wakeup_*`, used by httpd) and a lost-wakeup-free NTP stop; Ctrl+C
  to exit measured ~51 ms (upstream ~1000 ms). On console close the handler
  waits (≤4 s) for the orderly shutdown so mDNS goodbyes are sent.
* **Failures** are explicit `uxplay::outcome` values with stage, original
  code and system/library message; nothing fails silently.
* **Audio output:** UxPlay's own mute (control bar / tray) is engine-owned and
  ramped like a volume change; a soft limiter (unity below -1 dBFS, tanh knee
  above) catches decoded overs — AirPlay AAC-ELD peaks of +2.5 dBFS were
  measured, which the endpoint would otherwise hard-clip. The 10 s
  statistics line reports audio packets, empty/failed decodes, decoded peak,
  played frames, volume and mute, so a silent session shows where sound
  stops (a "no sound" report on 2026-11-03 was traced this way to a mute
  outside UxPlay: packets, peak and played frames were all normal).
* **UI stack (recorded exception to the owner's desktop-GUI default of a
  webview stack such as C++26 + saucer):** the only GUI is the video window,
  its control bar and the tray menu, built with Win32 + Direct2D/DirectWrite
  inside the existing C++26 module engine. Reasons: the video is a D3D11
  flip-model swap chain whose lowest-latency path (independent flip in
  fullscreen) must stay free when the controls are hidden — a separate
  layered popup does that, a web view over the video would need permanent
  composition or DirectComposition interop with a browser surface; a
  WebView2 runtime would add a multi-process browser to a ~10 MB player that
  is ready in ~0.5 s; the controls are seven buttons. No Qt anywhere (the
  `qt_` files are the QuickTime capture protocol).

## USB mirroring (status: not viable on iOS 26 via QuickTime)

Two USB paths exist; **neither is the shipped default** (`usb_mirroring` is off,
opt-in `-usb`):

1. **QuickTime hidden-config capture** (`uxplay_qt_protocol`,
   `uxplay_usb_mirror`, libusb). The hidden config is enabled with vendor
   request 0x52 (wIndex 2); its bulk endpoints carry the AV stream; HPD1/HPA1
   are byte-identical to macOS QuickTime (unit-tested). **Apple disabled this
   from iOS 16 onward.** Verified against the target device (iPhone 16 Pro Max,
   iOS 26.6): enabling the config, usbmux VERSION/SETUP, lockdown QueryType +
   StartSession, and the **TLS 1.2 session upgrade using the host pair record**
   (`C:\ProgramData\Apple\Lockdown\<udid>.plist`) all succeed, yet the device
   sends **zero** bytes on the capture endpoint — even with the vendor "kick"
   (0x40/0x6400) + host ping and a separate concurrently-serviced usbmux
   session. Standalone validation probes: `D:\TMP\probe\usbmux_probe2.c`.
   Conclusion: dead on iOS 26; kept as best-effort for older devices only. The
   retry loop no longer runs by default (it was cycling the phone's config).
   Its tray toggle is shown only when started with `-usb`; libusb's own
   logging is off (it repeated every failed open on each rescan) and the
   module reports a failure once with its libusb code. It no longer advises a
   Zadig/libusb driver swap: that replaces Apple's driver and breaks path 2.

2. **AirPlay over the USB network link** (the working USB path; automatic,
   no flag; `-usb` does not control it). With the iPhone on Apple's driver
   (not libusb0) and Personal Hotspot on, Windows exposes an "Apple Mobile
   Device Ethernet" NIC (172.20.10.0/28; iPhone at .1, ping <1 ms). The mirror
   is local USB traffic. `install_apple_usb_support.cmd` winget-installs the
   Apple driver.
   * **Advertising follows the cable live** (`lib/mdnsd/mdnsd.c`:
     `mdns_pick_ipv4`, `mdns_follow_link`, every 2 s, time-based): a
     preferred 172.20.10.x address wins (USB first), else the LAN adapter with
     a gateway and the lowest interface metric (Ethernet/Wi-Fi only; Hyper-V,
     WSL, VM adapters and tunnels such as Tailscale are skipped). On a switch
     the responder sends a goodbye on the old link, moves its IPv4 multicast
     membership (`IP_DROP/ADD_MEMBERSHIP`) and `IP_MULTICAST_IF`, announces on
     the new link and repeats once after 1 s (RFC 6762 §8.3). Before this the
     membership stayed on the startup link, so after plugging in (or
     unplugging) the phone's queries on the new link went unanswered. An open
     AirPlay session pins the advertisement (`dnssd_set_session_active`, from
     `conn_init`/`conn_destroy`) until it ends, unless its link vanished. The
     membership is re-asserted on every check: Windows answers a duplicate join
     with WSAEINVAL (probed), so a successful re-join means the OS had dropped
     it (link flap, resume) and triggers a re-announce.
   * **Link preference** (tray radio items "USB first" / "Wi-Fi only",
     persisted as `HKCU\Software\UxPlay\prefer_usb_link`, DWORD, default 1).
     Leaving USB mode must not touch the USB device: on the owner's setup
     (iPhone charged through an externally powered cable) disabling the
     "Apple Mobile Device Ethernet" adapter to get back to Wi-Fi stopped the
     charging until the power was replugged. "Wi-Fi only" therefore only
     changes UxPlay's own choice: `dnssd_set_usb_link_allowed(0)` makes the
     responder skip the USB link (applied within 250 ms, overriding the
     session pin), and `native_link_preference_idle` in `uxplay.cpp` ends a
     session that runs on the other link through the existing reset path
     (`reset_httpd`), because the iPhone does not reconnect by itself. The
     choice is applied before the first advertisement at startup.
   * **Disabled adapter:** `uxplay_usb_link` reports an iPhone whose Net-class
     function has `CM_PROB_DISABLED` (`link_adapter_disabled`); in "USB first"
     the tray offers to re-enable it (choosing "USB first" offers it at once):
     one elevated `pnputil /enable-device "<instance id>"`, the ID checked
     against `[A-Za-z0-9&_\.-]`. pnputil enables exactly that function.
   * **Window label** comes from the adapter that owns the connection's local
     address (`raop` `conn_transport(local_address)` →
     `native_media_set_airplay_local_address` → `is_usb_link_address`):
     172.20.10.x on a non-802.11 adapter is "USB"; a PC joined to the
     hotspot over Wi-Fi is labelled Wi-Fi.
   * **`uxplay_usb_link`** watches the iPhone (CM device list,
     `USB\VID_05AC&PID_12*`) and its link (`NotifyIpInterfaceChange`,
     `NotifyUnicastIpAddressChange`, `NotifyRouteChange2`, 5 s poll) and drives
     the tray: a USB status line, a "turn on Personal Hotspot" balloon when the
     iPhone is attached for 8 s without a link, and "USB connected".
   * **Cellular-data guard.** The iPhone gives the PC global IPv6 (observed
     `2001:b400:…/64`, carrier space) while this home Wi-Fi has **no IPv6**:
     with only the IPv4 metric raised, every IPv6-capable connection of the PC
     (Google, YouTube, Microsoft, CDNs) still went out through the phone's
     mobile data. The monitor checks `GetBestRoute2` toward public IPv4/IPv6
     addresses; when internet leaves through the iPhone link while another
     network has a gateway, the tray item and a clickable balloon offer the
     guard (the click gives the foreground, so UAC opens directly): one
     elevated `cmd /c netsh …` (system32 paths, numeric arguments only) sets,
     persistently, IPv4 `metric=9999 ignoredefaultroutes=enabled` and IPv6
     `metric=9999 routerdiscovery=disabled ignoredefaultroutes=enabled` on that
     interface, then re-checks the routes. The link keeps carrying the mirror
     (on-link 172.20.10.0/28) but never internet, even if Wi-Fi drops; a
     deliberate tether (no other network) is never guarded. To tether over USB
     again: `netsh interface ipv4 set interface <idx> ignoredefaultroutes=disabled`,
     `netsh interface ipv6 set interface <idx> routerdiscovery=enabled ignoredefaultroutes=disabled`,
     `Set-NetIPInterface -InterfaceIndex <idx> -AutomaticMetric Enabled`.
   * **Verified end-to-end (2026-11-02, before the live-follow and guard
     work):** with Personal Hotspot on, UxPlay advertised
     `UxPlay.local → 172.20.10.9` and the iPhone mirrored over the cable —
     `Local 172.20.10.9 / Remote 172.20.10.1`, H.265 hardware decode, 0 drops,
     sync delay 14.7 ms; the IPv4 default route stayed on Wi-Fi.
     **Not yet verified on the device** (the iPhone was not attached while this
     was built): plugging in while UxPlay runs (membership move), the hotspot
     hint and the guard's UAC flow. Covered so far by `usb_link_tests`, the
     duplicate-join probe and a live Wi-Fi run (advertised in 0.6 s, resolved
     via `dns-sd`).

The target device was moved off the Zadig `libusb0` binding back to Apple's
`appleusb.inf` on 2026-11-02 (normal state; iTunes/Finder work again).

## Build (MSYS2 CLANG64)

```sh
# dependencies (pinned main-branch commits, static, portable -march=x86-64-v2)
MSYSTEM=CLANG64 bash -lc 'bash win_native/build_scripts/build_deps.sh <src_root> <prefix> libusb libplist openssl ffmpeg'
# UxPlay
MSYSTEM=CLANG64 bash -lc 'bash win_native/build_scripts/build_uxplay_native.sh <uxplay_src> <build_dir> <prefix> Release'
# tests
<build_dir>/win_native/qt_protocol_tests.exe win_native/tests/qt_fixtures
<build_dir>/win_native/usb_link_tests.exe
<build_dir>/win_native/engine_smoke_test.exe <annexb stream> h264|h265 60 [auto|intel|nvidia]
```

Pinned inputs (observed 2026-11-02):

| input | source | branch | commit / version |
|---|---|---|---|
| FFmpeg | github.com/FFmpeg/FFmpeg | master | a88838af482100fa22dd22c94be592c35642de46 (libavcodec 63) |
| OpenSSL | github.com/openssl/openssl | master | aad00a888ccb47ec49afeb7545f2dddf9397fbb5 (4.2.0-dev) |
| libplist | github.com/libimobiledevice/libplist | master | fe3dc34dc3484006e12b403f7ceb06f0ad40b6f4 |
| libusb | github.com/libusb/libusb | master | 987c1170d0018ba82557823a98ae443e8335e11b |
| clang / libc++ / lld | MSYS2 CLANG64 | — | 23.1.4 |
| CMake / Ninja | MSYS2 CLANG64 | — | 4.4.5 / 1.13.2 |
| fxc | Windows SDK | — | 10.0.28000.0 |

FFmpeg options: `--disable-everything` plus decoders h264, hevc, aac, alac,
pcm_s16le, pcm_f32le; parsers h264, hevc, aac; hwaccels h264/hevc d3d11va
(+va2); swresample; w32threads.

**Linking (single executable):** everything is linked statically
(`-static`): the pinned FFmpeg/OpenSSL/libplist/libusb builds, libc++,
libunwind, winpthreads, and GLib with libintl, libiconv and pcre2
(`GLIB_STATIC_COMPILATION`; glib-2.0.pc does not carry it). `uxplay.exe`
imports only Windows system DLLs, so the package bundles no DLL. The UCRT
(`api-ms-win-crt-*` → `ucrtbase.dll`) stays dynamic: the MinGW/clang UCRT
target cannot link it statically (MSVC `/MT` can), but it is part of
Windows 10/11, so deployment is the same single file. Licence texts of every
statically linked library ship in `licenses/`; LGPL relinking is possible
from the bundled source and build scripts. Verified: the exe runs alone in an
empty folder with only System32 on PATH.

**Exception (recorded):** GLib 2.90.1 and its dependencies (libintl
gettext-runtime 1.0, libiconv 1.19, pcre2 10.49, winpthreads 14.0.0) are
MSYS2 binary packages (static archives), not main-branch builds. GLib is only
used for uxplay.cpp's main loop; rebuilding it from main (meson + its
dependency chain) was not done in this delivery. Replacing that loop with a
native one would remove GLib entirely.

## Removed in this build (not available without GStreamer)

`-hls`, `-mp4`, on-screen `-ca`, `-vrtp`, `-artp`, `-vs`/`-as` sink choice:
the options are accepted and reported as ignored at startup.

## New options

`-usb` (off by default; legacy QuickTime capture, does not stream on iOS 16+),
`-gpu auto|integrated|discrete|software` (aliases `intel`, `nvidia`, `warp`),
`-h264` / `-h265` (by default H.265 is offered only with HEVC hardware decode).
Display geometry/refresh are advertised from the monitor unless `-s`/`-fps`
are given.
