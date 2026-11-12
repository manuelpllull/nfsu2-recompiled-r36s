# SPEED2.EXE binary report

Three unpacked `SPEED2.EXE` files are supported, one v1.0 and two v1.2 (`config/exe_variants.txt`). Each build of the
port is lifted from one of them; see "Second exe" and "Third exe" below for how they differ and how the hand-written
code follows.

## v10: the exe the port was first lifted from
The binary the 27,758 catalogued functions of the original lift (`src/recomp/gen/`, `catalog.json`) came from:

| Field | Value |
|---|---|
| Variant | v1.0 No-CD (build stamp `Change 116661 on 2004/10/25`, `Oct 17 2004`), region not confirmed; earlier docs called it European v1.2 |
| Size | **4,788,224 bytes** (~4.57 MB) |
| SHA-1 | `f4bb0d9942815bb6e054e773994710c612acbc34` |
| SHA-256 | `04b83e2d13c91c6ec294b578a08240ebcfff6fa9513df584edaa968e4ab9990c` |
| MD5 | `95b4dc3e9a6c72b06a8bd62b0c56ea63` |
| Analysis SHA-256 | `22aab4e461f15d2ad4df7c98ebb6b307976c3c7bba6d5c43b1510eb9c59fef8b` (`SPEED2.analysis.exe`) |
| Entry point | **0x0075B9F1** (RVA `0x35B9F1`) |
| SizeOfImage | 0x52FBFA |

## v12: European retail v1.2, SecuROM removed
Earlier docs called this one "US v1.2". It is the European retail exe unpacked: all 13,357 code pointers in
its `.rdata` are identical to those of the retail European `SPEED2.EXE` (5,987,978 bytes, SecuROM-wrapped,
sha1 `6f5dfa7d0653db2511ea0bab80eea5a94502a50c`, from a European disc); the 1,230 differing `.rdata` bytes are
the import table. The retail exe's own `.text` and `.data` are encrypted, so it cannot be lifted directly.

| Field | Value |
|---|---|
| Size | 4,800,512 bytes |
| SHA-1 | `c92e5c568a42dbf2d733e0d9b9dc9ca09794a36c` |
| SHA-256 | `f9dd86c054878ce6276beb07c1fd61874f7a1e4bf1f241b084c65b73e24168a7` |
| MD5 | `6445adbf4e8d64be51e96a69bba168a4` |
| Machine | i386, PE32, GUI subsystem 4.0 |
| ImageBase | 0x00400000 (FileHeader has RELOCS_STRIPPED set) |
| Entry point | 0x0075BCC7 (RVA 0x35BCC7) — MSVC `WinMainCRTStartup` shape (`push 0x18; push 0x7D3998; call __SEH_prolog`) |
| PE timestamp | 0x214D4C48 (1987-09-15: not a real build time); CheckSum field equals the same value |
| Linker field | 7.00 |
| Rich header | VC++ 7.1 toolchain (builds 3077 / 4035) plus objects from older toolchains (builds 8444–9466, static libraries) |
| TLS | none |
| Delay imports | none |
| Resources | 1 icon, 1 group icon, 23 RT_RCDATA |
| Version resource | none |
| Embedded build date | `Feb  9 2005` (string next to the online-protocol `VERS` field) |

## Second exe (2026-10-24)

v10 carries the build stamp `Change 116661 on 2004/10/25` (`Oct 17 2004`, the original release); v12 carries
`Change 126890 on 2005/02/09 ... 'Bump PC version number.'` (the v1.2 patch). They share the same section table (`.text` 0x401000, `.rdata` 0x783000, `.data` 0x7E8000, `.rsrc` 0x8BC000; `.text` is
0x381821 bytes in v10, 0x381AF1 in v12): v1.2 is the v1.0 code plus the patch changes, relinked:

- **Code** moves by a varying amount (+0x70 near 0x48B000, +0x370 near 0x536000, +0x2D4 at `_ftol2`,
  +0x3465 for the D3DX library), and a few functions exist in only one build or were folded differently.
  Aligning the two disassemblies pairs 1,229,515 of ~1,242,000 instructions (3,135 differing stretches).
- **Data** keeps its addresses except the import table and a few `.rdata` tables (±4 to ±0x14 bytes,
  around 0x798FC4–0x79B2F0 and in the D3DX constants). Every data address the hand-written code uses was
  checked against the instructions that reference it in both exes; only fHudScaleX moved (0x79AC0C / 0x79AC10).

`scripts/exe_addr_map.py` derives the map (aligned instructions, call and data operands inside them, and the
15,529 code-pointer slots at the same place in both exes' data). `config/exe_functions.txt` and the second
columns of `config/hooks.txt`, `overrides.txt` and `exclude.txt` come from it, and
`scripts/exe_variant.py check v10.exe v12.exe` re-derives every row (0 disagreements). Every function the
native code replaces or calls (123) has the same instructions, data operands and mapped code operands in
both exes up to its last `ret`; the 28 v12 hook cases written earlier in `widescreen.c` by hand agree with
the map, and the shader jump table at 0x5C58B8 / 0x5C5BE8 holds the mapped case targets.

Found on the way (both exes):
- `config/overrides.txt` and `exclude.txt` held v12 addresses only (the Cyrix probe 0x6F5FC5, CRT
  memcpy 0x75CFD0, the data-as-code body 0x6D5610). They are not function starts in v10, so v10 lifts
  never applied them; the v10 column now names the same code (0x6F5D25, 0x75CCF0, 0x6D5390).
- `widescreen.c` wrote the HUD X scale to both 0x79AC0C and 0x79AC10, clobbering the neighbouring
  constant of whichever exe ran; it now writes the running exe's address only.
- The seven 4:3 FMV/title-card hook cases in `widescreen.c` had v10 addresses only.

## Third exe: v12b

Another unpacked v1.2 exe: 4,788,224 bytes, sha1 `3e4952823d7a98e83688e3471c847e6b175bc2f7`, sha256
`08577b93d4b954c74679b6387792a9aca6c9434513fbc4eaeffdb343650bb899`. It carries the v12 build stamp
(`Change 126890`, `Feb  9 2005`) and has v12's entry point (0x0075BCC7), section table and `.text` size (0x381AF1),
and keeps the original `.reloc` blocks in an unnamed section at RVA 0x4F5000, as v10 does. Its region is not
confirmed. It is not v12 unpacked differently; the layout differs in two places:

- **Code** from about 0x0051C000 to 0x0068FFFF sits 0x10 higher than in v12; before and after that range the
  addresses are v12's (0x0051B3CF and 0x006D5610 are where v12 has them, 0x0051D665 and 0x0068CE8C are not).
- **`.rdata`**: the three constants `widescreen.c` writes sit 8 higher (fHudPosX 0x00797D58, fHudPosX_x2
  0x00797D60, fHudScaleX 0x0079AC18); `.rdata` before 0x0078FA1C and every `.data` address the hand-written
  code uses are where v12 has them.

No v12 exe was at hand when this column was written, so `scripts/exe_variant.py check` was not run for it and
the functions that differ from v12 are not identified. The column is the v12 one plus the shifts above, and
every row was checked in the v12b exe alone: the 47 `func` rows are function starts of its catalogue, the 55
k_fxMethods rows are the slots of the effect vtable at 0x007B6FE8, the shader jump table at 0x005C5BF8 holds
the four case rows, the hook sites carry the WidescreenFix byte patterns (`scripts/widescreen_sites.py`), the
data rows are the operands of the instructions those patterns find, and the other data addresses named in
`src/runtime` and `android/app/src/main/cpp` were checked against the v12b code and data (operands and
values). Someone with both exes can confirm it with `python scripts/exe_variant.py check v12.exe v12b.exe`.

The lift builds and the result runs on a TrimUI Brick Pro; under Xvfb with software GLES it reaches the title,
the menus and a quick race.

## Sections

As analysed on v12; v10 has the same first five sections (`.text` VirtualSize 0x381821, no trailing import section).

| Name | VA | VirtualSize | Raw | Flags | Notes |
|---|---|---|---|---|---|
| .text | 0x00401000 | 0x381AF1 | 0x1000 | code, RWX | raw offset == RVA (image-layout dump) |
| .rdata | 0x00783000 | 0x641DB | | R/W | original IAT at 0x783000 |
| .data | 0x007E8000 | 0xD35F8 | 0x35000 raw | R/W | |
| .rsrc | 0x008BC000 | 0x3854E | | R/W | |
| (blank name) | 0x008F5000 | 0x3AC16 | | discardable | **original base relocations**: 999 blocks, 91,105 HIGHLOW entries, all pointing in-image (71,093 in .text, 14,601 in .rdata, 5,411 in .data); data directory zeroed |
| (no name) | 0x00930000 | 0x2000 | | code+data, RWX | rebuilt import directory (data directory [1] points here) |

## Variant

Evidence says this is an **already-unpacked image**: zeroed relocation
directory, blank section names, fake timestamp, writable `.text` with raw
offsets equal to RVAs, and an import directory rebuilt in a trailing section.
No SecuROM/SafeDisc strings or stub code remain; `.text` is ordinary MSVC code
and analyses directly. The exact retail version (1.0/1.1/1.2) **cannot be
confirmed** without a reference hash. The 2005 dates are consistent with the
last patch, and the function addresses documented by
[yugecin/nfsu2-re](https://github.com/yugecin/nfsu2-re) match this file
(checked: 0x5BEA20 and 0x5BEEA0 start functions that reference
`Software\EA Games\Need for Speed Underground 2` at 0x7A30E8).

Nothing was done to the original file. `scripts/normalize_exe.py` writes an
analysis copy (`work/SPEED2.analysis.exe`) whose only differences are the
relocation data directory pointing at the existing relocation blocks and that
section's name set to `.reloc`.

## Imports

252 functions from 14 DLLs: advapi32 (6), d3d9 (1: `Direct3DCreate9`),
ddraw (1), dinput8 (1), dsound (1, ordinal 1 = `DirectSoundCreate`), gdi32 (12),
kernel32 (152), netapi32 (1), shell32 (1), shfolder (1), tapi32 (9),
user32 (38), winmm (6), ws2_32 (22). Full list in NFSU2_RECON.md.
