# UxPlay for Windows (native)

AirPlay screen mirroring and audio from an iPhone or iPad to a Windows PC —
over Wi-Fi, or over the USB charging cable.

This is a Windows-only build of [UxPlay](https://github.com/FDH2/UxPlay) with
its own media engine: no GStreamer, no Bonjour service, no runtime to
install. It ships as a single `uxplay.exe`.

* **Video:** H.265 / H.264 decoded on the GPU that drives your monitor
  (Direct3D 11 video decoding through FFmpeg, zero-copy), presented on a
  flip-model swap chain with timestamp scheduling. H.265 is offered to the
  iPhone only when that GPU decodes it in hardware — Intel, AMD and NVIDIA
  alike, with a software fallback.
* **Audio:** WASAPI low-latency shared mode, one presentation timeline for
  picture and sound (lip sync by construction), soft limiter, local mute.
* **USB cable:** with *Personal Hotspot* enabled, the iPhone's USB network
  link carries AirPlay locally; UxPlay advertises itself on that link while
  the cable is connected and moves back to Wi-Fi when it is unplugged. A
  one-click option keeps the PC's internet traffic off the phone's mobile
  data. "Prefer USB / Wi-Fi only" switches without touching the USB device,
  so the cable keeps charging; "Wi-Fi only" is enforced (no answer on the
  USB link, and a connection that still arrives over it is refused).
* **Three modes:** *Quality* (full resolution, a little more buffering, no
  frame skipped), *Balanced* (the default: adapts to monitor, GPU and
  network) and *Performance* (lighter stream, minimal buffering, lowest
  delay and memory) — in the menu or with `-mode`.
* **Player-style window:** opens at a sensible size (a portrait phone at
  about 90 % of the screen height, a landscape one at about half the screen
  width), keeps the picture's aspect ratio while you resize it and remembers
  size and position per orientation; a control bar that appears with the
  mouse and hides after 3 s; fullscreen on double-click / F / F11 /
  Alt+Enter; always on top (T); display hot-plug and refresh changes
  followed live.
* **Crops black bars:** a 16:9 film on a 19.5:9 phone is mirrored with black
  bars. *Crop black bars automatically* (key C, or `-crop auto`) cuts them
  off so the window hugs the content, and shows the whole picture again the
  moment something appears in a bar; *Select the area to show* (key X) lets
  you frame any part with the mouse.
* **Behaves like a desktop program:** one instance per receiver name, a
  notification-area menu, optional start with Windows, a dialog that names
  the reason when it cannot start, a timestamped log and a crash record in
  `%LOCALAPPDATA%\UxPlay`.
* **Your language:** the interface (control bar, tray menu, notifications)
  follows the Windows display language — Traditional Chinese, Simplified
  Chinese, or English for every other language; `-ui` picks one.
* **Fast:** advertised ~0.5 s after launch (the first start of a newly
  downloaded copy takes longer while antivirus scans it); exits in ~0.05 s.

Tested with an iPhone 16 Pro Max on iOS 26.6 (Windows 11, Intel HD 530 +
GTX 960M).

## Download and use

1. Download `uxplay_native_win64.zip` from
   [Releases](https://github.com/bestkakkoii/UxPlay-USB/releases) and unzip it.
2. Run `uxplay.exe` (allow it through Windows Firewall when asked).
3. On the iPhone: Control Center → Screen Mirroring → *UxPlay@your-PC*.

For the USB cable: install Apple's driver once
(`install_apple_usb_support.cmd`, or the *Apple Devices* app from the
Microsoft Store), connect the cable, turn on *Settings → Personal Hotspot →
Allow Others to Join*, then pick UxPlay in Screen Mirroring.

The Traditional Chinese user guide (`user_guide_zh_tw.txt`) in the zip covers
setup, the control bar, options and troubleshooting. `uxplay.exe -h` lists all
options; common ones:

| option | effect |
|---|---|
| `-n name` | name shown on the iPhone |
| `-fs` | start in full screen |
| `-mode quality\|balanced\|performance` | what to optimize for (default: the choice saved by the menu, else balanced) |
| `-crop auto\|off` | cut black bars around the content off / show the whole picture (default: the choice saved by the menu, else off) |
| `-gpu auto\|integrated\|discrete\|software` | GPU used for decoding and display |
| `-h264` / `-h265` | offer only H.264 / always offer H.265 |
| `-ui auto\|zh-TW\|zh-CN\|en` | interface language (default: the Windows display language) |
| `-vsync ms` | shift audio against video (lip sync tuning) |
| `-pin`, `-pw` | require a PIN or password |
| `-reset n` | reset after n seconds without client heartbeat |

Options read from `%USERPROFILE%\.uxplayrc` (one per line, no leading `-`)
are applied first. Options of other UxPlay builds that need GStreamer (HLS
video, MP4 recording, on-screen cover art, RTP re-streaming, flip/rotate) or
other platforms (Bluetooth LE beacon, Raspberry Pi, D-Bus) are not part of
this build; they are accepted and ignored with a warning.

Requirements: Windows 10 or 11, x64, any CPU with SSE4.2 (all Windows 10/11
x64 PCs), a Direct3D 11 GPU (or the built-in software renderer).

## Build

MSYS2 CLANG64 (clang 23, libc++ with the C++26 standard library module,
CMake 4.4, Ninja) and the Windows SDK shader compiler. Dependencies (FFmpeg,
OpenSSL, libplist) are built from pinned main-branch commits as static
libraries:

```sh
bash win_native/build_scripts/fetch_deps.sh <src_root>
MSYSTEM=CLANG64 bash -lc 'bash win_native/build_scripts/build_deps.sh <src_root> <prefix> libplist openssl ffmpeg'
MSYSTEM=CLANG64 bash -lc 'bash win_native/build_scripts/build_uxplay_native.sh <this_repo> <build_dir> <prefix> Release'
```

[README_AI.md](README_AI.md) documents the architecture, invariants, pinned
inputs, tests and verification state.

## Licence

The program is distributed under the **GNU General Public License v3**
([LICENSE](LICENSE)): it is built from UxPlay's GPL-3.0 code. The Windows-native
media engine in [`win_native/`](win_native) is original work under the **MIT
licence** ([win_native/LICENSE](win_native/LICENSE)) and may be reused on its
own under those terms. Files under `lib/` keep their upstream licences
(LGPL-2.1+, GPL-3.0, MIT — see each file). The release zip contains the
licence texts of all statically linked libraries (FFmpeg LGPL-2.1+, OpenSSL
Apache-2.0, libplist LGPL-2.1+, LLVM libc++, winpthreads, llhttp, playfair) and
the complete corresponding source.

## Credits

* [UxPlay](https://github.com/FDH2/UxPlay) by F. Duncanh and contributors —
  the AirPlay server this build is based on (`uxplay.cpp`, `lib/`).
* [RPiPlay](https://github.com/FD-/RPiPlay) by Florian Draschbacher, and
  [shairplay](https://github.com/juhovh/shairplay) by Juho Vähä-Herttua — the
  protocol core UxPlay grew from.
* playfair (FairPlay), llhttp, csrp (`lib/srp.c`), and the authors of the
  embedded mDNS responder (`lib/mdnsd`).
* FFmpeg, OpenSSL and libplist.
