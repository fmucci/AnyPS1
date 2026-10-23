# Final report (copy to docs/report.md and fill in; every line must be backed by a command you ran)

## What runs
- Game / disc / boot executable: 
- Build command and result: 
- How to play it (player command, controls, where saves go): 

## How it was verified (acceptance checks, AnyPS1 docs/18)
| check | command | result |
|---|---|---|
| generic tests | `./tests/run_all.sh` | |
| BIOS imports | `python3 -I tools/bios_imports_check.py` | |
| hardware tests | `python3 -I tools/ps1_tests.py ...` | |
| reference comparison (frames / audio) | | |
| soaks (seeds x ticks, fatals) | | |
| save/load round trip | | |
| package | `python3 -I tools/package.py native` | |

## Coverage (what was exercised; paste `docs/coverage.md`)

## NOT exercised / unknown / judgement calls (be explicit: low_ram ranges, time base, overlays, anything inferred)

## How to continue (the first three things to do next)
