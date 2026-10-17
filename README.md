# AnyPS1

A knowledge base for **static recompilation of PlayStation 1 software**, written in the spirit of
[AnyPS5](https://github.com/boykopovar/AnyPS5): everything that was learned about the *platform* (CPU, BIOS, hardware, SDK conventions,
tooling method) while porting one commercial title, with **nothing that belongs to the title**.

## What is in here
Platform facts only: instruction semantics, how to translate and test them, the BIOS/hardware surface a recompiled program needs,
the register-level behaviour of each device, SDK-library conventions seen across executables, the pitfalls that cost time, and the
questions that are still open. Every statement is tagged **observed** (with how to re-check it), **inferred** (reasoning, not verified) or
**unknown**.

## What is deliberately not in here
- No game data, binaries, disc contents, screenshots, VRAM/audio dumps, file names or directory listings of any title.
- No game addresses, symbol names, strings or structure layouts (addresses that appear are hardware registers or documented BIOS vectors).
- No third-party source code. Reference emulators were only *read* to check behaviour; what is written here is our own description.
  (DuckStation's licence forbids derivatives; psx-spx is the primary public documentation and is cited, not copied.)

## Index
| doc | topic |
|-----|-------|
| [00-method](docs/00-method.md) | the porting method: oracle, differential tests, fail-loud policy, evidence tags |
| [01-cpu](docs/01-cpu.md) | R3000A semantics a translator must get right (delay slots, load delay, HI/LO, exceptions) |
| [02-recompiler](docs/02-recompiler.md) | function discovery, jump tables, resume points, self-modifying code, overlays, virtual time |
| [03-memory-and-formats](docs/03-memory-and-formats.md) | memory map policy, PS-X EXE / EXR headers, launcher chains |
| [04-bios](docs/04-bios.md) | BIOS A/B/C calls, events, exception exit, Exec, critical sections, pad |
| [05-irq-dma-timing](docs/05-irq-dma-timing.md) | interrupts, DMA control, virtual time, timers |
| [06-gpu](docs/06-gpu.md) | GP0/GP1 decoding, rendering approximations, display set-up |
| [07-gte](docs/07-gte.md) | geometry engine implementation and verification |
| [08-cdrom](docs/08-cdrom.md) | CD controller model, sector formats, streaming |
| [09-mdec](docs/09-mdec.md) | movie decoder: exact algorithm choices |
| [10-spu](docs/10-spu.md) | sound unit model, ADPCM, ADSR, reverb, XA |
| [11-memory-card](docs/11-memory-card.md) | card format and BIOS file layer |
| [12-sdk-conventions](docs/12-sdk-conventions.md) | Psy-Q library patterns visible in binaries |
| [13-pitfalls](docs/13-pitfalls.md) | things that failed before |
| [14-open-questions](docs/14-open-questions.md) | what is still unknown on the platform side |
| [15-sources](docs/15-sources.md) | references and how they were used |
| [CHANGELOG](CHANGELOG.md) | what was added when |

## How to update
When a porting session learns something about the platform: add or correct the statement in the matching doc, tag it, add a line to
`CHANGELOG.md`, commit. Game-specific consequences stay in the game's own repository. If a fact cannot be separated from the title,
describe the *mechanism* and drop the names.

Origin: extracted from the working findings of a native port (private repo "f2b", findings 0017–0029); finding numbers are cited as
`f2b#NNNN` only so the author can trace a statement back.
