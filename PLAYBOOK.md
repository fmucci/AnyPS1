# PLAYBOOK - migrate a PlayStation 1 game to a native build, as an agent, without a human

You are an AI coding agent. A person gave you this repository and told you which PS1 game to migrate. They may not know anything about emulation. **Do the whole job yourself.** Ask the person only in the three cases of section 8.

This repository has two parts: `docs/` (what is known about the platform, tagged observed / inferred / unknown / documented) and `template/` (the toolchain: recompiler, runtime, tests, reference-emulator harness, player). You never edit this repository to migrate a game; you **copy `template/` into a new repository** and work there.

## 0. Rules that never bend (they exist because they were learned the hard way)
1. **No game data in git. Ever.** Disc images, extracted files, frames, audio, dumps, and the game's addresses/symbols stay in git-ignored `og-files/`, `work/`, `build/`, `dist/` (the template's `.gitignore` does it). Only the game's own repository may hold `game/hooks.csv`/`roots.csv` (they contain addresses). Never push those to AnyPS1.
2. **Fail loudly.** An unproven access, an unimplemented BIOS call or an unknown hardware read must stop the run with a message; never return a made-up value silently. A "stub" is a documented, evidence-backed exception (`docs/stubs.md`, checked by `tools/check_stubs.py`).
3. **Tag every claim**: observed (say how to re-check), inferred, unknown, documented. Write findings in `docs/findings/NNNN-title.md` (template: `ai-rules/07-FINDINGS-TEMPLATE.md`).
4. **Commit small, with the tests green** (`./tests/run_all.sh`), and keep the README progress block, `docs/port-queue.md`, `docs/stubs.md` and the finding in the same commit as the change.
5. Run long jobs under `caffeinate` (macOS) / `nohup` and poll; never block a session for 10+ minutes on one command.
6. The user's disc is theirs. Never upload it, never send frames/audio of the game anywhere. Reference-emulator sources are cloned locally and never committed.

## 1. Prerequisites (check, then install what is missing; do not ask)
`python3` (3.9+), a C compiler (`cc`/`clang`/`gcc`), `git`, `make`, `7zz` or `7z` (only if the disc is an archive). Optional: SDL2 (player), Docker (Windows/Linux packages). If something is missing and you can install it (package manager, no root needed), do; else tell the person the exact command.
The disc must be a **raw MODE2/2352 `.bin` (+ `.cue`)**, single data track. A cooked 2048-byte `.iso` cannot be used (the XA audio and video sectors are missing): tell the person to dump or convert it.

## 2. Create the game repository
```
cp -R <this repo>/template ~/work/<game>            # then: cd ~/work/<game> && git init && git add -A && git commit -m "template"
python3 -I tools/init_game.py /path/to/disc.cue --name <short> --title "<Game Title>"
python3 -I tools/recomp/find_hooks.py --write       # SDK functions recognised by signature -> game/hooks.csv
tools/ref_emu/setup_ref.sh                          # reference emulator + host, built locally (about 5-10 min)
./tests/run_all.sh                                  # the generic tests must pass before you start
```
`init_game.py` writes `game/game.json` (name, executables, boot file names) and extracts the disc into `og-files/disc`. For a multi-disc title add the other discs' boot names to `boot_names`. If the boot disc lists more executables that are loaded later (look in the extracted files for `*.EXE`, `*.EXR`, overlays) add them to `exes`/`overlays` when a run asks for them, not before.

## 3. First build and the bring-up loop
```
python3 -I tools/recomp/build_game.py -O1                              # translate + compile the whole game: build/ps1_game
build/ps1_game og-files/<name>-d1.bin --ticks 3000 --card /tmp/c.mcr --ppm /tmp/f.ppm
```
Before anything else calibrate the time base: `python3 -I tools/calibrate_time.py --ticks 6000` and put the chosen value in `game/game.json` as `yield_period` (two ports needed 4000 and 40000; a too-large value shows as `CD timeout` messages). A run that ends with `FATAL: tick limit reached` is a success of that smoke test. Any other fatal is the next work item. Loop: read the message -> triage table (section 4) -> fix in `runtime/` (C, primary) -> rebuild -> rerun -> add a unit test or finding -> commit. After each step look at the picture: convert it with `python3 -I tools/ref_emu/sheet.py /tmp/f.png 1 1 /tmp/f.ppm` and open the PNG with your image tool.
Debug aids (env vars): `PS1_BIOSLOG`, `PS1_BIOSFROM=tick` (every BIOS call from a tick), `PS1_EVLOG`, `PS1_CDLOG`, `PS1_DMALOG`, `PS1_MDECLOG`, `PS1_SPUKON`, `PS1_SIOLOG`, `PS1_INTERPLOG`, `PS1_PCLOG=addr,..`, `PS1_WATCH`, `PS1_RAMDUMP=file`; `--calltrace` build + `PS1_RING=n` prints the last n function entries at a fatal. Disassemble with `python3 -I tools/dis.py ram:<ramdump> START END` (code that exists only after a load) or `tools/dis.py EXE START END`.

## 4. Triage table (symptom -> where to look)
| symptom | cause class | read / do |
|---|---|---|
| `BIOS X(NN) is not implemented` | missing BIOS call | psx-spx kernel tables; AnyPS1 `docs/04-bios.md`; implement in `runtime/hle.c`/`biosc.inc`, register in `docs/stubs.md` |
| `store/load at 0000xxxx: first 64 KiB of RAM belongs to the BIOS` | program uses a kernel area (a stack placed below 0x10000 shows as a store just below the initial SP at a function prologue: allow that 4 KiB range) | `docs/04-bios.md` (null-pointer reads, kernel tables): find the pc and the pointer's origin; allow exactly that range with its evidence in `game/game.json` `low_ram`; if a library walks a kernel table, seed it (see the device table in `runtime/game_boot.inc`) |
| `misaligned load/store`, wrong jump | translation bug or data run as code | `tools/bisect_codegen.py` (interpreter tier as reference), `docs/01-cpu.md`, `docs/02-recompiler.md`; add the case to `game/roots.csv` when a callback is invisible |
| `CD timeout: CD_read ...` in the output | time base too long for the SDK's own timeout | `tools/calibrate_time.py`; docs 05 |
| `DoExecute: no recompiled module for X.EXE` | a launcher loads another executable by name | add `X.EXE` to `exes` in `game/game.json` (first entry = boot file), rebuild; docs 21 |
| `WaitEvent ... never became ready`, `CdSync` hangs | event/interrupt not delivered | `docs/04`, `docs/05`; compare BIOS call sequences with the reference (`--bios LO:HI` in the host, `PS1_BIOSFROM`) |
| black screen after a script event | program polls an event/status we answer differently | same comparison; `docs/11` (card events), `docs/13` item list "silent freezes are status-bit waits" |
| movie freezes or runs slow | MDEC/DMA stall semantics | `docs/09-mdec.md` |
| sound too loud/silent/missing | SPU level/ADSR/XA | `docs/10-spu.md`, then envelope correlation with the reference (`PS1_SPUKON`) |
| controller does nothing | pad driver talks to SIO0 | `docs/17-controller-port.md` |
| save/load fails | card events, timing, kernel tables | `docs/11-memory-card.md` |
| picture differs from the reference in details | GPU rules | `docs/06-gpu.md`, `tools/gpu_conf`, pixel match with `tools/ref_emu/ppmbest.c` |
| crash in generated code only at -O2 | undefined behaviour in a model | build with `-fsanitize=undefined`, `docs/13` |
Unknown message: search `docs/` for the exact text first (`grep -rn`), then the reference BIOS source in `work/emu/pcsx_rearmed/libpcsxcore/psxbios.c`, then psx-spx (`docs/16`).

Self-check of a fresh checkout without any commercial disc: `python3 -I tools/selftest_public.py <ps1-tests-bin dir> --ticks 3000` (needs the public `ps1-tests` release under work/refs; `tools/mkdisc.py` wraps any PS-X EXE into a disc). Expect 43 of 48 ok and the 5 documented stops of AnyPS1 `docs/20`.

## 5. Verify without a human (acceptance, from `docs/18-autonomous-migration.md`)
Do not stop at "it boots". Close each phase with its machine-checkable statement and write the result in the README progress block:
1. disc inventory and boot file parse (init_game output) 2. every executable discovered, `unresolved_pointers.py --check` clean 3. `tests/run_all.sh` green (differential tests, mutation check) 4. every BIOS call the executables import is implemented or a registered stub (`tools/scan_psx.py EXE`) 5. public hardware tests pass (`tools/ps1_tests.py`, see `docs/16`) and the game's own traffic matches the reference (frames by pixel match, SPU by envelope correlation) 6. soaks and routes (section 6) with a coverage report 7. save/load round trip with the reference card (docs 11 method) 8. a package builds (`tools/package.py native`).

## 6. Reach game states without a person: `docs/19-playing-with-guides.md`
Use the reference emulator's savestates, a public walkthrough read with `WebSearch`/`WebFetch`, picture reading, and `run_game --branch`. Goals, in order: (1) title -> new game -> first controllable screen; (2) first battle; (3) first save point and a save + load round trip; (4) one of each device-heavy scene (movie, XA music, world map if any); (5) random-walker soaks from the save (`tools/route/`), 8 seeds x 400,000 ticks, zero fatals. Write what each reached state needs as a press list in `docs/` of the game repository so it can be replayed.
Measure coverage: build once with `python3 -I tools/recomp/build_game.py -O1 --coverage` (PS1_GAME_OUT=ps1_game_cov keeps the normal build), run each scenario with `PS1_COVERAGE=run1.json`, then `python3 -I tools/coverage_report.py run*.json --funcs build/game/gen_<prefix>.funcs --md docs/coverage.md`. The report lists device features, CD commands, DMA channels, IRQ sources, BIOS calls and the percentage of recompiled functions entered, and **what was not exercised**: copy that list into the final report.

## 7. Give back (so the next game is easier)
Platform facts (not the game's): port them into AnyPS1 `docs/` (observed tag, how to re-check) and add new SDK function signatures to `data/sdk_signatures.csv`. Game-specific facts stay in the game repository.

## 8. When to ask the person (only these)
1. The disc image is missing, not raw MODE2/2352, or not the disc they own. 2. A legal question (sharing builds, publishing anything of the game). 3. A platform you cannot build for here (e.g. Windows without Docker): say what you need.
Everything else - which BIOS call to write, what a value means, where to click in a menu, whether a picture looks right - you decide with evidence and record.

## 9. Stop conditions and the final report
Stop when sections 5-6 are green or when the same fatal has resisted three different evidence-based attempts (then write the finding with the unknowns and what you tried, and stop). Final report to the person: what runs, what was verified and how, what was not exercised, how to play it (`tools/package.py native`, then the player), where the saves are. No claims you did not check.
