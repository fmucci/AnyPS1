# 03 — Methodology (AnyPS5-style, applied to PS1)

Principle (from AnyPS5 `docs/dev/ARCHITECTURE.md`): **nothing is emulated.** Translate the title's code ahead of
time into native code; reimplement the platform libraries on the host; translate GPU work to a host graphics API;
fail loudly on anything unsupported. Below, each AnyPS5 stage is mapped to a PS1 equivalent. The right column is
the **plan**, not established fact: each item is "proposed" until a finding confirms it is feasible here.

| AnyPS5 stage | PS1 equivalent (proposed) | Status |
|--------------|---------------------------|--------|
| Relinker: ELF → host-native executable | Static recompiler: MIPS R3000 (MIPS-I, little-endian, delay slots, GTE coprocessor) → C (or LLVM IR) → host binary | not started |
| Imports bound by NID at load time | Calls to BIOS/SDK routines bound to host implementations by **address/signature** (PS1 has no NIDs) | UNKNOWN how binary calls them |
| System PRX libraries re-implemented | Re-implement BIOS A0/B0/C0 calls and the SDK libs actually linked (GPU, GTE, SPU, CD, pads, memcard) | UNKNOWN which are linked |
| Module loader for `sce_module/*` | Loader for `PS-X EXR` relocatable overlays and the exe chain | UNKNOWN format |
| AGC/PM4 → Vulkan + shader recompiler | GPU GP0/GP1 primitives, OT/DMA lists → host GL/Vulkan renderer (fixed function: no shaders to recompile) | not started |
| Audio / video | SPU ADPCM, XA-ADPCM streaming, MDEC FMV → host audio + video decoder | not started |
| Env-var diagnostics, fail-loudly | Same: `PS1_*` env vars; unsupported → abort with guest PC | to do at first code |
| Versioned content-addressed caches | Cache recompiled blocks / converted assets keyed by content hash + tool version | later |

## Phases (do not skip; each ends with findings committed)

**Phase 0 — Environment & inventory** (done: see `docs/findings/0001-bootstrap.md`).
Disc extracted, executables wrapped, REA opens the wrapped ELF (`tools/psx2elf.py EXE work/elf/main.elf`).

**Phase 1 — Static knowledge.** Answer Open questions in `00-CONTEXT.md`, in this order:
1. Executable chain & loader (Q1, Q2): follow the strings `credits.exe`/`menu.exe` and the `PS-X EXR` parsing code.
2. How the binary reaches hardware/BIOS (Q3–Q5): enumerate call targets and hardware-register accesses
   (`0x1f80xxxx`, `0xA0/0xB0/0xC0` BIOS jump tables). Produce an *inventory table*: address, callers count,
   proposed role, evidence class.
3. Data formats (Q6): one finding per format, validated on ≥2 samples, with a small independent parser in `tools/`.
Output: findings + `docs/inventory/*.md`. **No recompiler code yet.**

**Phase 2 — Recompiler prototype** on the smallest executable of the disc.
Goal: boots the translated binary to its first externally visible effect with every unsupported call failing loudly.
Decide C-emission vs LLVM only after Phase 1; write the decision in `docs/decisions/`.

**Phase 3 — Host libraries.** Implement only what the inventory proves is called, in the order the game needs them.
Each function: spec from cited docs + binary usage + a test or a trace comparison.

**Phase 4 — GPU/GTE/SPU/CD.** Renderer first on test scenes from the real game, then audio, then FMV.

**Phase 5 — Full game flow** (menu → level → credits), saves, input (SDL), packaging for macOS/Linux/Windows.

## Verification strategy (applies to every phase)
- **Differential evidence:** for any recompiled routine, compare against an independent oracle when one exists
  (e.g. interpret the original MIPS in a tiny reference interpreter and diff register/memory traces). If an
  oracle is used, it is a *test tool*, not part of the shipped port.
- Run with strict flags on (AnyPS5 equivalents: `*_STRICT`, sync variants) before trusting any result.
- A phase is "done" when its exit criteria are met **and** the evidence is cited — not when the code compiles.
