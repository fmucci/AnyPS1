# 02 — Recompiler design notes

## Function discovery (observed on five real executables, 0 decode problems)
- Entry points come from: the executable's entry, `jal` targets, the gap after a `jr ra` + delay slot ("leaf gap": small leaf functions without
  prologue are invisible to analysers that look for stack frames), `lui/addiu(ori)` pairs that build an aligned code address when the target is
  plausible, and jump-table entries.
- **Jump tables**: recognise `lui+addiu` constant base, indexed with `sll 2` and `addu` **or `add`** (the signed add is used by real code, a
  recogniser that accepts only `addu` misses tables and the runtime later stops with "indirect call to X"), then `lw` and `jr`. Bounded by a
  preceding `sltiu` when present, otherwise a heuristic (aligned in-image words, at most 256). Table words must be excluded from code and their
  targets added as part of the function body.
- A function body can extend *below* its entry (the entry jumps back); the emitter takes the entry as a parameter and starts with a `goto`.
- Code reached through a table of function pointers in data is reachable only at run time; the indirect-call table (`rt_call`) must contain every
  function and abort on anything else, naming the address.

## Emission
- One C function per recompiled function, registers in a CPU struct (no caching), delay slots and load delay resolved statically where the
  successor is known, otherwise recorded as "unchecked" (a runtime trace must watch those sites).
- **Yield counting**: every taken backward direct branch / `j` decrements a counter; at zero the function calls the host (`rt_yield`). The oracle
  counts the same events so traces still line up.
- **setjmp-style resume points**: SDK interrupt layers restore a saved context whose PC is the instruction after a `jal setjmp`, in the middle of
  another function. The discovery finds `setjmp` by its exact first words and the emitter generates an extra entry (a copy of the function starting
  at that PC) per resume point.
- **Hooks** bind by address + masked signature (immediates and branch targets masked); a mismatch refuses to build.

## Self-modifying code
- Real programs patch their own instructions (observed: an `ori a0,zero,N` argument load toggled between two values around a library call).
  A scan that "only looked for constant-address stores" missed it; the divergence showed up as a difference between recompiled code (static)
  and the oracle (executes RAM).
- Handling: a patch table (address + the allowed word variants); the emitter turns such an instruction into a `switch` on the current memory
  word; the runtime keeps one bit per RAM word of translated code and **aborts on any store into it** unless the exact word is a registered
  patch value; the oracle invalidates its decode cache on stores to code. Re-run the finder after every milestone.

## Overlays (code loaded at run time)
- Format and relocation: see 03. The program's own relocator is the specification: run it (in the oracle) on the image at a chosen base and
  translate the result offline.
- The base is whatever the guest allocator returns, so the build needs one generated module per observed base. The runtime registers a module
  **lazily**: the first indirect call into its address range triggers a hash check of every translated word against RAM; a mismatch is fatal.
- When the program frees and reuses the memory, the first store into the region must *unregister* the module (instead of tripping the
  self-modifying-code guard); a reload is verified again.

## Virtual time
See 05.

Origin: f2b#0017–0023, 0027.

## Portability across hosts (observed)
The runtime and the hardware model are plain C (standard library plus `setjmp`), so the same sources built with Apple clang (arm64), GCC (aarch64 Linux) and MinGW-w64 (x86-64 Windows, under Wine) produced a **byte-identical frame after tens of thousands of virtual frames**. Virtual time (no wall clock in the machine) and a single C hardware model are what make this possible; a cross-host hash comparison is a cheap determinism test. Things that did differ between hosts: GCC enabled `-Wimplicit-fallthrough` where clang did not (write `/* fall through */` before the next case label); test code must not rely on POSIX-only calls (`mkstemp`, `/tmp`, `unistd.h`); Wine's package installs `wine64` under a library directory rather than a `wine` command. Keep compiler, executable suffix, shared-library extension and SDL flags in one host-environment module.

## Callbacks that discovery cannot see (observed)
Function discovery that only accepts a pointer target with a standard prologue or right after a `jr ra` misses callbacks whose code starts differently (a function placed after data words, or compiled without a frame setup). Such a callback is reached only through the BIOS (a vblank event handler, an interrupt callback), so a missing function shows up as "indirect call: no generated function" **only after the routine that registers it actually runs** — and as silent loss of the feature while that routine is stubbed. Two safeguards: (1) give discovery an explicit list of extra entry points with the evidence for each, and (2) run a lister of code addresses built by `lui`+`addiu|ori` or stored as data words that resolve to code nobody emitted (relaxed rule without the prologue requirement, jump tables excluded: they decode as zero-base loads) and require it to report nothing. Early stubs of library start-up routines should be re-audited when the layer they bypassed exists.


## Discovery on a large monolithic executable (observed, second title)
A 1.9 MB program that fills almost all of RAM exposed four weaknesses of the discovery/emitter, all fixed:
1. **Zero-filled regions are not code.** Calls into memory that a later overlay occupies (zero bytes in the executable file, i.e. runs of `nop`) were followed instruction by instruction until the next non-code word: tens of thousands of nops per entry, thousands of entries, a discovery that took 15 min per pass (6 passes with automatic roots, more than an hour). Treating a run of more than 64 consecutive zero words as "not code" (the entry becomes a stub that aborts when called) brought one pass to 1.5 s. Real code never has 64 consecutive nops.
2. **Entry that is a branch delay slot.** Hand-written assembly with shared tails can be entered at an instruction that is the delay slot of the previous branch. The emitter needs a slot label for it (the entry jumps to the slot label, then falls into the next instruction); otherwise the generated C `goto`s a label that does not exist.
3. **Function pointers built across a call**: `lui a0,hi; jal f; addiu a0,a0,lo` (the delay slot completes the address passed to `f`) is a very common way to pass a callback; a pointer scan that only looks at `lui` immediately followed by `addiu` misses it.
4. **A function that follows a padding `nop` right after another function's `jr ra`+delay slot** is "covered" by the previous function's body when discovery descends through the padding; pointer candidates inside covered bodies must still be accepted when the previous instructions are `jr ra`, its delay slot (and optional padding nops).
Also needed: a stub function must still be **declared** before the direct calls to it (calls to not-recompiled functions are emitted as direct calls). Longer-term answer for callbacks and overlays that no static scan can name: an interpreter tier for addresses without a generated function (see 14).

## Unresolved `jr` is either a tail call or a return through a saved register (observed, second title)
Hand-written assembly (script/VM dispatchers, graphics primitives) often does `move s3,ra` at entry and `jr s3` at exit. A recompiler that emits every unresolved `jr rX` (X != ra, no recognised jump table) as "call the target, then return" turns this return into a call of the caller's continuation: the continuation then runs once inside the call and once more when the generated caller resumes, which shows up as corrupted lists/stacks far from the cause. Emit it as `if (target == return_address_at_entry) return; else tail-call(target)`, keeping the entry `ra` in a local. A whole-program **interpreter tier** (see 14) makes such bugs findable: run the same scenario fully interpreted (spec-verified, about 2x slower than the translated build) and bisect which translated function range reproduces the failure (`tools/bisect_codegen.py` style: translate only [lo,hi), interpret the rest).
