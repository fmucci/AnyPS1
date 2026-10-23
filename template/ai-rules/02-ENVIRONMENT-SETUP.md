# 02 — Environment setup

Toolchain (see the AnyPS1 `docs/02-recompiler.md` and the `docker/` images): Python 3, a C compiler (clang/gcc/MinGW-w64), SDL2 for the player, `7zz` (or `unar`)
to unpack the archives. Linux and Windows are verified in containers (`docker/linux/run.sh`, `docker/windows/run.sh`).

## From zero
1. `python3 -I tools/init_game.py /path/to/disc.cue --name <short> --title "<Title>"` (accepts .cue/.bin or an archive holding one): places the image under `og-files/`, extracts the ISO9660 files to `og-files/disc/`, writes `game/game.json`.
2. `python3 -I tools/recomp/find_hooks.py --write` (SDK functions by signature), `tools/ref_emu/setup_ref.sh` (reference emulator, built locally).
3. `./tests/run_all.sh` — recompiler and runtime tests (those that need the disc skip themselves when it is missing).

Nothing from `og-files/`, `work/`, `build/`, `dist/` is ever committed.
