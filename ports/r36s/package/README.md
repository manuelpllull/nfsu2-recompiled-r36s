# Need for Speed: Underground 2 — R36S & PortMaster Installation Guide

This port runs a native static binary recompilation of **Need for Speed: Underground 2** on the **R36S** handheld console (Rockchip RK3326 SoC) and compatible ARM64 handhelds running ArkOS, AmberELEC, or ROCKNIX.

---

## 📁 Installation Directory Structure

Copy the files to your SD card (either `TF1` or `TF2/EASYROMS`) under the `ports/` directory:

```text
/roms/ports/
├── NFSU2.sh                        <- Launcher script
└── nfsu2/                          <- Game folder
    ├── nfsu2.aarch64.v10           <- game binary for SPEED2.EXE v1.0 (included)
    ├── nfsu2.aarch64.v12           <- game binary for SPEED2.EXE v1.2 (included)
    ├── port.json                   <- PortMaster metadata
    ├── nfsu2.gptk                  <- Gamepad mapping profile
    ├── libs/                       <- Bundled runtime libraries (libSDL2, etc.)
    ├── widescreen/                 <- HUD layout for wide screens (ThirteenAG's widescreen fix, MIT)
    └── gamedata/                   <- YOUR PC GAME FILES GO HERE
        ├── SPEED2.EXE              <- unpacked v1.0 or v1.2 exe (see below)
        ├── GLOBAL/                 <- Global assets (GLOBALB.BUN, etc.)
        ├── CARS/                   <- Car models, textures, vinyls
        ├── TRACKS/                 <- City tracks and collision data
        ├── SOUND/                  <- Music and SFX bank files
        ├── MOVIES/                 <- Intro & FMV video cutscenes
        ├── LANGUAGES/              <- Language files (Spanish, English, etc.)
        └── ...
```

> [!IMPORTANT]
> **No copyrighted game assets are bundled.** You must provide your own legitimate retail PC copy of Need for Speed: Underground 2.
>
> **Required Executable**: an unpacked (No-CD) `SPEED2.EXE`, v1.0 or v1.2. The release has a binary for each, and the launcher picks the one for your exe:
> * **v10** (v1.0): `4,788,224 bytes`, MD5 `95b4dc3e9a6c72b06a8bd62b0c56ea63`, SHA-256 `04b83e2d13c91c6ec294b578a08240ebcfff6fa9513df584edaa968e4ab9990c`
> * **v12** (European retail v1.2, SecuROM removed): `4,800,512 bytes`, MD5 `6445adbf4e8d64be51e96a69bba168a4`, SHA-256 `f9dd86c054878ce6276beb07c1fd61874f7a1e4bf1f241b084c65b73e24168a7`
> * The retail `SPEED2.EXE` with SecuROM (5,987,978 bytes) does not work, nor do other builds (v1.1, other v1.0 dumps): the launcher stops with a message.

---

## 🛠️ Building `nfsu2.aarch64` yourself (optional)

The release already contains the binaries for v1.0 and v1.2. To build one yourself (for example from a
modified source), use `build.py` from the repository on Windows, macOS or Linux; see `BUILD.md` there:

```bash
python build.py "C:\Games\Need for Speed Underground 2" --install J:\ports\nfsu2     # Windows
python3 build.py ~/Games/NFSU2/SPEED2.EXE --install /Volumes/EASYROMS/ports/nfsu2       # macOS / Linux
```

A self-built binary is installed as `nfsu2.aarch64`; the launcher uses it when there is no
`nfsu2.aarch64.v10`/`.v12` for your exe.

---

## 🎮 Controller Layout (R36S)

Button names are the labels printed on the R36S. The layout follows the PS2 version (Cross = bottom button).

| R36S Button | Driving / Free Roam | Menus |
|:---|:---|:---|
| **Left Stick** | Steer (analog) | Navigate |
| **D-Pad Up** | Enter shop / engage event / challenge | Navigate |
| **D-Pad Down** | Career status | Navigate |
| **D-Pad Left** | World map | Navigate |
| **D-Pad Right** | SMS messages | Navigate |
| **B** (bottom) | Accelerate | Select / Confirm |
| **Y** (left) | Brake / Reverse | Space |
| **A** (right) | Handbrake (E-Brake) | Back |
| **X** (top) | Change camera | Back |
| **R1** | Nitrous | — |
| **L1** | Look behind | — |
| **R2 / L2** | Shift up / Shift down (manual gearbox) | — |
| **Start** | Pause | Select / Confirm |
| **Select** | Reset car | — |
| **Select + Start** | **Quit to the system menu** | **Quit to the system menu** |
| **Select + R3** | Show/hide the frame counter | Show/hide the frame counter |
| **L3** | Debug camera: next mode / back to normal (`NFSU2_DEBUG_CAMERA=1`) | — |
| **Right Stick** | Debug camera: turn (left/right), forward/back (up/down) | Garage: turn the camera around the car |

---

## ⚡ Technical Specifications & Performance

- **Resolution**: the handheld's own screen, detected at start. On the R36S that is 640×480, drawn
  straight to the panel as before. Screens wider than 4:3 (854×480, 960×544, 1280×720 …) get real
  widescreen: a wider field of view (nothing stretched), the HUD at the screen edges, and a correct
  mirror, radar and videos, ported from ThirteenAG's NFSU2 widescreen fix. Screens narrower than 4:3
  (e.g. 720×720) show the full 4:3 picture with black bars. Portrait-mounted panels are turned to
  landscape.
- **Display options** (in `ports/nfsu2/nfsu2.cfg`, all optional):

  | Setting | Effect |
  |---|---|
  | `NFSU2_ASPECT=4:3` | Always 4:3, with bars on wide screens |
  | `NFSU2_RENDER_SCALE=0.75` | Render below the panel resolution and scale up (faster on big screens) |
  | `NFSU2_RENDER=854x480` | Exact render size |
  | `NFSU2_SCREEN=854x480` | Panel size, only if it is detected wrongly |
  | `NFSU2_ROTATE=90` or `270` | Panel mounted rotated (portrait panels default to 90) |

  More pixels cost GPU time. On a weak GPU with a big screen, start with `NFSU2_RENDER_SCALE=0.75`.
- **GPU-only extras** (`NFSU2_GPU_EXTRAS=1` in `nfsu2.cfg`): trilinear texture filtering, the game's colour
  tinting and detailed car models on top of the chosen profile. They add no draw calls and no CPU work; whether
  the GPU has room for them is what the `[NFSU2 GPU]` lines in `log.txt` show. `NFSU2_GPU_EXTRAS=ab` alternates
  off/on at each launch for a comparison (`log.prev.txt` keeps the previous session).
- **Speed-ups you can switch off** (in `nfsu2.cfg`, only for troubleshooting): `NFSU2_ASYNC_TEXTURES=0`
  (textures created without stopping the game, since v33), `NFSU2_GROUP_CACHE=0` (replay of learned world draws),
  `NFSU2_RENDER_THREAD=0` (separate render thread), `NFSU2_INSTANCING=0` (repeated world objects such as lamp
  posts and barriers drawn with one instanced draw, since v34), `NFSU2_NATIVE_SCENE=0` (native scene culling and
  render-item code instead of the translated originals, since v35), `NFSU2_PRESENT_THREAD=0` (a separate thread shows
  the finished frame, so the CPU can start the next frame while the GPU still draws the last one, since v37;
  `NFSU2_PRESENT_SLOTS=3` allows one more frame in flight). `NFSU2_GPU_PROBE=0` turns off the GPU timing sample that
  `log.txt` reports as `[NFSU2 GPU] probe`.
- **Target FPS**: 30–60 FPS with CPU performance governor enabled in `NFSU2.sh`.
- **Audio Engine**: SDL2 PCM stereo float mixer with low latency.
- **Memory**: Runs comfortably within the 1 GB DDR3L RAM boundary of RK3326.
- **Save Games**: Automatically saved in `gamedata/AppData/Local/` or profile directory.

---

## 🛠️ Troubleshooting

- **Older ArkOS (2.0) or another image with an old glibc**: since v36 the binary needs only glibc 2.18 or newer (the
  C++ runtime is built in). If it still does not start, `log.txt` or the terminal shows the missing library or
  version; send that line. `log.txt` also reports `glibc compat: newer libm functions` (the running glibc has the
  faster math functions) or `glibc compat: GLIBC_2.17 libm functions` (older glibc; same results, slightly slower
  `log`/`pow`). `NFSU2_GLIBC_COMPAT_OLD=1` forces the second case for testing.
- **Black screen on launch**: Verify that `SPEED2.EXE` is present inside `gamedata/`. Check `ports/nfsu2/log.txt` for details.
- **Controls not responding**: Ensure `gamecontrollerdb.txt` exists in PortMaster, or verify controller configuration in EmulationStation.
- **Audio stutter**: Ensure CPU governor is set to `performance` in the launch script.
