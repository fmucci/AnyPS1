# 18 - Goal: a migration an agent can run without a human

Status: **plan** (written 2026-10-23, after two ports). Tags: observed = done in one of the two ports; **planned** = not built yet.

## The goal
A person who knows nothing about emulation gives an agent (Claude Code or similar) this repository and a disc image they own. The agent produces a native build of that game and proves it correct **without the person playing it, answering questions or supplying hardware knowledge**. Everything the two ports needed a human for must therefore become either (a) documented knowledge here, (b) a tool here, or (c) an automated check here.

## Where the two ports stand (observed)
Game-independent code already exists in the ports and is mostly parameter-free (a count of game-specific references per file: boot glue, hook and overlay tables, packaging names, a few debug addresses; the recompiler, the C hardware model, the BIOS layer, the interpreter tier and the tests are generic). What is **not** yet here: that code. This repository holds facts, not the toolchain, so a newcomer's agent would have to re-derive the toolchain from prose.

## What a human did in the ports, and what replaces it
| human step | observed cost | replacement |
|---|---|---|
| choose how to boot / what the first fatal means | many iterations | fail-loud runtime + a **triage table** (symptom -> doc section) in the playbook (**planned**) |
| decide "does it look right" | looking at frames | differential run against an independent core on the same press script, pixel-match percentage with a threshold (observed: finding 0012, median 96 %) |
| play to reach new areas | hours | scripted route search with savestates in the reference core + public walkthrough text, then replay; random walker soak with fatal detection (observed: ff9 0013-0015). Needs the savestate host, the fork/branch runner and the route tools to be shipped (**planned**) |
| find a save point to test the memory card | a day of manual play | the same route search; the card images are exchanged with the reference core and compared structurally (observed: ff9 0015) |
| judge sound | listening | envelope/level correlation against the reference (observed: ff9 0011) |
| package for a platform | manual | `package` tool per host (observed: macOS; Windows via container in f2b) |
| supply the disc and legal context | user | the user supplies the image; nothing of the game is stored (observed rule) |

## Closing a phase without playing (acceptance by checks)
A phase is closed by a machine-checkable statement, not by someone seeing it work:
1. **Inventory** - disc extracted, executables listed, hashes recorded. Check: the tool's report exists and every boot file parses.
2. **Static knowledge** - every executable discovered; unresolved indirect jumps listed. Check: 0 unresolved after the interpreter-based bisection.
3. **Recompiler** - differential test against the spec on random fragments (0 mismatches) and whole-executable lock-step against the oracle for N frames.
4. **Host libraries** - every BIOS call the executables import is implemented or has a documented stub (the stub table is generated from the import list and must have no `fatal` rows reachable in the soak).
5. **Devices** - the public hardware test programs (16) pass, and each device is compared with the reference core on the game's own traffic (GPU frames by pixel match, SPU by envelope correlation, MDEC/CD by bit-exact output where defined).
6. **Whole game** - N random-input soaks of T ticks with no fatal, coverage report (which devices/BIOS calls/overlays were exercised) above a threshold; the save/load round trip with the reference card; packaging builds.
A coverage report is what replaces "I played it": it lists what was *not* exercised, so the final report states unknowns honestly instead of claiming completeness.

## Work list to reach the goal (**planned**, in this order)
1. `template/` in this repository: the generic runtime, recompiler, test harness and tools copied from the second port with the game-specific parts split out (`game/` = boot glue, hook table, overlay table, names). A script `init-game` fills them from the disc.
2. `PLAYBOOK.md` at the root, written for an agent with no context: prerequisites check, the ordered steps, the acceptance checks above, the triage table, the stop conditions ("ask the human" only for: missing disc, legal, a platform not available).
3. Ship the reference-host tooling as scripts that build the reference core from its public source on the user's machine (the core is never redistributed) and expose: run with press script, savestates, card enable, BIOS call log, RAM dump.
4. Route/soak tooling: press-script runner, branch runner (fork at a tick), walker, picture contact sheets, pixel-match, coverage report.
5. A dry run: a different game (not one of the two) migrated by an agent that only reads this repository; every question it had to ask becomes a documented answer or a tool.
Open: how much of the story coverage is needed for "done" differs per game; the coverage threshold is a judgement the playbook must state as a number with a reason.
