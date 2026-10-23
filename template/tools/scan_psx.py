#!/usr/bin/env python3
"""Static inventory of BIOS call stubs, syscalls and hardware-register references in PS-X EXE files.

usage: scan_psx.py [--spx psx-spx.htm] EXE [EXE ...]      (run as: python3 -I tools/scan_psx.py ...)

Heuristic, linear scan -- NOT control-flow aware. What it does and does not claim:
  * BIOS stubs: exact 3-word pattern  li t2,{A0,B0,C0} ; jr t2 | jalr ra,t2 ; li t1,N (delay slot) -> observed bytes.
    A `jalr` stub is an inline call (the call site IS the stub); its caller count is not meaningful.
  * callers: count of `jal`/`j` words whose target is a stub  -> observed bytes (data that looks like a jal is counted).
  * syscall: words exactly equal to 0x0000000c (`syscall` with code 0), with the `li a0,N` found in the 4 preceding
              words (if any). Words without such a `li` are reported as unattributed and may be data (value 12).
  * hardware: (a) `lui rX,0x1f80` followed within WINDOW words by a load/store whose base is rX
              (address = 0x1f800000 + sign-extended offset); (b) aligned data words in 0x1f800000..0x1f802fff
              (pointer tables such as the CD register pointers). Misses indirect/computed accesses.
Names come from psx-spx (--spx); without it only numbers are printed.
"""
import re
import struct
import sys
from collections import defaultdict

WINDOW = 12
LOAD_OPS = {0x20: "lb", 0x21: "lh", 0x23: "lw", 0x24: "lbu", 0x25: "lhu"}
STORE_OPS = {0x28: "sb", 0x29: "sh", 0x2B: "sw"}


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def load_exe(path):
    d = open(path, "rb").read()
    assert d[:8] == b"PS-X EXE", path
    pc, gp, addr, size = struct.unpack("<IIII", d[0x10:0x20])
    return addr, pc, d[0x800:0x800 + size]


def load_names(spx):
    bios, hw = {}, defaultdict(list)
    if not spx:
        return bios, hw
    txt = open(spx, encoding="latin1").read()
    txt = re.sub(r"<[^>]*>", "", txt).replace("&gt;", ">").replace("&lt;", "<").replace("&amp;", "&")
    for line in txt.split("\n"):
        m = re.match(r"^  ([ABC])\(([0-9A-F]{2})h\)(?: or ([ABC])\(([0-9A-F]{2})h\))? (.*)$", line)
        if m:
            name = m.group(5).strip()
            bios.setdefault((m.group(1), int(m.group(2), 16)), name)
            if m.group(3):
                bios.setdefault((m.group(3), int(m.group(4), 16)), name)
        m = re.match(r"^  (1F80[0-9A-F]{4})h(?:\.\S+)?\s+\S+\s+(.*)$", line)
        if m:
            a = int(m.group(1), 16)
            nm = re.sub(r"^[-;\s]+", "", re.sub(r"\s+", " ", m.group(2)).strip())[:60]
            if nm not in hw[a]:
                hw[a].append(nm)
    return bios, hw


def scan(path, bios_names, hw_names):
    base, entry, text = load_exe(path)
    n = len(text) // 4
    w = struct.unpack("<%dI" % n, text[: n * 4])
    addr = lambda i: base + 4 * i

    def li(word, reg):  # ori/addiu reg,zero,imm -> imm or None
        op = word >> 26
        if op in (0x0D, 0x09) and ((word >> 21) & 31) == 0 and ((word >> 16) & 31) == reg:
            return word & 0xFFFF
        return None

    stubs = {}  # address -> (table, index)
    t2_loads = 0
    for i in range(n - 2):
        t = li(w[i], 10)
        if t in (0xA0, 0xB0, 0xC0):
            t2_loads += 1
            if w[i + 1] in (0x01400008, 0x0140F809):  # jr t2 | jalr ra,t2
                idx = li(w[i + 2], 9)
                if idx is not None:
                    stubs[addr(i)] = ("ABC"[(t - 0xA0) >> 4], idx)
    callers = defaultdict(int)
    for i in range(n):
        op = w[i] >> 26
        if op in (2, 3):
            tgt = (((w[i] & 0x3FFFFFF) << 2) | (base & 0xF0000000))
            if tgt in stubs:
                callers[tgt] += 1
    sys_calls = defaultdict(list)
    for i in range(n):
        if w[i] == 0x0000000C:
            code = None
            for k in range(1, 5):
                if i - k >= 0:
                    v = li(w[i - k], 4)
                    if v is not None:
                        code = v
                        break
            sys_calls[code].append(addr(i))
    hw = defaultdict(lambda: {"load": 0, "store": 0, "ptr_word": 0, "lui_only": 0})
    for i in range(n):
        wd = w[i]
        if (wd >> 26) == 0x0F and (wd & 0xFFFF) == 0x1F80:
            reg = (wd >> 16) & 31
            used = False
            for k in range(1, WINDOW + 1):
                if i + k >= n:
                    break
                x = w[i + k]
                op = x >> 26
                if op == 0x0F and ((x >> 16) & 31) == reg:
                    break
                if (op in LOAD_OPS or op in STORE_OPS) and ((x >> 21) & 31) == reg:
                    a = (0x1F800000 + sext16(x & 0xFFFF)) & 0xFFFFFFFF
                    hw[a]["load" if op in LOAD_OPS else "store"] += 1
                    used = True
                if op in (0x09, 0x0D) and ((x >> 21) & 31) == reg:
                    a = (0x1F800000 + (sext16(x & 0xFFFF) if op == 0x09 else (x & 0xFFFF))) & 0xFFFFFFFF
                    hw[a]["ptr_word"] += 0  # pointer built in a register; counted below as lui_only
                    hw[a]["lui_only"] += 1
                    used = True
            if not used:
                hw[0x1F800000]["lui_only"] += 0
    for i in range(n):
        if 0x1F800000 <= w[i] < 0x1F803000:
            hw[w[i]]["ptr_word"] += 1
    return base, entry, len(text), stubs, callers, t2_loads, sys_calls, hw


def main():
    args = sys.argv[1:]
    spx = None
    if args[:1] == ["--spx"]:
        spx, args = args[1], args[2:]
    bios_names, hw_names = load_names(spx)
    print("<!-- generated by tools/scan_psx.py; heuristic linear scan, see its docstring for limits -->")
    for path in args:
        base, entry, size, stubs, callers, t2, sysc, hw = scan(path, bios_names, hw_names)
        name = path.split("/")[-1]
        print("\n## %s  (load 0x%08x, entry 0x%08x, text 0x%x bytes)\n" % (name, base, entry, size))
        print("### BIOS call stubs (%d found; %d `li t2,0xA0/B0/C0` words in total)\n" % (len(stubs), t2))
        print("| table | fn | name (psx-spx) | stub address | `jal`/`j` words targeting it |")
        print("|-------|----|----------------|--------------|-----|")
        for a, (t, i) in sorted(stubs.items(), key=lambda kv: (kv[1], kv[0])):
            print("| %s | %02Xh | %s | 0x%08x | %d |" % (t, i, bios_names.get((t, i), "?"), a, callers.get(a, 0)))
        print("\n### `syscall` instructions\n")
        print("| a0 (nearest preceding `li a0`) | name | count | first address |")
        print("|-----|------|------:|---------------|")
        sysnames = {1: "EnterCriticalSection", 2: "ExitCriticalSection"}  # psx-spx SYS(01h)/SYS(02h)
        for code, lst in sorted(sysc.items(), key=lambda kv: (kv[0] is None, kv[0])):
            print("| %s | %s | %d | 0x%08x |" % ("?" if code is None else code, sysnames.get(code, "?"), len(lst), lst[0]))
        def rows(sel):
            for a in sorted(hw):
                h = hw[a]
                if not sel(a) or not (h["load"] or h["store"] or h["ptr_word"] or h["lui_only"]):
                    continue
                nm = " / ".join(hw_names.get(a, ["?"]))[:70]
                print("| 0x%08x | %s | %d | %d | %d | %d |" % (a, nm, h["load"], h["store"], h["lui_only"], h["ptr_word"]))
        head = "| address | name (psx-spx) | loads | stores | built ptr (lui+ori/addiu) | data words |\n|---------|----------------|------:|-------:|-------:|-----:|"
        print("\n### Scratchpad (0x1f800000..0x1f8003ff, 1 KiB fast RAM -- memory, not I/O)\n")
        print(head)
        rows(lambda a: 0x1F800000 <= a < 0x1F800400)
        print("\n### I/O registers (0x1f801000..0x1f802fff)\n")
        print(head)
        rows(lambda a: 0x1F801000 <= a < 0x1F803000)
        other = [a for a in hw if not (0x1F800000 <= a < 0x1F800400 or 0x1F801000 <= a < 0x1F803000)
                 and (hw[a]["load"] or hw[a]["store"] or hw[a]["ptr_word"] or hw[a]["lui_only"])]
        if other:
            print("\n### Other addresses reached via `lui 0x1f80` + offset (likely offset arithmetic, not registers)\n")
            print(head)
            rows(lambda a: a in other)


if __name__ == "__main__":
    main()
