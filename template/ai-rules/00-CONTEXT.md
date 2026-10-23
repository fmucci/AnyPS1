# 00 — Context

## What this project is
Port one PlayStation 1 game (named in `game/game.json`) to run **natively on macOS, Linux and Windows** by **static recompilation** (MIPS R3000A -> C) plus a C hardware/BIOS runtime:
oracle interpreter vs recompiled C in lock-step, one shared C hardware model, fail-loud on anything unproven, virtual time, no game data in git.
The platform knowledge (CPU, BIOS, devices, SDK habits, pitfalls) lives in the AnyPS1 repository (`docs/`); the step-by-step procedure for an agent is its `PLAYBOOK.md`. This repository is an instance of AnyPS1's `template/`.

## Inputs (never committed)
Filled in by `tools/init_game.py` (it prints them): the disc image (`og-files/<name>-d1.bin`, raw MODE2/2352), the extracted ISO9660 files (`og-files/disc/`), the boot executable named in `SYSTEM.CNF`. Record the facts you establish (sizes, entry point, SDK libraries seen, overlays) in `docs/findings/` with how to re-check them.

## Open questions (see docs/port-queue.md for the live list)
Which SDK libraries the executables link, whether more code is loaded at run time (overlays), how a multi-disc title detects the disc change. Add the title's own open questions here.
