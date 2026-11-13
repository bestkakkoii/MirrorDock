<div align="center">

<img src="docs/images/mirror_dock_logo.svg" width="120" alt="MirrorDock">

# MirrorDock

**Mirror your iPhone or iPad to a Windows PC — over Wi-Fi or the USB cable — and operate the iPhone with mouse and keyboard.**

[![Release](https://img.shields.io/github/v/release/bestkakkoii/MirrorDock?label=release&color=126BDB)](https://github.com/bestkakkoii/MirrorDock/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/bestkakkoii/MirrorDock/total?color=33B8FF)](https://github.com/bestkakkoii/MirrorDock/releases)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011%20x64-0078D4)](https://github.com/bestkakkoii/MirrorDock/releases/latest)
[![C++26](https://img.shields.io/badge/C%2B%2B-26%20modules-00599C)](README_AI.md)
[![Licence](https://img.shields.io/badge/licence-GPL--3.0-blue)](LICENSE)
[![Stars](https://img.shields.io/github/stars/bestkakkoii/MirrorDock?style=social)](https://github.com/bestkakkoii/MirrorDock/stargazers)

**English** · [繁體中文](README.zh-TW.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Español](README.es.md) · [Deutsch](README.de.md) · [Français](README.fr.md) · [Tiếng Việt](README.vi.md) · [ไทย](README.th.md) · [Русский](README.ru.md)

</div>

---

MirrorDock is an AirPlay receiver for Windows. Pick it in *Screen Mirroring* on the iPhone and the phone's picture and sound appear on the PC, in a window that behaves like a media player. It is a single `mirror_dock.exe`: no installer, no GStreamer, no Bonjour service, no runtime to set up.

It is built on the AirPlay protocol core of [UxPlay](https://github.com/FDH2/UxPlay) with a media engine written for Windows (Direct3D 11, FFmpeg, WASAPI). Up to version 1.6 it was published as *UxPlay for Windows (native)*.

## ✨ Highlights

| | |
|---|---|
| 📶 **Wi-Fi mirroring** | Phone and PC on the same network, nothing to install on either side. |
| 🔌 **USB cable mirroring** | With *Personal Hotspot* on, AirPlay runs over the charging cable: local, stable, and it does not use mobile data. |
| 🎬 **Sharp, smooth video** | H.265 / H.264 decoded on the GPU that drives your monitor (Intel, AMD or NVIDIA), shown without copying, paced to the display. Software decoding steps in when the GPU cannot. |
| 🔊 **Sound in sync** | Low-latency WASAPI output on the same timeline as the picture, so lips and voice match. |
| 🎛️ **Three modes** | *Quality*, *Balanced* (default) and *Performance*: you choose between the sharpest picture and the lowest delay. |
| ✂️ **Crop black bars** | Cuts the black bars around a film off automatically, or lets you frame any part of the picture with the mouse. |
| 🖱️ **Control the iPhone** | Click, drag, scroll and type on the picture, and the phone does it; while you do, the picture no longer waits for its timestamps and follows your hand sooner. Built in: no helper program, no jailbreak (iOS 27, USB cable, Developer Mode). |
| 🪟 **A proper window** | Opens at a sensible size, keeps the aspect ratio while you resize, remembers size and position, full screen with one key, always on top. |
| 📸 **Screenshots** | **F9** saves the mirrored picture at the phone's full resolution as a PNG file and copies it to the clipboard. |
| 🌐 **Eleven languages** | The interface follows your Windows display language. |
| 🩺 **Knows your PC** | The About window checks what matters for mirroring (graphics card, drivers, network, firewall, Apple's USB support), rates it, says what to fix, and fixes what it can at the press of a button. |
| 🔄 **Updates at one click** | Tells you when a newer release is out and installs it when you say so. Every release is signed, and checked before it is installed. |
| 🧳 **One file** | A single executable of about 12 MB that needs only Windows itself. |

## 🚀 Quick start

1. Download `mirror_dock_win64.zip` from [Releases](https://github.com/bestkakkoii/MirrorDock/releases/latest) and unzip it anywhere.
2. Run `mirror_dock.exe`. If Windows Firewall asks, choose **Allow** (a home network is a *private* network).
3. On the iPhone: **Control Center → Screen Mirroring → MirrorDock@your-PC**.

The program lives in the notification area (next to the clock). The video window appears when the phone connects; closing it ends that session, and **Exit** in the menu ends the program.

> [!TIP]
> Requirements: Windows 10 or 11 (x64) and a Direct3D 11 GPU. Without one the built-in software renderer is used.

## 🔌 Mirroring over the USB cable

1. Install Apple's USB driver once: run `install_apple_usb_support.cmd` from the zip, or install **Apple Devices** from the Microsoft Store.
2. On the iPhone turn on **Settings → Personal Hotspot → Allow Others to Join**, then plug in the cable.
3. Pick MirrorDock in *Screen Mirroring*. The control bar shows **USB** or **Wi-Fi**, whichever is in use.

- **Prefer USB / Wi-Fi only** switches the link without touching the USB device, so the phone keeps charging. *Wi-Fi only* is enforced: a connection that still arrives over the cable is refused.
- **Your mobile data is safe.** Mirroring itself is local. If Windows starts routing the PC's own internet traffic through the phone, MirrorDock notices and offers a one-click fix (**Save mobile data**).

## 🎛️ Modes

| Mode | What it does | Good for |
|---|---|---|
| **Quality** | Full resolution, a little more buffering, no frame is skipped. | Films, slides, text |
| **Balanced** *(default)* | Adapts to the monitor, the GPU and the network. | Everyday use |
| **Performance** | A lighter stream (up to 1920×1080), minimal buffering, always the newest frame. | Games, older PCs |

Choose it in the menu or with `-mode`. Timing changes at once; the stream resolution changes from the next connection.

## ✂️ Cropping black bars

A 16:9 film on a phone with a wider screen is mirrored with black bars, because the phone mirrors its whole screen.

- **Automatic** (key **C**): the bars are cut off and the window hugs the picture. The crop stays when one film follows another; when something else shows up in a bar and stays, the player's controls for example, the whole picture is back within about a second.
- **By hand** (key **X**): drag a frame around the part you want to see. **Esc** cancels.

## 🖱️ Controlling the iPhone

Press **F8** (or choose **Control the iPhone with mouse and keyboard** in the menu) and the picture becomes the phone's screen: what you do on it, the iPhone does.

| On the PC | On the iPhone |
|---|---|
| Left button: click, hold, drag | A finger: tap, long press, swipe |
| Mouse wheel (with **Shift**: sideways) | Scrolls what is under the pointer |
| Middle button | Home |
| Double-click | Full screen on / off, as always; with **Alt** held: a double tap on the iPhone |
| Keyboard | Types on the phone like a keyboard plugged into it; **Ctrl** is ⌘, **Ctrl+Space** switches the phone's input language |
| **Page Down** / **Page Up**, **Alt** + arrow keys | Flick to the next or previous page or video |
| Right button | MirrorDock's menu, as always |

What it takes:

1. **iOS 27 or later** and the **USB cable**, with this PC trusted. Only the control goes through the cable; the picture may keep coming over Wi-Fi.
2. **Developer Mode** on the iPhone: *Settings → Privacy & Security → Developer Mode*. The switch is hidden until a developer tool has asked for it, which MirrorDock does the first time you press F8.
3. An internet connection the first time, and the first time after each iOS update: MirrorDock installs Apple's *Developer Disk Image* on the phone. That takes about a minute.

> [!NOTE]
> AirPlay cannot carry input, so control goes another way: through the developer services of iOS, the ones Xcode uses to operate a real device. To install them, MirrorDock downloads Apple's image from the [DeveloperDiskImage](https://github.com/doronz88/DeveloperDiskImage) repository and asks Apple's signing service (`gs.apple.com`) to sign it for your phone, which tells Apple the phone's chip numbers, exactly as Xcode does. After that, control contacts nobody.

The window's own commands come first while control is on: **F8**, **F9**, **F11**, **Alt+Enter**, **Alt+F4**, **Esc** in full screen and the double-click still belong to the window. Of a double-click, the first click has already reached the phone as a tap when the second one arrives, so double-click a black bar when there is one, or press **F11**. The control bar stays away, because it would cover the bottom of the phone's screen.

**Less delay while you control.** An iPhone stamps its pictures and its sound a little later than the moment they arrive (the pictures by about 85 ms over Wi-Fi), and normally MirrorDock waits for that time. While control is on it plays both as early as they arrive in time, still in step with each other, so what a touch does shows up sooner. The change glides in and out over a few seconds, without a jump in the sound.

## ⌨️ Keyboard and mouse

| Action | Keys |
|---|---|
| Full screen on / off | **F**, **F11**, **Alt+Enter** or double-click; **Esc** leaves |
| Crop black bars automatically | **C** |
| Frame the area to show | **X** |
| Always on top | **T** |
| Mute (the PC side only) | **M** |
| Screenshot (PNG file and clipboard) | **F9** |
| Control the iPhone on / off | **F8** |
| Control bar | Move the mouse over the picture |
| Menu | Right-click anywhere on the picture |

While the iPhone is being controlled, the letter keys are typed on the phone; **F8** gives them back to the window. Screenshots are saved in the folder *MirrorDock* inside your *Pictures* folder, named by date and time, at the size the phone sends and not at the size of the window.

## 🔄 Updates

**Check for updates…** in the menu asks GitHub for the newest release and shows the answer in the About window. When there is a newer one, **Update now** fetches it, checks it and puts it in place of the running program, which then starts again by itself; while a mirroring session runs, that restart waits for its end, or for **Restart now**. **Download** still opens the zip in your browser if you would rather unpack it yourself, and **What's new** opens the release notes. Nothing is installed unless you ask for it.

- **Automatic check.** Once a day, with a notification when a newer release is out. One switch in the About window turns it off.
- **What is sent.** One request to `api.github.com`. GitHub sees your IP address and the client name `MirrorDock/<version>`, and nothing about you or your PC.
- **Not on your mobile data.** No automatic check is made over a connection Windows marks as metered, nor while the PC's internet runs through the iPhone.
- **Only genuine releases.** Every release is signed with a key that is kept off the internet, and MirrorDock carries its public half. A package whose signature does not fit is not installed, wherever it came from.

## 🩺 About and This PC

**About…** in the menu opens a window of its own, with three pages:

| Page | What it shows |
|---|---|
| **About** | Version and release date, the update check, links to the project, the user guide and the licence texts |
| **This PC** | What matters to MirrorDock on your PC, row by row: Windows, display, power, network, firewall, processor, memory, graphics card and its video decoding, sound, graphics driver, Apple's USB support, the iPhone on the cable |
| **Components** | Every library and tool MirrorDock is built from, with version, date and licence, and when each version of MirrorDock was published |

Every row of *This PC* is rated good, attention or problem. A row that is not good says what to do about it, and has a button when MirrorDock can do it for you: let the program through Windows Firewall, start Apple's service, install Apple's USB support, or open the page of Windows' Settings where the matter is settled. What needs administrator rights is done by Windows' own tools, behind the usual consent prompt. The score is the share of the rated rows that are in order, weighted by how much each matters to mirroring (hardware video decoding counts most). It says how well the PC is set up; it is not a benchmark. **Copy report** puts the page on the clipboard as plain text, for a bug report. Nothing on this page leaves your PC, and no serial number or device name is shown or stored.

## ⚙️ Options

Add options to a shortcut of `mirror_dock.exe`, or write them one per line (without the leading `-`) into `%USERPROFILE%\.uxplayrc`. `mirror_dock.exe -h` lists them all.

| Option | Effect |
|---|---|
| `-n name` | Name shown on the iPhone |
| `-fs` | Start in full screen |
| `-mode quality\|balanced\|performance` | What to optimize for |
| `-crop auto\|off` | Cut black bars off / show the whole picture |
| `-gpu auto\|integrated\|discrete\|software` | GPU used for decoding and display |
| `-h264` / `-h265` | Offer only H.264 / always offer H.265 |
| `-ui language` | Interface language (see below) |
| `-vsync ms` | Shift the sound against the picture |
| `-pin`, `-pw` | Require a PIN or a password |

## 🌐 Languages

The interface follows the Windows display language: English, 繁體中文, 简体中文, 日本語, 한국어, Español, Deutsch, Français, Tiếng Việt, ไทย and Русский. Any other language gets English. Force one with `-ui en|zh-TW|zh-CN|ja|ko|es|de|fr|vi|th|ru`.

User guides in all eleven languages are in the zip (folder `user_guide`).

## 🧩 How it works

```mermaid
flowchart LR
    phone["📱 iPhone / iPad"] -- "AirPlay over Wi-Fi<br/>or the USB network link" --> core["Protocol core<br/>RTSP · pairing · timing"]
    core --> video["Video<br/>FFmpeg + Direct3D 11 decoding"]
    core --> audio["Audio<br/>AAC / ALAC decoding"]
    video --> clock["One presentation timeline"]
    audio --> clock
    clock --> screen["🖥️ Window<br/>flip-model swap chain"]
    clock --> speaker["🔊 WASAPI<br/>low-latency output"]
```

Picture and sound are scheduled on one timeline, with a delay that adapts to the network and to how fast the PC decodes: as short as possible without stutter.

Control is a second, separate path. Over the USB cable MirrorDock opens the phone's developer tunnel (with a small IPv6 and TCP implementation of its own, so no network adapter and no administrator rights are needed) and sends touch and keyboard reports to the phone's input service.

## 📊 Measured

One 2016 laptop (Intel Core i7-6700HQ, Intel HD Graphics 530), a 2872×1320 60 fps H.265 test stream, *Balanced* mode, version 1.6:

| | |
|---|---|
| Frames of the test stream shown | 480 of 480 |
| Audio / video delay at rest | about 18 ms |
| Memory while idle | 46 MB |
| Exit | 0.04 s |

Mirroring over Wi-Fi and over USB was verified with an iPhone 16 Pro Max (iOS 26) up to version 1.5, and version 1.7 has run with that phone over Wi-Fi: its 3200×1536 stream took about 310 MB of memory and 200 MB of GPU memory. [CHANGELOG.md](CHANGELOG.md) says for each release what was tested with a phone and what was only compiled.

## ❓ FAQ

**The iPhone does not list my PC.**
Both must be on the same network, and Windows Firewall must allow `mirror_dock.exe`. The page *This PC* of the About window says whether the firewall or a network set to *public* is in the way, and its button lets MirrorDock through the firewall. Turning the iPhone's Wi-Fi off and on again refreshes the list.

**The cable is plugged in but mirroring still uses Wi-Fi.**
Personal Hotspot must be on, and the link choice must be *Prefer USB*. The second line of the menu shows the state of the USB link.

**There is no sound.**
Check the speaker button in the control bar, then the Windows volume mixer.

**Lips and voice do not match.**
Fine-tune with `-vsync` followed by a number of milliseconds (positive values delay the sound).

**F8 says the iPhone cannot be controlled.**
The message names the reason: the cable is not plugged in or this PC is not trusted, Developer Mode is off, the iPhone runs an iOS older than 27, or the Developer Disk Image could not be installed (that step needs the internet). After switching Developer Mode on, the iPhone restarts and asks once more for confirmation.

**Is Developer Mode safe to leave on?**
It is Apple's own switch for people who build apps. It lets a trusted computer install and debug apps and reach the developer services; it does not jailbreak the phone, and you can switch it off again at the same place.

**Does MirrorDock use the internet?**
Mirroring is local. MirrorDock goes online for two things only: updates (the check on GitHub once a day, which you can switch off, and the download of a release when you ask for it), and, the first time you control an iPhone, Apple's Developer Disk Image and Apple's signature for it.

**Where is the log?**
`%LOCALAPPDATA%\MirrorDock\mirror_dock.log`, or **Open log file** in the menu.

## 🛠️ Build from source

MSYS2 CLANG64 (clang 23, libc++ with the C++26 standard library module, CMake 4.4, Ninja) and the Windows SDK shader compiler. FFmpeg, OpenSSL and libplist are built from pinned commits as static libraries; the pins are in `win_native/build_scripts/build_inputs.tsv`, the list the About window shows. A build of your own that is to update itself from releases of your own needs a signing key of your own: see `win_native/build_scripts/sign_release.ps1`.

```sh
bash win_native/build_scripts/fetch_deps.sh <src_root>
MSYSTEM=CLANG64 bash -lc 'bash win_native/build_scripts/build_deps.sh <src_root> <prefix> libplist openssl ffmpeg'
MSYSTEM=CLANG64 bash -lc 'bash win_native/build_scripts/build_uxplay_native.sh <this_repo> <build_dir> <prefix> Release'
```

[README_AI.md](README_AI.md) documents the architecture, the invariants, the pinned inputs and what has been verified. [CHANGELOG.md](CHANGELOG.md) lists the releases.

## 📜 Licence

MirrorDock is distributed under the **GNU General Public License v3** ([LICENSE](LICENSE)), because it is built on UxPlay's GPL-3.0 code. The Windows media engine in [`win_native/`](win_native) is original work under the **MIT licence** ([win_native/LICENSE](win_native/LICENSE)); the remote control in [`phone_control/`](phone_control) is original work under **GPL-3.0-or-later**. The release zip contains the licence texts of all bundled libraries and the complete corresponding source.

## 🙏 Credits

- [UxPlay](https://github.com/FDH2/UxPlay) by F. Duncanh and contributors — the AirPlay server MirrorDock is built on.
- [RPiPlay](https://github.com/FD-/RPiPlay) by Florian Draschbacher and [shairplay](https://github.com/juhovh/shairplay) by Juho Vähä-Herttua — where that protocol core comes from.
- [pymobiledevice3](https://github.com/doronz88/pymobiledevice3) by doronz88 and contributors — where the knowledge about the developer services of iOS comes from that remote control is written on.
- playfair, llhttp, csrp, FFmpeg, OpenSSL and libplist.

AirPlay, iPhone and iPad are trademarks of Apple Inc. MirrorDock is an independent open-source project and is not affiliated with Apple.
