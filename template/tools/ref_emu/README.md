# Reference emulator harness (local, nothing of the emulator is committed)

Purpose: run the same disc in an **independent emulator** with scripted pad input, savestates and a BIOS-call log, and compare *what the program does* with our native build.
Build everything with `tools/ref_emu/setup_ref.sh` (clones PCSX-ReARMed into `work/emu/`, adds our debug instrumentation with `patch_core.py`, builds the core and our host `host.c`). The core's BIOS is a high-level re-implementation, not the console BIOS: treat its behaviour as *evidence*, not ground truth.

| tool | use |
|---|---|
| `host.c` -> `work/emu/host/host2` | `--frames N --press btn@lo-hi --dump frame:file.ppm --savestate F --loadstate F:FRAME --savecard F --loadcard F --bios LO:HI --pcs hex,.. --watch lo hi --poll addr --jal LO:HI --ramdump FRAME:F --audio F` ; env `REF_CARD1=1` enables card 1 (without it the core runs with no card) |
| `refrun.py`, `refstep.py` | run / resume a press script and write pictures + a state (`CARD_OUT`, `CARD_IN` env for card images) |
| `refwalk.py`, `refbatch.py` | random-walker branches in parallel from a state + a contact sheet |
| `sheet.py`, `refpick.py`, `ppmbest.c` | contact sheets (PNG), picture picking, best identical-pixel match between two sets of pictures |
| `refenv.py` | where things live (override with `REF_HOST_DIR`, `REF_CORE`) |
Procedure for reaching game states: AnyPS1 `docs/19-playing-with-guides.md`.
