# template/ - the game-independent toolchain

Copy this directory to start a migration (see ../PLAYBOOK.md). Contents: `runtime/` (C hardware model, HLE BIOS, memory card, interpreter tier), `tools/recomp/` (recompiler, oracle, hooks, build), `tools/ref_emu/` (reference-emulator harness), `tools/route/` (route/soak helpers), `tests/`, `frontend/` (SDL2 player), `docker/` (Windows/Linux builds), `ai-rules/` (agent rules), `data/sdk_signatures.csv` (SDK functions recognised by signature), `game/` (the only place that names a title; filled by `tools/init_game.py`).
Verified to build and run identically to the second port (same frame, same tests) when instantiated with that game's `game/` files.
