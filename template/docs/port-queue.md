# Port queue

One row per piece of work; the status column is the only place progress is recorded. Update it in the commit that changes it (README progress block too).

| id | what | status | notes |
|---|---|---|---|
| Q0-1 | `tools/init_game.py DISC` configures the instance (game/game.json, extracted files) | todo | |
| Q1-1 | Boot executable runs in the oracle up to the first missing piece, piece by piece (BIOS calls, kernel patches, hardware) | todo | |
| Q1-2 | Recompile the boot executable to C and run it (`tools/recomp/build_game.py`) | todo | |
| Q2-1 | Pad, memory card | todo | |
| Q2-2 | CD streaming, FMV (STR + MDEC), XA audio | todo | |
| Q3-1 | Acceptance checks of each phase (see AnyPS1 docs/18) | todo | |
