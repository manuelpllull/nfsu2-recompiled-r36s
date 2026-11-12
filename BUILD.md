# Building `nfsu2.aarch64` from your own `SPEED2.EXE`

The R36S / PortMaster port is a static recompilation: the game's x86 code is **lifted** from `SPEED2.EXE`
to C and compiled for the handheld's ARM64 CPU. The result is tied to the exact exe it was lifted from.

**Players do not need this guide**: every release contains the binaries for both supported exes
(`nfsu2.aarch64.v10` and `nfsu2.aarch64.v12`) and the launcher picks the right one. This guide is for
building it yourself and for maintainers producing the release binaries (section 8).

`build.py` does the whole job on **Windows, macOS and Linux** with one command. It checks your exe, lifts
it, compiles the game code and the port's runtime, and writes `nfsu2.aarch64`.

- [1. What you need](#1-what-you-need)
- [2. Install the tools](#2-install-the-tools) (Windows, macOS, Linux, Linux without Docker)
- [3. Get the source](#3-get-the-source)
- [4. Build](#4-build)
- [5. Put it on the SD card](#5-put-it-on-the-sd-card)
- [6. Troubleshooting](#6-troubleshooting)
- [7. What the build does (lifting by hand, lifting without Docker)](#7-what-the-build-does-lifting-by-hand)
- [8. Release binaries (maintainers)](#8-release-binaries-maintainers)

---

## 1. What you need

**Your game.** A PC copy of Need for Speed: Underground 2 with one of these unpacked `SPEED2.EXE` builds:

| Build | Game version | Size | SHA-1 |
|---|---|---|---|
| `v10` | v1.0 (Oct 2004) | 4,788,224 bytes | `f4bb0d9942815bb6e054e773994710c612acbc34` |
| `v12` | v1.2 (Feb 2005), European retail | 4,800,512 bytes | `c92e5c568a42dbf2d733e0d9b9dc9ca09794a36c` |
| `v12b` | v1.2 (Feb 2005), another unpacked exe | 4,788,224 bytes | `3e4952823d7a98e83688e3471c847e6b175bc2f7` |

The release has binaries for `v10` and `v12` only; for `v12b` build your own with this guide. It has the size
of `v10`, so the launcher cannot tell the two apart by size: with a `v12b` exe, remove `nfsu2.aarch64.v10`
from the port folder so that your `nfsu2.aarch64` is the one used.

The retail `SPEED2.EXE` straight from the disc (5,987,978 bytes, protected by SecuROM) **cannot** be used:
its code is encrypted in the file and only decrypted in memory after the disc check. `build.py` tells you
which build your exe is, or why it is not supported, before doing anything else (`--check`).

**A PC.** Any 64-bit Windows 10/11, macOS 12+ (Apple silicon or Intel) or Linux machine with:

- 8 GB of RAM (16 GB recommended: more parallel compiles), 10 GB of free disk space
- an internet connection for the first build (it downloads Ubuntu packages and the pcrecomp lifter)
- time: the first build takes about **30 minutes** on a 12-core PC (up to 90 on a slow laptop); a rebuild
  after an update takes seconds to minutes, because the lifted code and compiled objects are cached

## 2. Install the tools

`build.py` needs **Python 3.8+** and **Docker**. Docker runs the build inside an Ubuntu 22.04 container with
the ARM64 compiler, the lifter and its Python modules, so nothing else is installed on your machine.
`git` is needed to get the source (and by the build itself, inside Docker, to fetch the lifter).

### Windows 10 / 11

1. **Docker Desktop**: <https://www.docker.com/products/docker-desktop/> or `winget install Docker.DockerDesktop`.
   Keep the default *WSL 2* backend (the installer sets up WSL if it is missing; a restart may be needed).
   Start Docker Desktop once and wait until it says *Engine running*.
2. **Python 3**: <https://www.python.org/downloads/> (tick *Add python.exe to PATH*) or
   `winget install Python.Python.3.12`.
3. Optional, **Git**: `winget install Git.Git`.

Memory: Docker Desktop on WSL 2 may use up to half of your RAM, which is enough. To change it, put
`[wsl2]` and `memory=12GB` in `%UserProfile%\.wslconfig` and restart Docker Desktop.

### macOS (Apple silicon or Intel)

1. **Docker Desktop for Mac**: <https://www.docker.com/products/docker-desktop/> or `brew install --cask docker`.
   Open it once and wait until it is running.
   In *Settings > Resources*, give it at least **8 GB** of memory (the default can be lower).
2. **Python 3**: already there after `xcode-select --install`, or `brew install python`.
3. Optional, **Git**: part of `xcode-select --install`.

On Apple silicon the container runs natively (arm64) and compiles with the native ARM64 compiler, so no
emulation is involved. Keep the game folder somewhere Docker can read: anywhere under `/Users` or
`/Volumes` works by default (*Settings > Resources > File sharing*).

### Linux, with Docker (any distribution)

1. **Docker Engine**: <https://docs.docker.com/engine/install/>, e.g. on Ubuntu/Debian
   `sudo apt install docker.io`, on Fedora `sudo dnf install docker-ce` (after adding Docker's repository),
   on Arch `sudo pacman -S docker`. Then `sudo systemctl enable --now docker` and
   `sudo usermod -aG docker $USER` (log out and in again so you can run Docker without `sudo`).
2. **Python 3**: installed on nearly every distribution (`python3 --version`).

### Linux without Docker (`--native`)

On **Ubuntu 22.04** or **Debian 12** (x86-64 or arm64, also inside WSL 2 on Windows) the build can run
directly on the machine. Install:

```bash
# x86-64
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu python3 python3-pip git
# arm64 (e.g. a Raspberry Pi 5 or an ARM server): the native compiler is already an ARM64 compiler
sudo apt install gcc g++ binutils python3 python3-pip git
for t in gcc g++ nm strip objdump readelf; do command -v aarch64-linux-gnu-$t >/dev/null || sudo ln -s "$(command -v $t)" /usr/local/bin/aarch64-linux-gnu-$t; done

python3 -m pip install --user "capstone>=5.0,<6" pefile
# (Debian 12 / Ubuntu 24.04 refuse system-wide pip installs: add --break-system-packages, or use a venv)
```

Newer distributions (glibc 2.38 and later, e.g. Ubuntu 24.04) can produce a binary that needs a newer
glibc than the handheld has; the build checks for this and stops with `imports too-new symbol versions`.
Use Docker there.

## 3. Get the source

```bash
git clone https://github.com/manuelpllull/nfsu2-recompiled-r36s.git
```

or download it from GitHub (*Code > Download ZIP*). It contains `build.py`, this guide and everything
the build needs.

## 4. Build

Open a terminal in the source folder (Windows: PowerShell or *Terminal*; macOS: *Terminal*) and give
`build.py` your `SPEED2.EXE` or the game folder that contains it:

```bash
# Windows
python build.py "C:\Games\Need for Speed Underground 2"
# macOS / Linux
python3 build.py ~/Games/NFSU2/SPEED2.EXE
# Linux without Docker
python3 build.py ~/Games/NFSU2/SPEED2.EXE --native
```

On Windows you can also drag the game folder (or `SPEED2.EXE`) onto `docker\build_r36s.bat`.

Useful options:

| Option | What it does |
|---|---|
| `--check` | only checks the exe and the tools, builds nothing |
| `--install DIR` | also copies the binary into a port folder, e.g. `--install J:\ports\nfsu2` (Windows) or `--install /Volumes/EASYROMS/ports/nfsu2` (macOS); the previous binary is kept as `nfsu2.aarch64.prev` |
| `--out DIR` | where the result goes (default `build/r36s` in the source folder) |
| several exes | `python build.py <v1.0 exe> <v1.2 exe>` builds both in one run; with `--install` they go in as `nfsu2.aarch64.v10` / `.v12` |
| `--export-guest`, `--publish` | release binaries, see section 8 |
| `--jobs N` | parallel compiles (default: one per core, at most one per 2.5 GB of memory) |
| `--clean` | deletes the cached work (to free ~6 GB, or to force a full rebuild) |
| `--lift-only` | only lifts the exe to C (`src/recomp/gen/`), natively on Windows, macOS or Linux, without Docker (section 7) |
| `--no-x87-window` | with `--lift-only`: leaves out the R36S/Android x87 annotations (for the Windows x86 host build) |
| `--gen DIR`, `--work DIR` | with `--lift-only`: where the lifted C and the cached lift work go |
| `--setup-lifter` | only fetches and patches the pcrecomp lifter (checks that git works) |

The result is `build/r36s/nfsu2-<build>.aarch64` (e.g. `nfsu2-v12.aarch64`), with its SHA-1 next to it
and the whole log in `build/r36s/build.log`. The binary is built for your exe only: the launcher on the
handheld refuses to start it with a different `SPEED2.EXE`.

## 5. Put it on the SD card

The release zip `NFSU2-R36S-<version>.zip` already holds the binaries for both exes; a binary you built
yourself goes next to them as `nfsu2.aarch64` (the launcher prefers the release binary for your exe when
there is one, so remove `nfsu2.aarch64.v10`/`.v12` if you want yours used):

```text
<SD card>/ports/                (EASYROMS/ports on ArkOS / dArkOS, roms/ports on others)
├── NFSU2.sh                    <- launcher (from the release zip)
└── nfsu2/                      <- port folder (from the release zip)
    ├── nfsu2.aarch64.v10/.v12  <- the release binaries (the launcher picks the one for your exe)
    ├── nfsu2.aarch64           <- a binary you built, renamed to exactly this name (optional)
    ├── nfsu2.cfg, nfsu2.gptk, port.json, widescreen/ ...
    └── gamedata/               <- your whole game folder, including the same SPEED2.EXE
```

1. Unzip `NFSU2-R36S-<version>.zip` into the SD card's `ports/` folder.
2. Copy your game folder's contents (`SPEED2.EXE`, `GLOBAL`, `CARS`, `TRACKS`, `SOUND`, `MOVIES`,
   `LANGUAGES`, ...) into `ports/nfsu2/gamedata/`. Use the **same** `SPEED2.EXE` you built from.
3. Copy `build/r36s/nfsu2-<build>.aarch64` to `ports/nfsu2/nfsu2.aarch64`, or run the build with
   `--install <SD card>/ports/nfsu2`, which does this and checks the copy.
4. Put the card back in the handheld and start *Need for Speed Underground 2* from the Ports menu
   (restart EmulationStation if it is not listed yet). The first start creates a 512 MB swap file on the
   card and compiles shaders, so it takes longer than the next ones.

## 6. Troubleshooting

| Message or problem | What to do |
|---|---|
| `this SPEED2.EXE is not one the port supports` | Only the exes in section 1 work; the retail SecuROM exe cannot be lifted. |
| `docker not found` / `Docker is not running` | Install or start Docker Desktop (Windows/macOS) or `sudo systemctl start docker` (Linux). `build.py` starts Docker Desktop itself on Windows and macOS when it can. |
| `permission denied` talking to Docker (Linux) | `sudo usermod -aG docker $USER`, log out and in, or run `sudo python3 build.py ...`. |
| A compile ends with `Killed` or the PC runs out of memory | Run again with `--jobs 2`, or give Docker more memory (section 2). Finished files are kept. |
| `no space left on device` | Free disk space (10 GB), or `--clean` and build again. On Windows the Docker disk lives in WSL; `wsl --shutdown` releases its memory. |
| macOS: `mounts denied` / the exe is not found inside Docker | Move the game under `/Users`, or add its folder in Docker Desktop *Settings > Resources > File sharing*. |
| `imports too-new symbol versions` (`--native`) | Your distribution's glibc is too new for the handheld: build with Docker. |
| The handheld shows `SPEED2.EXE does not match this build` | The exe in `gamedata/` is not the one the binary was built from: copy that exe, or build again from the exe you have. |
| The game starts but crashes | Send `ports/nfsu2/log.txt` (and `build/r36s/build.log`) with your bug report. |

## 7. What the build does (lifting by hand)

`build.py` runs `docker/r36s_build.sh` in the container (or on the machine with `--native`). Each step is
cached in the work folder (Docker volume `nfsu2-r36s-work`, or `~/.cache/nfsu2-r36s`) and redone only when
the exe, the lifter or the lift settings change.

| Step | Tool | Time (12-core PC) |
|---|---|---|
| 1. pcrecomp lifter at commit `35548b8` + `patches/pcrecomp/*.patch`; runtime headers from `ports/r36s/sdk/include/recomp32` | `git` | < 1 min |
| 2. identify the exe (`config/exe_variants.txt`) | `scripts/exe_variant.py detect` | – |
| 3. analysis copy, vtable/RTTI seeds, function catalog (27,742 functions), lift to C, x87 analysis | `scripts/normalize_exe.py`, pcrecomp `vtable_scan.py`, `rtti.py`, `scripts/run_lift.py catalog` / `lift`, `ports/r36s/lw/lw.sh` | 7 + 2 + 1 min |
| 4. compile the lifted C for the Cortex-A35 (GCC 11 `-O2`) | `ports/r36s/build_guest_vm.sh` | ~14 min |
| 5. compile the runtime (D3D9 to GLES 2, input, sound, native helpers) and link | `ports/r36s/build_cross.sh` | < 1 min |

### Lifting without Docker, on any OS

Steps 1–3 are plain Python. `build.py --lift-only` runs them natively on Windows, macOS and Linux and
writes the lifted C to `src/recomp/gen/` (cached in `%LOCALAPPDATA%\nfsu2-r36s` on Windows,
`~/.cache/nfsu2-r36s` elsewhere):

```bash
python build.py "C:\Games\Need for Speed Underground 2" --lift-only     # Windows
python3 build.py ~/Games/NFSU2/SPEED2.EXE --lift-only                    # macOS / Linux
```

It needs only these, no Docker and no compiler:

| | Windows 10/11 | macOS 12+ | Linux |
|---|---|---|---|
| Python 3.8+ | [python.org](https://www.python.org/downloads/) or `winget install Python.Python.3.12` | `brew install python` | preinstalled (`sudo apt install python3 python3-pip`) |
| git | `winget install Git.Git` | `xcode-select --install` | `sudo apt install git` (or your distribution's package) |
| capstone 5, pefile | `py -m pip install "capstone>=5.0,<6" pefile` | `python3 -m pip install "capstone>=5.0,<6" pefile` | `python3 -m pip install --user "capstone>=5.0,<6" pefile` |

Add `--no-x87-window` when the lifted code is for the Windows x86 host build: the x87 window annotations
(step 3, last part) only compile with the R36S and Android runtime.

### Every step by hand

The same steps without `build.py`: Python 3.8+, `capstone` and `pefile` for the lift (steps 1–3, any OS),
and an ARM64 Linux compiler for steps 4–5 (Linux, WSL 2, or the Docker image):

```bash
pip install "capstone>=5.0,<6" pefile
git clone https://github.com/sp00nznet/pcrecomp.git ../pcrecomp
git -C ../pcrecomp checkout 35548b8
git -C ../pcrecomp am --3way "$PWD"/patches/pcrecomp/*.patch
cp ports/r36s/sdk/include/recomp32/*.h ports/r36s/sdk/include/recomp32/*.c ../pcrecomp/runtime/recomp32/

python scripts/exe_variant.py detect "$GAME/SPEED2.EXE"                       # v10, v12 or v12b
python scripts/normalize_exe.py "$GAME/SPEED2.EXE" work/SPEED2.analysis.exe
python ../pcrecomp/tools/cpp/vtable_scan.py work/SPEED2.analysis.exe --seeds work/vtable_seeds.json
python ../pcrecomp/tools/cpp/rtti.py        work/SPEED2.analysis.exe --seeds work/rtti_seeds.json
python -c "import json;a=json.load(open('work/vtable_seeds.json'));b=json.load(open('work/rtti_seeds.json'));json.dump(a+b,open('work/seeds_all.json','w'))"
python scripts/run_lift.py catalog work/SPEED2.analysis.exe work/seeds_all.json work/catalog.json
python scripts/run_lift.py lift    work/SPEED2.analysis.exe work/catalog.json src/recomp/gen
bash ports/r36s/lw/lw.sh

# ARM64 Linux compiler from here on
GUEST_CC=aarch64-linux-gnu-gcc GUEST_OUT=work/guest BUDGET=99999999 bash ports/r36s/build_guest_vm.sh
GUEST_DIR=work/guest OUT=work/host bash ports/r36s/build_cross.sh            # -> work/host/package/nfsu2.aarch64
```

`run_lift.py` recognises the exe and uses its column of `config/hooks.txt`, `overrides.txt` and
`exclude.txt`; the hand-written native code follows the lift through `src/runtime/nfs_exe_addr.h`
(generated from `config/exe_functions.txt` by `scripts/exe_variant.py header`). Adding another exe build:
`scripts/exe_addr_map.py` maps every address from a supported exe to the new one, and
`scripts/exe_variant.py check` verifies the tables (see `docs/NFSU2_BINARY_REPORT.md`).

Nothing derived from the game (lifted C, objects, binaries, catalogs) may be committed or published: the
`.gitignore` keeps `src/recomp/gen/`, `build/` and `work/` out of the repository.

## 8. Release binaries (maintainers)

The release workflow cannot lift anything itself: the game exes are not in the repository or the CI. A
maintainer with both exes exports the compiled game code (the *guest objects*) once, and every release of
`main` links its two binaries from it:

```bash
python build.py "<v1.0 SPEED2.EXE>" "<v1.2 SPEED2.EXE>" --publish
```

1. `build.py` builds both exes as in section 4, with `--export-guest`: next to each binary it writes
   `guest-objs-<build>.tar.gz` (the ~110 compiled game-code objects, the exe's `nfs_exe_variant.h` and a
   `guest.key`).
2. `--publish` uploads them with the [GitHub CLI](https://cli.github.com/) (`gh auth login` first, with
   write access to the repository) to the release **`r36s-guest-objs`**, creating it if needed. Without `gh`,
   upload `guest-objs-v10.tar.gz`, `guest-objs-v12.tar.gz` and their `.sha1` files to that release by hand.
3. The release workflow downloads them, compares each `guest.key` with `python scripts/guest_key.py <build>`
   computed from the release commit (the key covers the exe, the lifter and its patches, the lift driver
   and per-exe lists, the x87 analysis, the guest compile script and headers), compiles the runtime with
   `ports/r36s/build_cross.sh` and links `nfsu2.aarch64.v10` and `nfsu2.aarch64.v12` into the package.

When the key differs, the release stops with *was compiled from other source*: run the command above again
from the commit being released. Changes to the runtime alone (renderer, launcher, native helpers) do not
change the key, so they need no new export.

