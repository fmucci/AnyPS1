# 22 - Finding debug code left in a release

Method used on the first two titles (FF9 finding 0024, f2b finding 0049). Evidence tags as in 00.

1. **Strings first.** `strings -n 5 EXE | grep -iE 'debug|cheat|level ?select|warp|god|invinc|test|assert|\.c$|f:/'`. Retail Sony-SDK programs always carry library chatter (`ResetGraph:`, `SetGraphDebug`, `GPU timeout`); that is the SDK, not the game. Words like `NO CLIP` can be death-reason texts: read the neighbours.
2. **Code that lives in an archive (FF9.IMG style).** Find code sectors (>=2 stack-frame prologues and >=2 `jr ra`), group runs, then find each file's RAM base by testing which sector-aligned base makes the `jal` targets land on prologues. The first pointer table of an overlay (words 0x800axxxx) confirms the base.
3. **String -> function.** Resolve `lui` + `addiu/ori` pairs that load the string address (the `lui` can be 5-10 instructions earlier, the `addiu` often sits in a `jal` delay slot; do not stop at the first `addiu` of that register, stop when the register is reloaded). Map each reference to the enclosing prologue.
4. **Referencers.** For each such function count `jal` callers and stored addresses (words equal to it) over all overlays and the boot executable. Zero of both = *orphan* candidate (the debug caller was cut in the release). Functions with callers but with debug `printf` text inside (format ending in `\n`) are live code whose output goes to the console TTY, invisible on a retail console.
5. **Verify** by calling the orphan from the runner at a tick where its overlay is loaded and looking at the picture/TTY. Statically this stays INFERRED: an address built at run time (`lui/addiu` in code you did not scan) can still reach it.

Pitfalls: Python `(v>>21)&31==rt` binds `==` first (wrap the shift in parentheses); do not require a NUL after the string (debug texts end in `\n`); strings and the code that prints them can be in different parts of the same overlay file, so look up addresses over the whole file, not per code run.
Tool: `tools/overlay_orphans.py` of the FF9 port (copy and set the overlay table).
