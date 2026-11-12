# Need for Speed Underground 2 — R36S & PortMaster ARM64 Port

[![Platform](https://img.shields.io/badge/Platform-R36S%20%7C%20ArkOS%20%7C%20PortMaster-blue.svg)](docs/R36S_PORT.md)
[![Architecture](https://img.shields.io/badge/Arch-Linux%20aarch64%20(RK3326)-orange.svg)](docs/R36S_PORT.md)
[![Graphics](https://img.shields.io/badge/Graphics-OpenGL%20ES%202.0%20(Mali--G31)-green.svg)](docs/R36S_PORT.md)
[![CI/CD](https://github.com/manuelpllull/nfsu2-recompiled-r36s/actions/workflows/ci.yml/badge.svg)](https://github.com/manuelpllull/nfsu2-recompiled-r36s/actions/workflows/ci.yml)
[![Releases](https://img.shields.io/github/v/release/manuelpllull/nfsu2-recompiled-r36s?include_prereleases&label=Latest%20Release)](https://github.com/manuelpllull/nfsu2-recompiled-r36s/releases)

A native, high-performance static recompilation of **Need for Speed: Underground 2** for the **R36S** and compatible RK3326 Linux handheld consoles running ArkOS, AmberELEC, or ROCKNIX via **PortMaster**.

> [!TIP]
> **Quick Start**:
> 1. Download the latest [`NFSU2-R36S.zip`](https://github.com/manuelpllull/nfsu2-recompiled-r36s/releases) release package.
> 2. Extract into `roms/ports/` on your SD card.
> 3. Copy your PC game files into `roms/ports/nfsu2/gamedata/` (must contain `GLOBAL/`, `CARS/`, `TRACKS/`, etc.).
> 4. Launch **Need for Speed Underground 2** from the **Ports** menu!
> 
> Detailed architecture, controller maps, and benchmark reports: [**R36S Port Guide**](docs/R36S_PORT.md).

### 🎮 R36S Port Highlights
* **Native 640×480 Display**: Pixel-perfect 4:3 native rendering for the R36S IPS panel without black bars or stretched HUD.
* **Custom D3D9 → OpenGL ES 2.0 Translation Layer**: Optimized for the ARM Mali-G31 GPU with MojoShader bytecode translation.
* **Multithreaded Render Thread**: Decoupled guest engine thread and dedicated render thread with redundant state filtering.
* **Cortex-A35 Math Optimizations**: Native replacements for `ftol2`, matrix transforms, and distance culling for smooth framerates on in-order RK3326 cores.
* **PortMaster Gamepad Integration**: Preconfigured GPTK controls with analog steering, D-Pad navigation, and shortcut hotkeys.

---

## Upstream Project (Android & Windows)

### Android downloads / Descargas Android

[Underground 2 v0.14.6](https://github.com/codepdbh/nfsu2-recompiled/releases/tag/v0.14.6)

| APK | Architecture |
|---|---|
| [ARM64 / 64 bits](https://github.com/codepdbh/nfsu2-recompiled/releases/download/v0.14.6/NFSU2-AndroidEvolved-v0.14.6-arm64.apk) | arm64-v8a, recommended |
| [ARMv7 / 32 bits](https://github.com/codepdbh/nfsu2-recompiled/releases/download/v0.14.6/NFSU2-AndroidEvolved-v0.14.6-armeabi-v7a.apk) | armeabi-v7a; requires Android with 32-bit app support |

Android 10+ and Vulkan 1.1+. Both APKs use the same package and signing key;
install the architecture supported by your device. Original game data is required.
Audio, pacing and validation details: [runtime follow-up](docs/AUDIO_FRAME_PACING.md).
Issue-driven changes and remaining limits: [16 October review](docs/ISSUE_REVIEW_2026_10_16.md).

<div align="center">

<img src="docs/images/logo.png" alt="NFSU2 Android Evolved Logo" width="220" />

### **Static Recompilation of SPEED2.EXE into Native C (x86 & ARM64)**

[![Platform](https://img.shields.io/badge/Platform-Windows%20x86%20%7C%20Android%20ARM64-blue.svg)](#status)
[![Graphics](https://img.shields.io/badge/Graphics-Direct3D%209%20%7C%20Vulkan%20(DXVK)-orange.svg)](#graphics--vulkan-runtime)
[![Audio](https://img.shields.io/badge/Audio-AAudio%20%7C%20DirectSound-green.svg)](#audio--multimedia)
[![Status](https://img.shields.io/badge/Status-Playable%20Race%20Prototype-brightgreen.svg)](#status)
[![License](https://img.shields.io/badge/License-MIT%20(Tooling)-lightgrey.svg)](#legal)

*A full static binary recompilation of **Need for Speed: Underground 2** (`SPEED2.EXE`, x86-32) into clean, portable C code using an enhanced fork of [pcrecomp](https://github.com/sp00nznet/pcrecomp).*

[🎮 Android Showcase](#gameplay-showcase) • [⚡ Features](#features) • [📊 Status](#status) • [🛠️ Build Pipeline](#pipeline) • [📱 Android Guide](#android-port) • [⚖️ Legal](#legal)

---

</div>

## Gameplay Showcase

> Real-time screenshots captured running natively on **Android (ARM64)** with DXVK Native over Vulkan 1.3:

<div align="center">

| **In-Game Night Race (Overtake & HUD)** | **Car Customization & Tuning Garage** |
|:---:|:---:|
| <img src="docs/images/android_race_action.png" alt="Android Race Action" width="460"/> | <img src="docs/images/android_garage_tuning.png" alt="Android Garage Tuning" width="460"/> |
| *Real-time race against AI with active touch HUD & analog gauges* | *Full 3D vinyl, rim and chassis rendering in the garage* |

| **Driving Perspective & Telemetry** | **Customizable Touch Controls & Styles** |
|:---:|:---:|
| <img src="docs/images/android_race_gameplay.png" alt="Android Race Gameplay" width="460"/> | <img src="docs/images/android_controls_styles.png" alt="Android Controls Customization" width="460"/> |
| *Smooth 35–51+ FPS gameplay on Samsung Galaxy S24 Ultra (SM-S938B)* | *Live layout editor, button scaling, gyroscope tilt & layout presets* |

| **Interactive Button Layout Editor** | **Native Game Launcher & Settings** |
|:---:|:---:|
| <img src="docs/images/android_controls_editor.png" alt="Controls Editor" width="460"/> | <img src="docs/images/android_launcher.png" alt="Android Launcher" width="460"/> |
| *Drag, resize and save personalized control profiles* | *Automatic internal storage detection, language & FPS limits* |

</div>

---

## Features

- **⚡ Native Binary Recompilation**: Translates 27,700+ x86 functions into standard C code. No JIT compiler, no virtualization layer and no dynamic translation overhead at runtime.
- **📱 Android ARM64 Runtime**: Native execution on modern mobile devices with portable virtual memory management, structured exception handling and POSIX thread synchronization.
- **🌋 Vulkan Graphics Acceleration**: Full Direct3D 9 to Vulkan translation powered by an embedded port of DXVK Native.
- **🎮 Fully Customizable Touch Controls**:
  - Drag-and-drop on-screen controls editor with button scaling and persistent named layouts.
  - Preset control themes: *Clásico*, *Xbox*, and *PlayStation*.
  - Gyroscope / Tilt-based accelerometer steering for an intuitive driving experience.
  - Dedicated driving buttons: Steering, Handbrake, Nitrous (N₂O), Brake/Reverse, and Throttle.
- **🖥️ In-Game Resolution & FPS Scaler**:
  - Dynamic render resolution scaling (25%, 50%, 75%, Native Display, 720p).
  - Configurable frame rate limiters (Uncapped, 30 FPS, 60 FPS, 120 FPS) to balance thermals and battery endurance.
- **🔊 Low-Latency Audio**: Native PCM streaming using Android's AAudio backend with automatic buffer underrun protection.
- **🌐 Multilingual Support**: Detects and loads your game's language files (Spanish, English, French, German, Italian, etc.) directly from phone storage.

---

## Architecture Overview

```
                          ┌────────────────────────┐
                          │   SPEED2.EXE (x86)     │
                          └───────────┬────────────┘
                                      │
                         [ pcrecomp disasm32 ]
                                      │
                         [ lift32 (+ simd32) ]
                                      │
                          ┌───────────▼────────────┐
                          │  27,742 C Functions    │
                          │   (~8.9M lines C99)    │
                          └─────┬────────────┬─────┘
                                │            │
           ┌────────────────────┘            └────────────────────┐
           ▼                                                      ▼
 ┌──────────────────────┐                               ┌──────────────────────┐
 │     Windows Host     │                               │    Android ARM64     │
 │  MSVC x86 Toolchain  │                               │   Android NDK / C++  │
 │  Direct3D 9 / Win32  │                               │   DXVK Native/Vulkan │
 └──────────────────────┘                               └──────────────────────┘
```

> [!IMPORTANT]
> **This repository contains no copyrighted game code or assets.** You must own a legitimate copy of Need for Speed Underground 2 and provide your own game files. All generated code and analysis artifacts are produced locally on your machine.

---

## Status

Current state as of **October 2026**:

| Milestone / Component | Platform | Status | Details |
|---|---|:---:|---|
| **M0 C Generation** | Any | **DONE** | 27,742 functions, 8.9M lines of C, 0 lift failures |
| **M1 Compiles / M2 Links** | Windows | **DONE** | MSVC x86, 0 errors (~4.5 min on 16 threads) |
| **M3 CRT / M4 Entry / M5 WinMain** | Windows | **DONE** | Game startup runs fully recompiled (registry, timers, threads) |
| **M6 Window / M7 D3D9 / M8 First Frame** | Windows | **DONE** | Device initialization, window management and rendering |
| **M9 Intros / M10 Menus** | Windows | **WORKING** | FMVs play, menus render, full keyboard and controller input |
| **Car Rendering & Visuals** | Win / Android | **WORKING** | Fixed 16-bit signed flag bug; cars, vinyls and headlights render accurately |
| **Race Loading & Gameplay** | Win / Android | **PLAYABLE** | Resort Loop and Quick Races load; acceleration, steering, nitrous, AI traffic active |
| **ARM64 Android Port** | Android | **PLAYABLE** | 35–51+ FPS at 1170×540 on Snapdragon 8 Gen 3; touch controls and audio active |
| **Surface Recovery & Pause** | Android | **VERIFIED** | Clean background/resume without losing process or graphics state |
| **Frame Rate Limiter & Scaler** | Android | **DONE** | 30/60/120 FPS frame caps and dynamic resolution modes implemented |

---

## Tested Game Build

The port can be built from any of these unpacked (No-CD) `SPEED2.EXE` builds: the original release (v1.0) and the final patch (v1.2), which is the same code plus the patch changes, relinked; `v12b` is a second v1.2 exe whose code is laid out slightly differently from `v12`. Functions sit at different offsets, so each build is lifted from its own exe, and the hand-written native code, hooks and overrides take that exe's addresses automatically (`config/exe_variants.txt`, `config/exe_functions.txt`).

| Property | v10 | v12 | v12b |
|---|---|---|---|
| **What it is** | v1.0 (`Oct 17 2004`), the exe this port was first lifted from (region unconfirmed) | v1.2 (`Feb 9 2005`), European retail with SecuROM removed | v1.2 (`Feb 9 2005`), another unpacked exe of the same build stamp (region unconfirmed); build it yourself, the release has no binary for it |
| **Exact File Size** | `4,788,224` bytes | `4,800,512` bytes | `4,788,224` bytes (same as v10) |
| **SHA-256** | `04b83e2d13c91c6ec294b578a08240ebcfff6fa9513df584edaa968e4ab9990c` | `f9dd86c054878ce6276beb07c1fd61874f7a1e4bf1f241b084c65b73e24168a7` | `08577b93d4b954c74679b6387792a9aca6c9434513fbc4eaeffdb343650bb899` |
| **MD5** | `95b4dc3e9a6c72b06a8bd62b0c56ea63` | `6445adbf4e8d64be51e96a69bba168a4` | `6f236382dfee70bfcfcfe9d4fbbd0507` |
| **PE Entry Point** | `0x0075B9F1` | `0x0075BCC7` | `0x0075BCC7` |
| **Binary Format** | Unpacked image (`.text` clean code, rebuilt imports, relocations preserved) | same | same |

> [!IMPORTANT]
> A build runs only with the exe it was lifted from: `scripts/run_lift.py` recognises the exe by its hash, and the launcher and runtime refuse a different one (an exe of the other build would run its data into the wrong code). Other builds (another v1.0 exe, v1.1) are not supported unless they match one of these hashes.

*For deep binary details, see [docs/NFSU2_BINARY_REPORT.md](docs/NFSU2_BINARY_REPORT.md).*

---

## Requirements

For the **R36S binary** you only need Python and Docker on Windows, macOS or Linux: see
[R36S Handheld & PortMaster Port](#r36s-handheld--portmaster-port) and [BUILD.md](BUILD.md). The
requirements below are for the native Windows x86 host build.

### Windows Host
- Windows 10 / 11 64-bit
- Visual Studio 2022 / 2026 with C++ x86 desktop development toolset (MSVC)
- LLVM / Clang (for differential testing)
- The lifting programs of [Lifting the Functions](#lifting-the-functions-windows-macos-linux) (Python 3.8+,
  git, capstone, pefile); `unicorn` and `keystone-engine` only for pcrecomp's differential tests

### Android Build
- Android SDK Platform 35 / Min SDK 26 (Android 8.0+)
- Android NDK (r25c or newer with Clang ARM64)
- Vulkan 1.3 capable Android device (Vulkan 1.1 fallback in development)

---

## Lifting the Functions (Windows, macOS, Linux)

Lifting turns the x86 code of your `SPEED2.EXE` into C (`src/recomp/gen/`, about 27,700 functions and
9 million lines). It is plain Python and runs natively on **Windows, macOS and Linux** with one command,
without Docker or a compiler:

```bash
python build.py "<your game folder or SPEED2.EXE>" --lift-only                     # R36S / Android x87 data included
python build.py "<your game folder or SPEED2.EXE>" --lift-only --no-x87-window     # for the Windows x86 host build
```

(`python3` on macOS and Linux.) It recognises the exe (v1.0 or v1.2, `config/exe_variants.txt`), fetches the
pcrecomp lifter at commit `35548b8` and applies `patches/pcrecomp/`, then runs the analysis copy, the
vtable/RTTI seeds, the function catalog (5–15 min), the lift (~3 min) and, unless `--no-x87-window`, the
local x87 window data the R36S and Android builds use. Results are cached in the work folder
(`%LOCALAPPDATA%\nfsu2-r36s` on Windows, `~/.cache/nfsu2-r36s` elsewhere; `--work` changes it), so a second
run with the same exe and lifter is instant. Output: `src/recomp/gen/` (`--gen` changes it).

### Programs needed for lifting

| | Windows 10/11 | macOS 12+ | Linux |
|---|---|---|---|
| **Python 3.8+** | [python.org](https://www.python.org/downloads/) (tick *Add python.exe to PATH*) or `winget install Python.Python.3.12` | `brew install python` or `xcode-select --install` | preinstalled; Debian/Ubuntu: `sudo apt install python3 python3-pip` |
| **git** | `winget install Git.Git` ([git-scm.com](https://git-scm.com/download/win)) | `xcode-select --install` or `brew install git` | `sudo apt install git` / `sudo dnf install git` / `sudo pacman -S git` |
| **capstone 5, pefile** | `py -m pip install "capstone>=5.0,<6" pefile` | `python3 -m pip install "capstone>=5.0,<6" pefile` | `python3 -m pip install --user "capstone>=5.0,<6" pefile` (add `--break-system-packages` on Debian 12 / Ubuntu 24.04, or use a venv) |
| **Disk / RAM** | 3 GB / 4 GB | 3 GB / 4 GB | 3 GB / 4 GB |

`python build.py --setup-lifter` only fetches and patches the lifter (a quick check that git works).
Compiling the lifted code for the R36S needs the ARM64 toolchain: `build.py` without `--lift-only` does the
lift and the compile in Docker on all three systems ([R36S Handheld & PortMaster Port](#r36s-handheld--portmaster-port)).

### The steps by hand

The same steps without `build.py`, from the repository folder (bash; in PowerShell use `$env:GAME` and
backticks instead of `\`):

```bash
GAME="/path/to/Need for Speed Underground 2"
git clone https://github.com/sp00nznet/pcrecomp.git ../pcrecomp
git -C ../pcrecomp checkout 35548b8
git -C ../pcrecomp am --3way "$PWD"/patches/pcrecomp/*.patch
cp ports/r36s/sdk/include/recomp32/*.h ports/r36s/sdk/include/recomp32/*.c ../pcrecomp/runtime/recomp32/

python scripts/exe_variant.py detect "$GAME/SPEED2.EXE"                       # v10, v12 or v12b
python scripts/normalize_exe.py "$GAME/SPEED2.EXE" work/SPEED2.analysis.exe   # analysis copy (relocations restored)
python ../pcrecomp/tools/cpp/vtable_scan.py work/SPEED2.analysis.exe --seeds work/vtable_seeds.json
python ../pcrecomp/tools/cpp/rtti.py        work/SPEED2.analysis.exe --seeds work/rtti_seeds.json
python -c "import json;a=json.load(open('work/vtable_seeds.json'));b=json.load(open('work/rtti_seeds.json'));json.dump(a+b,open('work/seeds_all.json','w'))"
python scripts/run_lift.py catalog work/SPEED2.analysis.exe work/seeds_all.json work/catalog.json
python scripts/run_lift.py lift    work/SPEED2.analysis.exe work/catalog.json src/recomp/gen
# R36S / Android only: local x87 window data
python ports/r36s/lw/annotate.py strip src/recomp/gen
python ports/r36s/lw/analyze.py src/recomp/gen work/nfs_lw_info.json
python ports/r36s/lw/annotate.py apply src/recomp/gen work/nfs_lw_info.json
python ports/r36s/lw/annotate.py header src/recomp/gen work/nfs_lw_info.json src/recomp/gen/nfs_lw.h
```

`run_lift.py` uses the exe's column of `config/hooks.txt`, `overrides.txt` and `exclude.txt` and records the
exe in the generated code; the hand-written native code follows it through `src/runtime/nfs_exe_addr.h`.

### Windows x86 host build

After `python build.py "<game>" --lift-only --no-x87-window`, in Git Bash with Visual Studio 2022 (C++ x86
desktop tools):

```bash
source scripts/vsenv.sh x86
scripts/build.sh
scripts/run.sh [--fullscreen]
```

---

## Android Port

The `android/` directory contains the complete Gradle project for the Android APK:

1. **Building the APK**:
   ```bash
   cd android
   ./gradlew assembleDebug
   ```
2. **Device Deployment**:
   - Install the generated APK on your Android device.
   - Copy your PC game files (`Need for Speed Underground 2`) to the device's internal storage inside a folder named `/sdcard/nfsu2/`.
   - Open **NFSU2 Android Evolved**, grant file access, choose your preferred language, resolution, and FPS cap, and launch the game!

See additional documentation:
- [Android Port Status & Build Notes](docs/ANDROID_PORT.md)
- [DXVK Native Android Port](ports/dxvk_android/README.md)
- [Gameplay & Controls Validation](docs/ANDROID_GAMEPLAY_CONTROLS_TEST.md)
- [Menu & Save Profile Testing](docs/ANDROID_MENU_PROFILE_TEST.md)

---

## R36S Handheld & PortMaster Port

The `ports/r36s/` directory contains the build and packaging for the **R36S** and compatible ARM64 handhelds
(Rockchip RK3326) running ArkOS, dArkOS, AmberELEC or ROCKNIX through PortMaster.

Every [release](https://github.com/manuelpllull/nfsu2-recompiled-r36s/releases) contains ready-to-run
binaries for both supported exes, `nfsu2.aarch64.v10` (v1.0) and `nfsu2.aarch64.v12` (v1.2); the launcher
picks the one for the player's `SPEED2.EXE`:

1. Unzip the release `NFSU2-R36S-<version>.zip` into the SD card's `ports/` folder (`NFSU2.sh` + `nfsu2/`).
2. Copy your game files, with an unpacked v1.0 or v1.2 `SPEED2.EXE`, into `ports/nfsu2/gamedata/`.
3. Launch **Need for Speed Underground 2** from the Ports menu.

To build the binary yourself, use **`build.py`**, one command on Windows, macOS or Linux; the full guide
is **[BUILD.md](BUILD.md)**. Maintainers produce the release binaries with it too (BUILD.md, section 8).

### What you need to build it

| | Windows 10/11 | macOS 12+ (Apple silicon or Intel) | Linux |
|---|---|---|---|
| **Container** | [Docker Desktop](https://www.docker.com/products/docker-desktop/) (WSL 2 backend) or `winget install Docker.DockerDesktop` | [Docker Desktop](https://www.docker.com/products/docker-desktop/) or `brew install --cask docker`, 8 GB memory in *Settings > Resources* | [Docker Engine](https://docs.docker.com/engine/install/) (`sudo apt install docker.io`), user in the `docker` group |
| **Python 3.8+** | [python.org](https://www.python.org/downloads/) or `winget install Python.Python.3.12` | `xcode-select --install` or `brew install python` | preinstalled (`python3`) |
| **Without Docker** | inside WSL 2 Ubuntu, as Linux | – | Ubuntu 22.04 / Debian 12: `gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu python3-pip git`, then `pip install capstone pefile` (see BUILD.md) |

Plus: 8 GB RAM (16 GB recommended), 10 GB of free disk, internet for the first build, and one of the
supported exes (section *Tested Game Build* above). The retail SecuROM exe cannot be lifted.

### Build and install

```bash
git clone https://github.com/manuelpllull/nfsu2-recompiled-r36s.git
cd nfsu2-recompiled-r36s

python build.py "C:\Games\Need for Speed Underground 2" --check     # Windows: is the exe supported, is Docker ready?
python build.py "C:\Games\Need for Speed Underground 2"             # Windows
python3 build.py ~/Games/NFSU2/SPEED2.EXE                            # macOS / Linux
python3 build.py ~/Games/NFSU2/SPEED2.EXE --native                   # Linux without Docker
```

The first build takes about 30 minutes on a 12-core PC (lift, then compiling ~9 million lines of C); later
builds reuse the cached work. The result is `build/r36s/nfsu2-<v10|v12>.aarch64`: copy it to
`ports/nfsu2/nfsu2.aarch64`, or let `build.py --install <SD card>/ports/nfsu2` do it.

**Release binaries (maintainers).** The game exes cannot be in the repository or in CI, so the release
workflow links its binaries from compiled game code exported on a PC with both exes:

```bash
python build.py "<v1.0 SPEED2.EXE>" "<v1.2 SPEED2.EXE>" --publish     # needs the GitHub CLI (gh auth login)
```

This builds both, writes `guest-objs-v10.tar.gz` and `guest-objs-v12.tar.gz` and uploads them to the
release `r36s-guest-objs`. The workflow (`.github/workflows/release.yml`) checks they were compiled from the
release commit's source (`scripts/guest_key.py`), compiles the runtime and links `nfsu2.aarch64.v10` and
`nfsu2.aarch64.v12` into the package. Run it again whenever the lift or the guest compile inputs change
(the release stops with a message saying so).

See also:
- [BUILD.md](BUILD.md): every step, options, troubleshooting, lifting by hand
- [R36S Port Architecture & Technical Guide](docs/R36S_PORT.md)
- [PortMaster Package & Controls Setup](ports/r36s/package/README.md)

---

## Patches Applied to pcrecomp

The project uses tailored patches over upstream pcrecomp located in `patches/pcrecomp/`:

| Patch | Name / Functionality |
|:---:|---|
| **0001** | `pe_analyze`: code range restricted to adjacent executable sections holding the entry point |
| **0002** | `lift32`: Complete MMX, SSE and SSE2 support via `simd32.py`; +40 SIMD difftest cases |
| **0003** | `RECOMP_STRICT_ICALL`: host dispatch hooks for unresolved indirect calls |
| **0004** | Lazy-flag `lahf` support and x87 environment save/restore |
| **0005** | Backward-branch yields for guest thread progress and cooperative multitasking |
| **0006** | Post-call instrumentation hooks and API tracing |
| **0007** | Pre-call hooks for native argument inspection and memory rewriting |
| **0008** | x87 TOP reporting, fair FIFO machine lock, and hashed symbol lookups |
| **0009** | Explicit sign handling for signed conditions (fixed race-loading crash and car rendering) |
| **0010** | `RECOMP_NATIVE_RANGE`: selective hybrid bisection execution against original code |
| **0011** | Thread-local xmm/mxcsr and guest CPU flag preservation per guest thread |

---

## Legal & Disclaimer

- Need for Speed, Underground, EA, and all associated game materials, logos, and trademarks are the property of **Electronic Arts Inc.**
- This project is a reverse engineering and static recompilation research initiative created for educational and archival purposes.
- All code authored in this repository (lifter tooling, Android bridge, WSI wrappers, launcher and scripts) is released under the **MIT License**.
- **No proprietary game binaries, copyrighted music, videos, or 3D models are distributed in this repository.**
