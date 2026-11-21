#!/usr/bin/env python3
"""NFSU2 lift driver (pcrecomp lift32, global-register model).

  run_lift.py catalog  <analysis.exe> <seeds.json> <catalog.json>
      Recursive descent (pcrecomp disasm32) to a catalog that keeps every
      function's BASIC BLOCK ranges, so lifting can re-decode without paying
      for the whole discovery again (it takes ~12 minutes on SPEED2.EXE).

  run_lift.py lift  <analysis.exe> <catalog.json> <outdir> [--split N] [--names names.json] [--prefix P]
      Lift every catalogued function with lift32.Lifter and write:
        recomp_NNNN.c          the functions, N per file
        recomp_funcs.h         declarations
        recomp_dispatch.c      sorted VA -> function table (binary search)
        recomp_imports.c       IAT slot VA -> "dll!name" (for crash trails)
        lift_stats.json        counts for docs/LIFT_REPORT.md
        recomp_variant.c       which SPEED2.EXE this is (config/exe_variants.txt)
        nfs_exe_variant.h      the same, for src/runtime/nfs_exe_addr.h
      The exe is recognised by its SHA-1 (or the .text SHA-1 of the analysis
      copy); config/hooks.txt, overrides.txt and exclude.txt are read in its
      column. An exe that is not listed stops the lift.
      --prefix P names a second module's output (P + every file and table
      above, e.g. server_recomp_dispatch_table for server.dll) so it links
      next to SPEED2.EXE's; no SPEED2.EXE configuration applies to it.

Everything written here is derived from the game binary and is gitignored.
"""
import json, os, sys, time, collections

HERE = os.path.dirname(os.path.abspath(__file__))
PCRECOMP = os.environ.get("PCRECOMP", os.path.normpath(os.path.join(HERE, "..", "..", "pcrecomp")))
for sub in ("pe", "disasm", "lift"):
    sys.path.insert(0, os.path.join(PCRECOMP, "tools", sub))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32  # noqa: E402
from pe_analyze import analyze_pe, build_iat_map   # noqa: E402
import disasm32                                     # noqa: E402
from disasm32 import Disassembler, Function, BasicBlock, Instruction  # noqa: E402


def cmd_catalog(exe, seeds_path, out):
    info = analyze_pe(exe)
    iat = build_iat_map(info)
    data = open(exe, "rb").read()
    seeds = {info.image_base + info.entry_point_rva}
    forced = set()   # config/seeds-<variant>.txt: entries that must be in the catalog whatever decodes over them
    for e in json.load(open(seeds_path)):
        a = e.get("address") if isinstance(e, dict) else e
        a = int(a, 0) if isinstance(a, str) else a
        seeds.add(a)
        if isinstance(e, dict) and e.get("note") == "config seed":
            forced.add(a)
    dis = Disassembler(data, info.image_base, info.sections)
    t = time.time()
    funcs = dis.find_functions(info.code_start, info.code_end, iat, seeds=seeds,
                               release_operands=True)
    for a in sorted(forced - set(funcs)):
        # find_functions dropped it as "inside another instruction": the straddling body is a false
        # decode of data (a switch table before a function only a one-slot vtable names).
        f = dis.disassemble_function(a, iat)
        if f and f.blocks:
            f.entry_kind = "start"
            funcs[a] = f
            print(f"[*] forced config seed 0x{a:08X} ({f.size} bytes)")
        else:
            print(f"[*] WARNING: forced config seed 0x{a:08X} does not decode")
    print(f"[*] {len(funcs)} functions in {time.time() - t:.0f}s")
    doc = {"exe": os.path.basename(exe), "image_base": info.image_base,
           "code_start": info.code_start, "code_end": info.code_end,
           "functions": []}
    for f in sorted(funcs.values(), key=lambda f: f.address):
        doc["functions"].append({
            "address": f.address, "end": f.end, "size": f.size,
            "entry_kind": f.entry_kind, "is_thunk": f.is_thunk,
            "jump_targets": sorted(f.jump_targets),
            "calls_to": sorted(f.calls_to),
            "blocks": [[b.start, b.end, sorted(set(b.successors)), int(b.is_exit)]
                       for b in sorted(f.blocks.values(), key=lambda b: b.start)],
        })
    json.dump(doc, open(out, "w"))
    print(f"[*] wrote {out}")


def rebuild(md, img, base, fd):
    """A disasm32.Function with full capstone operands, from a catalog entry."""
    f = Function(address=fd["address"], end=fd["end"], size=fd["size"],
                 name=f"sub_{fd['address']:08X}", is_thunk=fd["is_thunk"],
                 entry_kind=fd["entry_kind"])
    f.jump_targets = set(fd["jump_targets"])
    f.calls_to = set(fd["calls_to"])
    for s, e, succ, ex in fd["blocks"]:
        b = BasicBlock(start=s, end=e, successors=list(succ), is_exit=bool(ex))
        for ins in md.disasm(img[s - base:e - base], s):
            b.instructions.append(Instruction(ins.address, ins.size, ins.mnemonic,
                                              ins.op_str, bytes(ins.bytes),
                                              list(ins.operands)))
        f.blocks[s] = b
    return f


sys.path.insert(0, HERE)
import exe_variant                                  # noqa: E402


def cmd_lift(exe, catalog, outdir, split=400, names_path=None, prefix=""):
    import pefile
    from lift32 import Lifter
    info = analyze_pe(exe)
    iat = build_iat_map(info)
    module = os.path.basename(exe)
    pe = pefile.PE(exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    cat = json.load(open(catalog))
    fds = cat["functions"]
    lifted = {fd["address"] for fd in fds}
    names = {}
    if names_path and os.path.exists(names_path):
        names = {int(k, 16): v for k, v in json.load(open(names_path)).items()}
    variant = None
    overrides, excluded, hooks = {}, {}, {}
    if not prefix:
        variant = exe_variant.detect(exe)
        if variant is None:
            sys.exit(f"{exe} is not a SPEED2.EXE listed in config/exe_variants.txt (its addresses for hooks, "
                     "overrides and native code are unknown); see docs/NFSU2_BINARY_REPORT.md")
        print(f"[*] SPEED2.EXE variant {variant['id']} ({variant['size']} bytes)")
        overrides = exe_variant.va_column("overrides.txt", variant)
        excluded = exe_variant.va_column("exclude.txt", variant)
        hooks = exe_variant.va_column("hooks.txt", variant)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    lifter = Lifter(iat_map=iat, func_names={}, lifted=lifted, precise_carry=True,
                    hook_sites=set(hooks))
    os.makedirs(outdir, exist_ok=True)
    for fn in os.listdir(outdir):
        if fn.startswith(prefix + "recomp_") and fn.endswith((".c", ".h")):
            os.remove(os.path.join(outdir, fn))

    stats = collections.Counter()
    unimpl = collections.Counter()
    unimpl_sites = collections.defaultdict(list)
    failed = []
    entries = []
    t0 = time.time()
    chunk, idx, lines_total = [], 0, 0

    def flush():
        nonlocal chunk, idx, lines_total
        if not chunk:
            return
        p = os.path.join(outdir, f"{prefix}recomp_{idx:04d}.c")
        with open(p, "w", newline="\n") as fh:
            fh.write(f"/* AUTO-GENERATED from {module} by run_lift.py (pcrecomp lift32). DO NOT EDIT, DO NOT COMMIT. */\n")
            fh.write(f'#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n#include "{prefix}recomp_funcs.h"\n#include <math.h>\n#include <string.h>\n\n')
            body = "\n\n".join(chunk)
            fh.write(body)
            lines_total += body.count("\n")
        idx += 1
        chunk = []

    for n, fd in enumerate(fds):
        a = fd["address"]
        try:
            if a in overrides:
                code = (f"/* OVERRIDE: {overrides[a]} */\n"
                        f"void sub_{a:08X}(void) {{ recomp_func_t _f = recomp_lookup_manual(0x{a:08X}u);"
                        f" if (_f) {{ _f(); return; }} RECOMP_UNIMPL(0x{a:08X}u, \"override missing\"); }}")
                stats["overridden"] += 1
            elif a in excluded:
                code = (f"/* EXCLUDED: {excluded[a]} */\n"
                        f"void sub_{a:08X}(void) {{ RECOMP_UNIMPL(0x{a:08X}u, \"excluded: not code\"); }}")
                stats["excluded"] += 1
            else:
                f = rebuild(md, img, base, fd)
                code = lifter.lift_function(f)
        except Exception as ex:  # noqa: BLE001
            failed.append((a, repr(ex)))
            code = (f"/* LIFT FAILED: {ex!r} */\n"
                    f"void sub_{a:08X}(void) {{ RECOMP_UNIMPL(0x{a:08X}u, \"lift failed\"); }}")
        for line in code.splitlines():
            if "RECOMP_UNIMPL(" in line:
                key = line.split('RECOMP_UNIMPL(', 1)[1].split('"')[1].split()[0] if '"' in line else "?"
                unimpl[key] += 1
                if len(unimpl_sites[key]) < 10:
                    unimpl_sites[key].append(f"{a:08X}")
            if "RECOMP_ICALL(" in line: stats["icall_sites"] += 1
            if "RECOMP_ITAIL(" in line: stats["itail_sites"] += 1
        if names.get(a):
            code = f"/* {names[a]} */\n" + code
        chunk.append(code)
        entries.append(a)
        if len(chunk) >= split:
            flush()
            el = time.time() - t0
            print(f"[*] {n + 1}/{len(fds)} lifted ({el:.0f}s)", flush=True)
    flush()

    with open(os.path.join(outdir, f"{prefix}recomp_funcs.h"), "w", newline="\n") as fh:
        fh.write("/* AUTO-GENERATED. DO NOT EDIT. */\n#pragma once\n#include <stdint.h>\n")
        for a in entries:
            fh.write(f"void sub_{a:08X}(void);\n")
    with open(os.path.join(outdir, f"{prefix}recomp_dispatch.c"), "w", newline="\n") as fh:
        fh.write(f'/* AUTO-GENERATED. DO NOT EDIT. */\n#include "recomp_types.h"\n#include "{prefix}recomp_funcs.h"\n\n')
        fh.write(f"const recomp_dispatch_entry_t {prefix}recomp_dispatch_table[] = {{\n")
        for a in sorted(entries):
            fh.write(f"    {{ 0x{a:08X}u, sub_{a:08X} }},\n")
        fh.write("};\n")
        fh.write(f"const uint32_t {prefix}recomp_dispatch_count = {len(entries)};\n")
    with open(os.path.join(outdir, f"{prefix}recomp_imports.c"), "w", newline="\n") as fh:
        fh.write("/* AUTO-GENERATED: IAT slot -> import name, for diagnostics. */\n#include <stdint.h>\n")
        fh.write("typedef struct { uint32_t slot; const char* name; } nfs_import_name_t;\n")
        fh.write(f"const nfs_import_name_t {prefix}nfs_import_names[] = {{\n")
        for slot, (dll, nm) in sorted(iat.items()):
            fh.write(f'    {{ 0x{slot:08X}u, "{dll}!{nm}" }},\n')
        fh.write("    { 0, 0 } };\n")
    if variant:
        exe_variant.write_gen_files(outdir, variant, exe)

    out = {"functions": len(entries), "files": idx, "c_lines": lines_total,
           "exe_variant": variant["id"] if variant else None,
           "lift_failures": len(failed), "failures": failed[:200],
           "unimplemented_total": sum(unimpl.values()),
           "unimplemented": dict(unimpl.most_common()),
           "unimplemented_sites": dict(unimpl_sites),
           "overridden": stats["overridden"], "excluded": stats["excluded"],
           "icall_sites": stats["icall_sites"], "itail_sites": stats["itail_sites"],
           "seconds": round(time.time() - t0)}
    json.dump(out, open(os.path.join(outdir, f"{prefix}lift_stats.json"), "w"), indent=1)
    print(json.dumps({k: v for k, v in out.items() if k not in ("failures", "unimplemented_sites")}, indent=1))


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "catalog":
        cmd_catalog(a[1], a[2], a[3])
    elif a and a[0] == "lift":
        split = int(a[a.index("--split") + 1]) if "--split" in a else 400
        names = a[a.index("--names") + 1] if "--names" in a else None
        prefix = a[a.index("--prefix") + 1] if "--prefix" in a else ""
        cmd_lift(a[1], a[2], a[3], split, names, prefix)
    else:
        sys.exit(__doc__)
