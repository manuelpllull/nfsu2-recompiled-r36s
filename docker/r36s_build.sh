#!/bin/bash
# Lift one SPEED2.EXE and build the R36S binary for it. Run it through build.py (repository root), which
# does this on Windows, macOS and Linux:
#   docker run --rm -v nfsu2-r36s-work:/work -v <repo>:/src:ro -v <folder with SPEED2.EXE>:/exe:ro \
#              -v <output folder>:/out nfsu2-r36s-build bash /src/docker/r36s_build.sh
# or, natively on an Ubuntu/Debian x86-64 or arm64 box with the packages of docker/Dockerfile.r36s-build
# (build.py --native), with these paths instead of the container's:
#   EXE          the SPEED2.EXE to lift              (default /exe/SPEED2.EXE)
#   NFSU2_SRC    the repository                      (default /src)
#   NFSU2_WORK   work folder, reused between runs    (default /work)
#   NFSU2_OUT    output folder                       (default /out)
#   NFSU2_JOBS   parallel compiles                   (default: cores, at most one per 2.5 GB of memory)
#   HOST_UID/HOST_GID  owner given to the output files (Docker on Linux)
#   EXPORT_GUEST=1     also write guest-objs-<variant>.tar.gz: the compiled game code with its key
#                      (scripts/guest_key.py), which the release pipeline links into the published binary
#
# Steps, each skipped when its result for this exe is already in the work folder:
#   1. pcrecomp 35548b8 + patches/pcrecomp, with the runtime headers of ports/r36s/sdk/include/recomp32
#      (the patched headers plus the thread-local CPU state the R36S runtime uses)
#   2. which exe this is (config/exe_variants.txt); an unknown exe stops here
#   3. normalize_exe.py, vtable/RTTI seeds, catalog, lift (run_lift.py), local x87 window data (lw.sh)
#   4. guest objects: build_guest_vm.sh with the aarch64 cross compiler (GCC 11 -O2, no profile)
#   5. host runtime and link: ports/r36s/build_cross.sh
# Output in /out: nfsu2-<variant>.aarch64 (+ .sha1, .debug), lift_stats-<variant>.json, build-<variant>.txt
set -euo pipefail
EXE="${EXE:-/exe/SPEED2.EXE}"
W="${NFSU2_WORK:-/work}"
SRC="${NFSU2_SRC:-/src}"
OUTD="${NFSU2_OUT:-/out}"
PC_COMMIT=35548b8
t0=$(date +%s)
step() { echo; echo "=== [$(date +%T), +$(( $(date +%s) - t0 ))s] $*"; }

[ -f "$EXE" ] || { echo "ERROR: no SPEED2.EXE at $EXE"; exit 1; }
mkdir -p "$W" "$OUTD"
for t in aarch64-linux-gnu-gcc aarch64-linux-gnu-g++ aarch64-linux-gnu-nm aarch64-linux-gnu-strip python3 git tar; do
  command -v "$t" >/dev/null || { echo "ERROR: $t not found (see BUILD.md, Linux without Docker)"; exit 1; }
done
python3 -c "import capstone, pefile" 2>/dev/null || {
  echo "ERROR: the Python modules capstone and pefile are missing: python3 -m pip install capstone pefile"; exit 1; }
# Jobs: one per core, but no more than one per 2.5 GB of memory (the largest lifted files need ~2 GB each)
MEMK=$(awk '/MemTotal/{print $2}' /proc/meminfo)
JOBS=$(nproc); MJ=$(( MEMK / 2600000 )); [ "$MJ" -lt 1 ] && MJ=1; [ "$JOBS" -gt "$MJ" ] && JOBS=$MJ
[ -n "${NFSU2_JOBS:-}" ] && JOBS="$NFSU2_JOBS"
echo "cores $(nproc), memory $(( MEMK / 1024 )) MB -> $JOBS parallel compiles"

step "1. pcrecomp"
P="$W/pcrecomp"
# The checkout is redone when the patches change (their hash is the marker)
PATCHKEY=$(cat "$SRC"/patches/pcrecomp/*.patch | tr -d '\r' | sha1sum | cut -d' ' -f1)
if [ "$(cat "$P/.nfsu2-patched" 2>/dev/null)" != "$PATCHKEY" ]; then
  rm -rf "$P"
  git clone -q https://github.com/sp00nznet/pcrecomp.git "$P"
  git -C "$P" checkout -q "$PC_COMMIT"
  git -C "$P" -c user.name=build -c user.email=build@localhost am -q --3way "$SRC"/patches/pcrecomp/*.patch
  echo "$PATCHKEY" > "$P/.nfsu2-patched"
fi
for f in "$SRC"/ports/r36s/sdk/include/recomp32/*.h "$SRC"/ports/r36s/sdk/include/recomp32/*.c; do
  cp "$f" "$P/runtime/recomp32/"
done
echo "pcrecomp $(git -C "$P" log -1 --format=%h) + $(ls "$SRC"/patches/pcrecomp/*.patch | wc -l) patches"

step "2. exe"
sha1sum "$EXE"
V="$(python3 "$SRC/scripts/exe_variant.py" detect "$EXE")" || {
  echo "ERROR: $EXE is not a SPEED2.EXE listed in config/exe_variants.txt"; exit 1; }
echo "variant $V"
R="$W/nfsu2-$V"           # the repository copy this variant builds in (its lifted code stays here)
L="$W/lift-$V"            # analysis copy, seeds, catalog
G="$W/guest-$V"           # guest objects
H="$W/host-$V"            # host objects and the binary
mkdir -p "$R" "$L"
# Sources from the checkout (Windows line endings are fine for every tool here); generated code is kept.
( cd "$SRC" && tar --exclude=./.git --exclude=./build --exclude=./work --exclude=./src/recomp/gen \
    --exclude='*.iso' -cf - . ) | tar -C "$R" -xf -
chmod +x "$R"/ports/r36s/*.sh "$R"/ports/r36s/lw/*.sh "$R"/docker/*.sh 2>/dev/null || true
export PCRECOMP="$P"
EXESHA=$(sha1sum "$EXE" | cut -d' ' -f1)
# The lift is redone when anything it depends on changes: the exe, the lifter (pcrecomp + patches), the lift
# driver and its per-exe lists, the x87 window analysis. The catalog only depends on the exe and the lifter.
liftkey() { { echo "$EXESHA $PATCHKEY"; cat "$SRC"/scripts/run_lift.py "$SRC"/scripts/exe_variant.py "$SRC"/scripts/normalize_exe.py \
  "$SRC"/config/exe_variants.txt "$SRC"/config/seeds-*.txt "$SRC"/config/hooks.txt "$SRC"/config/overrides.txt "$SRC"/config/exclude.txt \
  "$SRC"/ports/r36s/lw/*; } | tr -d '\r' | sha1sum | cut -d' ' -f1; }
LIFTKEY=$(liftkey)
CATKEY=$( { echo "$EXESHA $PATCHKEY"; cat "$SRC/config/seeds-$V.txt" 2>/dev/null; } | sha1sum | cut -d' ' -f1)

step "3. lift"
GEN="$R/src/recomp/gen"
if [ "$(cat "$GEN/.lift-done" 2>/dev/null)" != "$LIFTKEY" ]; then
  cd "$R"
  python3 scripts/normalize_exe.py "$EXE" "$L/SPEED2.analysis.exe"
  python3 "$P/tools/cpp/vtable_scan.py" "$L/SPEED2.analysis.exe" --seeds "$L/vtable_seeds.json"
  python3 "$P/tools/cpp/rtti.py" "$L/SPEED2.analysis.exe" --seeds "$L/rtti_seeds.json"
  python3 - "$L" "$SRC/config/seeds-$V.txt" <<'PY'
import json, sys
d = sys.argv[1]
seen, out = set(), []
for n in ("vtable_seeds.json", "rtti_seeds.json"):
    for e in json.load(open(f"{d}/{n}")):
        if e["address"] not in seen:
            seen.add(e["address"]); out.append(e)
extra = sys.argv[2] if len(sys.argv) > 2 else ""
if extra:
    import os
    if os.path.exists(extra):
        for line in open(extra):
            line = line.split("#", 1)[0].strip()
            if line:
                a = int(line, 16)
                if a not in seen:
                    seen.add(a); out.append({"address": a, "address_hex": "0x%08X" % a, "note": "config seed"})
json.dump(out, open(f"{d}/seeds_all.json", "w"))
print(f"{len(out)} seeds")
PY
  if [ ! -f "$L/catalog.json" ] || [ "$(cat "$L/catalog.key" 2>/dev/null)" != "$CATKEY" ]; then
    python3 scripts/run_lift.py catalog "$L/SPEED2.analysis.exe" "$L/seeds_all.json" "$L/catalog.json"
    echo "$CATKEY" > "$L/catalog.key"
  fi
  rm -rf "$GEN"; mkdir -p "$GEN"
  python3 scripts/run_lift.py lift "$L/SPEED2.analysis.exe" "$L/catalog.json" "$GEN"
  bash ports/r36s/lw/lw.sh
  echo "$LIFTKEY" > "$GEN/.lift-done"
else
  echo "lifted code for this exe and lifter already in $GEN"
fi
python3 "$R/scripts/exe_variant.py" current

step "4. guest objects ($JOBS jobs)"
cd "$R"
for pass in 1 2 3; do
  out=$(GUEST_CC=aarch64-linux-gnu-gcc GUEST_OUT="$G" BUDGET=99999999 JOBS="$JOBS" bash ports/r36s/build_guest_vm.sh 2>&1 \
        | tee /dev/stderr | tail -1)
  case "$out" in *"guest objects complete"*) break;; esac
  [ "$pass" = 3 ] && { echo "ERROR: guest objects incomplete: $out"; exit 1; }
done
echo "$(ls "$G"/*.o | wc -l) guest objects"

step "5. host runtime and link"
rm -rf "$H"
GUEST_DIR="$G" OUT="$H" JOBS="$JOBS" bash ports/r36s/build_cross.sh

step "6. output"
cp "$H/package/nfsu2.aarch64" "$OUTD/nfsu2-$V.aarch64"
cp "$H/package/nfsu2.aarch64.debug" "$OUTD/nfsu2-$V.aarch64.debug"
sha1sum "$OUTD/nfsu2-$V.aarch64" | cut -d' ' -f1 > "$OUTD/nfsu2-$V.aarch64.sha1"
cp "$GEN/lift_stats.json" "$OUTD/lift_stats-$V.json" 2>/dev/null || true
{
  echo "variant $V, SPEED2.EXE sha1 $EXESHA"
  echo "pcrecomp $(git -C "$P" log -1 --format=%h) + $(ls "$SRC"/patches/pcrecomp/*.patch | wc -l) patches"
  echo "guest: aarch64-linux-gnu-gcc $(aarch64-linux-gnu-gcc -dumpversion), $(cat "$G/.flags" 2>/dev/null | head -c 300)"
  echo "binary: $(cat "$OUTD/nfsu2-$V.aarch64.sha1")  $(stat -c %s "$OUTD/nfsu2-$V.aarch64") bytes"
  echo "built $(date -u +%FT%TZ) in $(( $(date +%s) - t0 )) s"
} | tee "$OUTD/build-$V.txt"
if [ "${EXPORT_GUEST:-0}" = 1 ]; then
  step "7. export the guest objects for the release"
  GK=$(python3 "$R/scripts/guest_key.py" "$V")
  X="$W/export-$V"; rm -rf "$X"; mkdir -p "$X/objs"
  cp "$G"/*.o "$X/objs/"
  cp "$GEN/nfs_exe_variant.h" "$X/"
  { echo "variant $V"; echo "key $GK"; echo "exe_sha1 $EXESHA"; echo "objects $(ls "$X"/objs/*.o | wc -l)"
    echo "source_commit $(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown)"; echo "built $(date -u +%FT%TZ)"; } > "$X/guest.key"
  tar -C "$X" -czf "$OUTD/guest-objs-$V.tar.gz" guest.key nfs_exe_variant.h objs
  sha1sum "$OUTD/guest-objs-$V.tar.gz" | cut -d' ' -f1 > "$OUTD/guest-objs-$V.tar.gz.sha1"
  echo "guest-objs-$V.tar.gz: key $GK, $(stat -c %s "$OUTD/guest-objs-$V.tar.gz") bytes"
fi
if [ -n "${HOST_UID:-}" ]; then
  chown "$HOST_UID:${HOST_GID:-$HOST_UID}" "$OUTD"/nfsu2-"$V".aarch64* "$OUTD"/lift_stats-"$V".json "$OUTD"/build-"$V".txt \
    "$OUTD"/guest-objs-"$V".tar.gz* 2>/dev/null || true
fi
echo "BUILD OK"
