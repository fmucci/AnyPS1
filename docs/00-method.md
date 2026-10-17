# 00 — Method

## Pipeline (what worked)
1. **Static knowledge first.** Identify the executable format, the SDK libraries linked in, which hardware each executable touches
   (scan raw words for COP2, I/O constants, BIOS call stubs). Nothing is assumed about "typical PS1 behaviour": every claim about a
   binary is checked in the binary.
2. **Decoder + executable spec + C emitter.** A decoder for the full MIPS-I user set + COP0/COP2, an *executable specification* written in
   a different style from the emitter (Python integers, byte-wise unaligned accesses, dynamic load-delay model), and an emitter that
   produces C. The three are cross-checked (see 01-cpu).
3. **Oracle.** A slow interpreter built from the spec runs the original executable. The recompiled C and the oracle run in lock-step and
   write the same *event stream*: every store (pc, address, size, value), every HLE call with its argument registers, every interrupt
   delivery. The first differing event is the bug. Final-state hashes (hardware state incl. VRAM/SPU RAM, RAM, console output) are compared too.
4. **One hardware model, two executors.** Device models are written once in C and used by both the runtime and (through `ctypes`) the
   oracle, so a device can never be "implemented twice differently". Divergence can only come from translated code or the BIOS layer.
5. **Fail loud.** Everything not proven used by the program aborts with a message naming the access (unmapped region, unknown register,
   unimplemented BIOS call, unknown GP0 opcode, store into translated code). Each abort that turns out to be legitimate gets a note and is
   then enabled. This converts "the port misbehaves somewhere" into "the port stops exactly here".
6. **Library layers run unmodified.** Wherever possible the SDK's own code (interrupt layer, VSync, graphics, CD, streaming, SPU) runs on the
   hardware model; HLE hooks are a last resort, bound by a *masked signature* of the function's first instruction words (so a hook can never
   attach to the wrong function) and listed in a stub registry that a test keeps in sync.
7. **Virtual time.** No wall clock inside the emulated machine (see 05). Determinism is what makes differential testing possible.

## Evidence rules
- Tag: **observed** (reproducible command or evidence), **inferred** (reasoning), **unknown**.
- A result nobody else can regenerate is not a result.
- When an independent implementation exists (another emulator's source), read it to check *behaviour*, record what it does, and keep the
  verdict: confirmed / differs / no independent source. Never copy its code.
- When the documentation itself says "approximate", do not trust the approximation: compare against an independent implementation on random
  inputs (this changed the MDEC, see 09).

## Debug aids that paid off (all cost nothing when unused)
Store watch on one address (prints pc/ra/tick), function-entry breakpoint with a ring of the last N entered functions, "stop when the
console output contains a text", guest-memory dump after the run, per-device logs (CD commands/sectors, DMA list nodes, event waits, GPU
environment values), scripted pad input by virtual frame, snapshot of the executable hand-off block at every exec. The call ring plus a
store watch resolved most "why is this value zero" questions in minutes.

## Test layers
Unit tests for each device with hand-derived expectations; differential tests (C vs spec on random fragments and states, C vs oracle on
whole executables); a mutation check (deliberately broken rules must be detected); a reference-implementation check per device where one exists; a
whole-program smoke test with scripted input.
