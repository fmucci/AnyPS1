#!/usr/bin/env python3
"""Build the independent GTE reference used ONLY as a local test oracle (queue P4-1, finding 0024).
usage: python3 -I tools/gte_ref/build_ref.py        (needs network on first use, a C++20 compiler)
Downloads DuckStation's `gte.cpp` and the small headers it needs into work/refs/duckstation/ (git-ignored; the code is under a
no-derivatives licence and is never copied into this repository), strips the parts that depend on the rest of that emulator (free camera,
aspect-ratio hack, PGXP, state saving, tick accounting), compiles the remainder with a shim (tools/gte_ref/shim) into work/refs/libgteref.*
and prints the path. tests/gte/test_gte_ref.py uses it when it exists."""
import os
import re
import subprocess
import sys
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "work", "refs", "duckstation")
BASE = "https://raw.githubusercontent.com/stenzek/duckstation/master/"
FILES = ["src/core/gte.cpp", "src/core/gte_types.h", "src/common/bitfield.h", "src/common/bitutils.h", "src/common/types.h"]


def fetch():
    os.makedirs(OUT, exist_ok=True)
    for f in FILES:
        sub = "common" if f.startswith("src/common/") else ""
        os.makedirs(os.path.join(OUT, sub), exist_ok=True)
        p = os.path.join(OUT, sub, os.path.basename(f))
        if not os.path.exists(p):
            with urllib.request.urlopen(BASE + f, timeout=60) as r:
                open(p, "wb").write(r.read())


def strip(src):
    L = src.split("\n")
    # keep constants/helpers (after the Config struct), registers and commands; cut Reset/DoState/SetAspectRatio/freecam
    start = next(i for i, l in enumerate(L) if l.startswith("#define REGS"))
    body = "\n".join(L[start:])
    body = re.sub(r"static void ApplyFreecam\(s64& x, s64& y, s64& z\);\n", "", body)
    body = re.sub(r"  if \(s_config\.freecam_active\)\n    ApplyFreecam\(x, y, z\);\n", "", body)
    a = body.index("  s64 Sx;\n  switch (s_config.aspect_ratio)")
    b = body.index("  const s64 Sy = ")
    body = body[:a] + "  const s64 Sx = (s64(result) * s64(REGS.IR1) + s64(REGS.OFX));\n\n" + body[b:]
    body = re.sub(r"  if \(g_settings\.gpu_pgxp_enable\)\n    RTPS_PGXP\(V, shift, lm, x, y, z\);\n", "", body)
    # remove RTPS_PGXP and Execute_NCLIP_PGXP definitions
    a = body.index("NEVER_INLINE void GTE::RTPS_PGXP")
    b = body.index("ALWAYS_INLINE void GTE::Execute_RTPS(")
    body = body[:a] + body[b:]
    a = body.index("void GTE::Execute_NCLIP_PGXP")
    b = body.index("void GTE::Execute_AVSZ3")
    body = body[:a] + body[b:]
    a = body.index("void GTE::Reset()")
    b = body.index("u32 GTE::ReadRegister(")
    body = body[:a] + body[b:]
    body = re.sub(r"static void RTPS_PGXP\([^;]*\);\n", "", body)
    body = re.sub(r"static void Execute_NCLIP_PGXP\([^;]*\);\n", "", body)
    body = re.sub(r"      if \(g_settings\.gpu_pgxp_enable && g_settings\.gpu_pgxp_culling\)\n        Execute_NCLIP_PGXP\(inst\);\n      else\n        Execute_NCLIP\(inst\);\n", "      Execute_NCLIP(inst);\n", body)
    body = body[:body.index("GTE::InstructionImpl GTE::GetInstructionImpl(")]
    body = re.sub(r"\s*CPU::AddGTETicks\([0-9]+\);", "", body)
    return body


def main():
    fetch()
    src = open(os.path.join(OUT, "gte.cpp")).read()
    import shutil
    open(os.path.join(OUT, "types.h"), "w").write('#include "common/types.h"\n')
    pre = ('#include "shim.h"\n#include "gte_types.h"\nnamespace GTE {\nstatic constexpr s64 MAC0_MIN_VALUE = -(INT64_C(1) << 31);\n'
           'static constexpr s64 MAC0_MAX_VALUE = (INT64_C(1) << 31) - 1;\nstatic constexpr s64 MAC123_MIN_VALUE = -(INT64_C(1) << 43);\n'
           'static constexpr s64 MAC123_MAX_VALUE = (INT64_C(1) << 43) - 1;\nstatic constexpr s32 IR0_MIN_VALUE = 0x0000;\nstatic constexpr s32 IR0_MAX_VALUE = 0x1000;\n'
           'static constexpr s32 IR123_MIN_VALUE = -(INT64_C(1) << 15);\nstatic constexpr s32 IR123_MAX_VALUE = (INT64_C(1) << 15) - 1;\n'
           'u32 ReadRegister(u32 index);\nvoid WriteRegister(u32 index, u32 value);\nu32* GetRegisterPtr(u32 index);\nvoid ExecuteInstruction(u32 inst_bits);\n'
           'using InstructionImpl = void (*)(Instruction);\n')
    open(os.path.join(OUT, "gte_core.cpp"), "w").write(pre + strip(src))
    shim = os.path.join(ROOT, "tools", "gte_ref", "shim")
    ext = {"darwin": ".dylib", "win32": ".dll"}.get(sys.platform, ".so")
    out = os.path.join(ROOT, "work", "refs", "libgteref" + ext)
    cmd = ["c++", "-std=c++20", "-O1", "-shared", "-fPIC", "-w", "-I", shim, "-I", OUT, os.path.join(shim, "ref_api.cpp"), "-o", out]
    subprocess.run(cmd, check=True)
    print(out)


if __name__ == "__main__":
    main()
