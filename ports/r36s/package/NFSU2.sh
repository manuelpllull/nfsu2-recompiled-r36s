#!/bin/bash
# PortMaster Launch Script for Need for Speed: Underground 2 (Recompiled)
# Target Devices: R36S, RG351MP, RGB20S, RK2020 (Rockchip RK3326 ARM64), Anbernic RG40XX H/V, RG35XX (Allwinner H700)
# Supported OS: ArkOS, AmberELEC, ROCKNIX, TheRA, Knulli, muOS

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

# PortMaster is optional: a fresh dArkOS has it only after Tools > Install.PortMaster. Without it the
# game still starts (the binary reads the pad through SDL itself; nothing here needs gptokeyb).
if [ -f "$controlfolder/control.txt" ]; then
  source "$controlfolder/control.txt"
  [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
  get_controls
fi
# Root firmwares (Knulli, muOS, ROCKNIX) run ports as root and have no sudo: never call sudo there.
if [ "$(id -u)" = "0" ]; then
  ESUDO=""
elif [ -z "${ESUDO+x}" ]; then
  ESUDO="sudo"
fi

CURR_TTY="/dev/tty1"
exec < "$CURR_TTY"

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
if [ -n "$directory" ] && [ -d "/$directory/ports/nfsu2" ]; then
  GAMEDIR="/$directory/ports/nfsu2"
else
  GAMEDIR="$SCRIPT_DIR/nfsu2"
fi
cd "$GAMEDIR"

# Binary update delivered in parts (the build host can only write files of up to 30 MB to the card): join
# nfsu2.aarch64.partNN, check the result against nfsu2.aarch64.update.sha1, then replace the binary. On a
# checksum failure the parts are kept and the old binary runs. The result goes to update.log.
if [ -f nfsu2.aarch64.update.sha1 ] && ls nfsu2.aarch64.part* >/dev/null 2>&1; then
  cat nfsu2.aarch64.part* > nfsu2.aarch64.new
  if [ "$(sha1sum nfsu2.aarch64.new | cut -d' ' -f1)" = "$(tr -d ' \n\r' < nfsu2.aarch64.update.sha1)" ]; then
    chmod 755 nfsu2.aarch64.new; mv -f nfsu2.aarch64.new nfsu2.aarch64
    rm -f nfsu2.aarch64.part* nfsu2.aarch64.update.sha1 ._nfsu2.aarch64.part* ._nfsu2.aarch64.update.sha1
    echo "updated nfsu2.aarch64 ($(date)): $(sha1sum nfsu2.aarch64)" > update.log
  else
    rm -f nfsu2.aarch64.new
    echo "update parts failed the checksum ($(date)); kept the old binary" > update.log
  fi
  sync
fi

export DEVICE_ARCH="aarch64"
export LD_LIBRARY_PATH="$GAMEDIR/libs:$LD_LIBRARY_PATH"
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# Video backend.
# ArkOS / ROCKNIX / Knulli (RK3326, mainline H700): DRM device present -> KMSDRM.
# muOS on H700 (RG40XX H/V, RG35XX): stock Anbernic kernel with the Mali fbdev blob, no /dev/dri ->
# let the firmware's own SDL2 pick its Mali/fbdev backend. NFSU2_VIDEODRIVER in nfsu2.cfg overrides.
[ -f "$GAMEDIR/nfsu2.cfg" ] && NFSU2_VIDEODRIVER="$(sed -n 's/^NFSU2_VIDEODRIVER=//p' "$GAMEDIR/nfsu2.cfg" | tr -d '"\r')"
if [ -n "$NFSU2_VIDEODRIVER" ]; then
  export SDL_VIDEODRIVER="$NFSU2_VIDEODRIVER"
elif [ -e /dev/dri/card0 ] || [ -e /dev/dri/card1 ]; then
  export SDL_VIDEODRIVER=kmsdrm
else
  unset SDL_VIDEODRIVER
  # The bundled libs/libSDL2 is built for KMSDRM; on fbdev firmwares use the system SDL2 instead.
  for sdl in /usr/lib/libSDL2-2.0.so.0 /usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0 /usr/lib64/libSDL2-2.0.so.0; do
    if [ -f "$sdl" ]; then export LD_PRELOAD="$sdl${LD_PRELOAD:+:$LD_PRELOAD}"; break; fi
  done
fi
export SDL_AUDIODRIVER=alsa
export ALSOFT_DRIVERS=alsa
export SDL_VIDEO_GL_DRIVER=libGLESv2.so
export SDL_VIDEO_EGL_DRIVER=libEGL.so
export MESA_GL_VERSION_OVERRIDE=2.1
export MESA_GLES_VERSION_OVERRIDE=2.0
export MALLOC_CHECK_=0

# Clocks: CPU, GPU (Mali) and memory controller (DMC) at their top step while the game runs. The RK3326
# devfreq governors otherwise keep the GPU and DRAM clocks low, and this game is bound by CPU and memory.
# The previous governors are restored on exit.
SUDO="$ESUDO"
CLOCK_STATE=""
for gov in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor \
           /sys/class/devfreq/*/governor; do
  [ -f "$gov" ] || continue
  old=$(cat "$gov" 2>/dev/null)
  echo performance | $SUDO tee "$gov" > /dev/null 2>&1 && CLOCK_STATE="$CLOCK_STATE $gov=$old"
done
restore_clocks() {
  for entry in $CLOCK_STATE; do
    echo "${entry#*=}" | $SUDO tee "${entry%%=*}" > /dev/null 2>&1 || true
  done
}
trap restore_clocks EXIT

# R36S 1GB RAM Protection:
# Free reclaimable page caches to maximize contiguous memory before launching
sync
echo 3 | $ESUDO tee /proc/sys/vm/drop_caches > /dev/null 2>&1 || true

# Ensure safety swapfile to prevent OOM Killer from terminating the game during high texture loads
SWAP_TOTAL=$(free -m | awk '/Swap:/ {print $2}')
if [ -z "$SWAP_TOTAL" ] || [ "$SWAP_TOTAL" -lt 256 ]; then
  echo "[PortMaster] Ensuring 512MB safety swap on SD card..."
  if [ ! -f "$GAMEDIR/.swapfile" ]; then
    dd if=/dev/zero of="$GAMEDIR/.swapfile" bs=1M count=512 2>/dev/null
    chmod 600 "$GAMEDIR/.swapfile" 2>/dev/null
    mkswap "$GAMEDIR/.swapfile" >/dev/null 2>&1
  fi
  if $ESUDO swapon "$GAMEDIR/.swapfile" >/dev/null 2>&1; then
    SWAP_ADDED=1
  else
    # The card's file system cannot hold swap (exFAT): drop the file instead of leaving 512 MB behind
    rm -f "$GAMEDIR/.swapfile"
    echo "[NFSU2] swap file not usable on this card; running without extra swap" >> "$GAMEDIR/log.txt"
  fi
fi

# Verify game assets presence
EXE=""
for cand in "$GAMEDIR/gamedata/SPEED2.EXE" "$GAMEDIR/gamedata/speed2.exe" "$GAMEDIR/SPEED2.EXE" "$GAMEDIR/speed2.exe"; do
  if [ -f "$cand" ]; then EXE="$cand"; break; fi
done

if [ -z "$EXE" ]; then
  printf "\033c" > "$CURR_TTY"
  printf "======================================================================\n" > "$CURR_TTY"
  printf " ERROR: Game assets not found!\n" > "$CURR_TTY"
  printf " Please copy your PC Need for Speed Underground 2 files into:\n" > "$CURR_TTY"
  printf "   %s/gamedata/\n" "$GAMEDIR" > "$CURR_TTY"
  printf " Required files: SPEED2.EXE, GLOBAL/, CARS/, TRACKS/, SOUND/, etc.\n" > "$CURR_TTY"
  printf "======================================================================\n" > "$CURR_TTY"
  sleep 5
  exit 1
fi

# Verify the executable: the binary is built from one SPEED2.EXE (config/exe_variants.txt in the source) and
# carries "NFSU2_EXE_BUILT_FOR=<id>:<size>". v10 (v1.0) is 4,788,224 bytes, v12 (European retail v1.2, No-CD) 4,800,512.
EXESIZE=$(stat -c%s "$EXE" 2>/dev/null || wc -c < "$EXE" | tr -d ' ')
# The release ships one binary per supported exe (nfsu2.aarch64.v10, nfsu2.aarch64.v12): take the one for
# this SPEED2.EXE. Otherwise a binary built yourself (build.py), plain nfsu2.aarch64, is used.
BIN=""
# v1.1 (WidescreenFix "1.1 exe", Change 117143) has the same size as v1.0: tell them apart by MD5.
EXEMD5=""
[ "$EXESIZE" = "4788224" ] && EXEMD5=$(md5sum "$EXE" 2>/dev/null | cut -d' ' -f1)
case "$EXESIZE" in
  4788224) if [ "$EXEMD5" = "e92e984afb427410cd7b404bc589fd42" ]; then
             [ -f "$GAMEDIR/nfsu2.aarch64.v11" ] && BIN="nfsu2.aarch64.v11"
           else
             [ -f "$GAMEDIR/nfsu2.aarch64.v10" ] && BIN="nfsu2.aarch64.v10"
           fi ;;
  4800512) [ -f "$GAMEDIR/nfsu2.aarch64.v12" ] && BIN="nfsu2.aarch64.v12" ;;
esac
[ -n "$BIN" ] || BIN="nfsu2.aarch64"
if [ ! -f "$GAMEDIR/$BIN" ]; then
  printf "\033c" > "$CURR_TTY" 2>/dev/null || true
  printf "======================================================================\n" > "$CURR_TTY" 2>/dev/null || true
  printf " ERROR: game binary not found: %s/%s\n" "$GAMEDIR" "$BIN" > "$CURR_TTY" 2>/dev/null || true
  printf " Copy nfsu2.aarch64.v10 / nfsu2.aarch64.v12 and the libs/ folder\n" > "$CURR_TTY" 2>/dev/null || true
  printf " from the port release into %s\n" "$GAMEDIR" > "$CURR_TTY" 2>/dev/null || true
  printf "======================================================================\n" > "$CURR_TTY" 2>/dev/null || true
  echo "[NFSU2] binary $BIN missing (SPEED2.EXE $EXESIZE bytes)" >> "$GAMEDIR/log.txt"
  sleep 6
  exit 1
fi
chmod 755 "$GAMEDIR/$BIN" 2>/dev/null || true
BUILT=$(grep -a -o -m1 'NFSU2_EXE_BUILT_FOR=[a-z0-9]*:[0-9]*' "$GAMEDIR/$BIN" 2>/dev/null | head -n1 | cut -d= -f2)
WANT_ID="${BUILT%%:*}"; WANT_SIZE="${BUILT##*:}"
if [ -z "$BUILT" ]; then
  WANT_ID="v10"; WANT_SIZE="4788224"   # binaries from before the marker were all lifted from v10
fi
[ "$WANT_ID" = "v12" ] && export NFSU2_PARALLEL_THREADS="${NFSU2_PARALLEL_THREADS:-7263c7}"
echo "[NFSU2] SPEED2.EXE: $EXESIZE bytes; $BIN was built for $WANT_ID ($WANT_SIZE bytes)" >> "$GAMEDIR/log.txt" 2>&1
if [ "$WANT_ID" = "v11" ] && [ "$EXEMD5" != "e92e984afb427410cd7b404bc589fd42" ]; then WANT_SIZE="mismatch"; fi
if [ "$WANT_ID" = "v10" ] && [ "$EXEMD5" = "e92e984afb427410cd7b404bc589fd42" ]; then
  printf "\033c" > "$CURR_TTY" 2>/dev/null || true
  printf " ERROR: this SPEED2.EXE is v1.1; copy nfsu2.aarch64.v11 into %s\n" "$GAMEDIR" > "$CURR_TTY" 2>/dev/null || true
  echo "[NFSU2] SPEED2.EXE is v1.1 but $BIN is for v1.0: nfsu2.aarch64.v11 missing" >> "$GAMEDIR/log.txt"
  sleep 6; exit 1
fi
if [ "$EXESIZE" != "$WANT_SIZE" ]; then
  printf "\033c" > "$CURR_TTY" 2>/dev/null || true
  printf "======================================================================\n" > "$CURR_TTY" 2>/dev/null || true
  printf " ERROR: SPEED2.EXE does not match this build.\n" > "$CURR_TTY" 2>/dev/null || true
  printf " Yours is %s bytes; %s was built from the %s exe\n" "$EXESIZE" "$BIN" "$WANT_ID" > "$CURR_TTY" 2>/dev/null || true
  printf " (%s bytes). Use that SPEED2.EXE, or build the port from yours\n" "$WANT_SIZE" > "$CURR_TTY" 2>/dev/null || true
  printf " (the 4,788,224-byte v1.0 and 4,800,512-byte v1.2 No-CD exes are supported).\n" > "$CURR_TTY" 2>/dev/null || true
  printf "======================================================================\n" > "$CURR_TTY" 2>/dev/null || true
  sleep 6
  exit 1
fi

# Unbind vtconsole to allow KMSDRM full access to DRM primary plane
printf "\033c" > "$CURR_TTY" 2>/dev/null || true
printf "\e[?25l" > "$CURR_TTY" 2>/dev/null || true
echo 0 | $ESUDO tee /sys/class/vtconsole/vtcon0/bind > /dev/null 2>&1 || true
echo 0 | $ESUDO tee /sys/class/vtconsole/vtcon1/bind > /dev/null 2>&1 || true

# ---------------------------------------------------------------------------
# Graphics profile and frame-rate cap.
# These feed the game's OWN PC graphics options (the registry values its Options
# menu writes), so the game renders exactly as it would on PC with those settings.
# Override in "$GAMEDIR/nfsu2.cfg", e.g.:
#   NFSU2_GRAPHICS=quality      # performance | balanced | quality
#   NFSU2_FPS=30                # 30 or 60
#   NFSU2_REGISTRY_EXTRA="g_CrowdEnable=0"   # any extra g_* option
#   NFSU2_GPU_EXTRAS=1          # test: options that cost GPU time but no CPU time (trilinear texture
#                               # filtering, colour tinting, detailed car models) on top of the profile
#   NFSU2_GPU_EXTRAS=ab         # alternate off/on at every launch (to compare two sessions' logs)
#   NFSU2_DRAW_DIST=200         # metres; scenery wholly beyond it is not drawn and the fog reaches
#                               # opaque at that distance, so nothing visibly pops (0 = the game's own)
# Display (the game adapts to the handheld's own screen; nothing to set on a 640x480 R36S):
#   NFSU2_ASPECT=auto           # auto: widescreen on 16:9 etc. panels, 4:3 with bars on narrower ones
#                               # 4:3: always 4:3 (black bars on the sides of a wide screen)
#   NFSU2_RENDER_SCALE=0.75     # render below the panel resolution and scale up (faster on big panels)
#   NFSU2_RENDER=854x480        # or an exact render size
#   NFSU2_SCREEN=854x480        # only if the panel size is detected wrongly
#   NFSU2_ROTATE=90             # panel mounted rotated (90 or 270; portrait panels default to 90)
#   NFSU2_SHOW_FPS=1            # frame counter on from the start (Select + R3 toggles it in game)
# ---------------------------------------------------------------------------
NFSU2_GRAPHICS=balanced
NFSU2_FPS=60
NFSU2_REGISTRY_EXTRA=""
NFSU2_GPU_EXTRAS=0
NFSU2_DRAW_DIST=200
NFSU2_LANG=Spanish
[ -f "$GAMEDIR/nfsu2.cfg" ] && . "$GAMEDIR/nfsu2.cfg"
export NFSU2_DRAW_DIST NFSU2_LANG
for v in NFSU2_ASPECT NFSU2_RENDER_SCALE NFSU2_RENDER NFSU2_SCREEN NFSU2_ROTATE NFSU2_SHOW_FPS; do
  eval "[ -n \"\${$v:-}\" ] && export $v"
done
# HUD layout table of ThirteenAG's widescreen fix (MIT, see widescreen/LICENSE), used on wide screens
# unless the game folder has its own scripts/NFSUnderground2.WidescreenFix.dat.
export NFSU2_WIDESCREEN_DAT="$GAMEDIR/widescreen/NFSUnderground2.WidescreenFix.dat"

COMMON="NotFirstTime=1;g_MotionBlurEnable=0;g_LightStreaksEnable=0;g_DepthOfFieldEnable=0;g_OverBrightEnable=0;g_BleachByPassEnable=0;g_FSAALevel=0;g_RainEnable=0;g_RacingResolution=0;g_VSyncOn=0;g_CarDamageEnable=0"
case "$NFSU2_GRAPHICS" in
  performance)
    PROFILE="g_PerformanceLevel=1;g_TextureFiltering=1;g_CarEnvironmentMapEnable=0;g_CarEnvironmentMapUpdateData=0;g_CarShadowEnable=1;g_CarHeadlightEnable=1;g_CarLightingEnable=1;g_CrowdEnable=0;g_RoadReflectionEnable=0;g_FogEnable=1;g_HorizonFogEnable=1;g_LightGlowEnable=0;g_AnimatedTextureEnable=1;g_ParticleSystemEnable=0;g_WorldLodLevel=0;g_CarLodLevel=0;g_TintingEnable=0" ;;
  quality)
    PROFILE="g_PerformanceLevel=3;g_TextureFiltering=2;g_CarEnvironmentMapEnable=1;g_CarEnvironmentMapUpdateData=1;g_CarShadowEnable=1;g_CarHeadlightEnable=1;g_CarLightingEnable=1;g_CrowdEnable=1;g_RoadReflectionEnable=1;g_FogEnable=1;g_HorizonFogEnable=1;g_LightGlowEnable=1;g_AnimatedTextureEnable=1;g_ParticleSystemEnable=1;g_WorldLodLevel=3;g_CarLodLevel=1;g_TintingEnable=1" ;;
  *)
    PROFILE="g_PerformanceLevel=2;g_TextureFiltering=1;g_CarEnvironmentMapEnable=1;g_CarEnvironmentMapUpdateData=0;g_CarShadowEnable=1;g_CarHeadlightEnable=1;g_CarLightingEnable=1;g_CrowdEnable=0;g_RoadReflectionEnable=0;g_FogEnable=1;g_HorizonFogEnable=1;g_LightGlowEnable=1;g_AnimatedTextureEnable=1;g_ParticleSystemEnable=1;g_WorldLodLevel=1;g_CarLodLevel=0;g_TintingEnable=0" ;;
esac
if [ "$NFSU2_GPU_EXTRAS" = "ab" ]; then
  if [ -f "$GAMEDIR/.gpu_extras_on" ]; then NFSU2_GPU_EXTRAS=0; rm -f "$GAMEDIR/.gpu_extras_on"
  else NFSU2_GPU_EXTRAS=1; touch "$GAMEDIR/.gpu_extras_on"; fi
fi
GPU_EXTRAS=""
[ "$NFSU2_GPU_EXTRAS" = "1" ] && GPU_EXTRAS="g_TextureFiltering=2;g_TintingEnable=1;g_CarLodLevel=1"
export NFSU2_REGISTRY="$COMMON;$PROFILE${GPU_EXTRAS:+;$GPU_EXTRAS}${NFSU2_REGISTRY_EXTRA:+;$NFSU2_REGISTRY_EXTRA}"
CAP_ARG="--cap60"; [ "$NFSU2_FPS" = "30" ] && CAP_ARG="--cap30"

# Record the clocks actually in effect (launcher.log next to log.txt)
{
  echo "[launcher] graphics=$NFSU2_GRAPHICS gpu_extras=$NFSU2_GPU_EXTRAS fps=$NFSU2_FPS"
  # What this kernel offers for GPU load (read by the binary for its [NFSU2 GPU] log line)
  for d in /sys/class/devfreq/*; do [ -d "$d" ] && echo "[launcher] devfreq $d: $(ls "$d" 2>/dev/null | tr '\n' ' ')"; done
  for f in /sys/devices/platform/*gpu*/utilisation /sys/class/misc/mali0/device/utilisation; do
    [ -f "$f" ] && echo "[launcher] $f = $(cat "$f" 2>/dev/null)"
  done
  ls -d /sys/devices/platform/*gpu* 2>/dev/null | sed 's/^/[launcher] gpu device /'
  for f in /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq /sys/class/devfreq/*/cur_freq; do
    [ -f "$f" ] && echo "[launcher] $f = $(cat "$f" 2>/dev/null) ($(cat "$(dirname "$f")/governor" 2>/dev/null || cat "$(dirname "$f")/scaling_governor" 2>/dev/null))"
  done
  echo "[launcher] CFW=${CFW_NAME:-unknown} uid=$(id -u) SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-<system default>} LD_PRELOAD=${LD_PRELOAD:-}"
  ls -l /dev/dri /dev/fb0 /dev/mali* 2>&1 | sed 's/^/[launcher] /'
  free -m
  uname -a
  for f in /sys/kernel/mm/transparent_hugepage/enabled /sys/kernel/mm/transparent_hugepage/defrag /sys/kernel/mm/transparent_hugepage/hpage_pmd_size; do
    [ -f "$f" ] && echo "[launcher] $f = $(cat "$f" 2>/dev/null)"
  done
  grep -i huge /proc/meminfo
} > "$GAMEDIR/launcher.log" 2>&1

# Keep the previous run's log (a crash on one run is otherwise overwritten by the next launch)
[ -f "$GAMEDIR/log.txt" ] && mv -f "$GAMEDIR/log.txt" "$GAMEDIR/log.prev.txt"

# GPTOKEYB: START + SELECT exit shortcut.
if [ -n "${GPTOKEYB:-}" ]; then
  $GPTOKEYB "$BIN" -k &
fi

# Launch game: GLES 2.0 at the screen's native resolution (see Display above)
LANG_ARG="--lang=$NFSU2_LANG"
echo "[NFSU2] running $BIN" > "$GAMEDIR/log.txt"
"./$BIN" "$GAMEDIR/gamedata" $CAP_ARG $LANG_ARG >> "$GAMEDIR/log.txt" 2>&1
EXIT_CODE=$?

# Get everything onto the card before the console returns to the menu (people often switch off right
# after quitting): pending writes first, then the swap file this launcher enabled on the card.
sync
if [ -n "$SWAP_ADDED" ]; then
  $ESUDO swapoff "$GAMEDIR/.swapfile" >/dev/null 2>&1
  sync
fi

# Restore console and services on exit
echo 1 | $ESUDO tee /sys/class/vtconsole/vtcon0/bind > /dev/null 2>&1 || true
echo 1 | $ESUDO tee /sys/class/vtconsole/vtcon1/bind > /dev/null 2>&1 || true
$ESUDO systemctl restart oga_events >/dev/null 2>&1 &
printf "\033c" > "$CURR_TTY" 2>/dev/null || true
printf "\e[?25h" > "$CURR_TTY" 2>/dev/null || true
exit $EXIT_CODE
