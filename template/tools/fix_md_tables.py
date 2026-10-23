#!/usr/bin/env python3
"""Removes blank lines that split a Markdown table in two (GitHub then renders the rest of the rows as plain paragraphs).
usage: python3 -I tools/fix_md_tables.py [--check] FILE...     --check only reports (exit 1 when something is broken)
A blank line between two table rows is removed unless the following row is a header (the row after it is a |---| separator), i.e. a new table starts."""
import re
import sys

SEP = re.compile(r"^\|?\s*:?-{2,}")


def fix(text):
    lines = text.split("\n")
    out, fixed = [], 0
    for i, l in enumerate(lines):
        if l.strip() == "" and out and out[-1].startswith("|"):
            j = i + 1
            while j < len(lines) and lines[j].strip() == "":
                j += 1
            if j < len(lines) and lines[j].startswith("|") and not (j + 1 < len(lines) and SEP.match(lines[j + 1])):
                fixed += 1
                continue
        out.append(l)
    return "\n".join(out), fixed


def main():
    check = "--check" in sys.argv
    bad = 0
    for p in [a for a in sys.argv[1:] if not a.startswith("--")]:
        s = open(p, encoding="utf-8").read()
        n, k = fix(s)
        if k:
            print("%s: %d blank line(s) inside a table" % (p, k))
            bad += 1
            if not check:
                open(p, "w", encoding="utf-8").write(n)
    sys.exit(1 if (bad and check) else 0)


main()
