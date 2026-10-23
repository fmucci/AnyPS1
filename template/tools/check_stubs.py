#!/usr/bin/env python3
"""Enforce the stub registry (queue P3-4, ai-rules/01 rule 3): every HLE hook name, every implemented BIOS call and every provisional
hardware placeholder must be mentioned in docs/stubs.md. usage: python3 -I tools/check_stubs.py   (exit 1 and a list when something is missing)"""
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, ROOT)
from tools.recomp import hooks  # noqa: E402

REGISTRY = open(os.path.join(ROOT, "docs", "stubs.md")).read().lower()
names = set()
for line in open(hooks.HOOKS_CSV).read().splitlines()[1:]:
    names.add(line.split(",")[2])
names |= set(hooks.Hle.BIOS_NAMES.values())
names |= {"io.c", "gte.c", "card.c"}          # hardware placeholders are listed by file
missing = sorted(n for n in names if n.lower() not in REGISTRY)
if missing:
    print("NOT REGISTERED in docs/stubs.md:", ", ".join(missing))
    sys.exit(1)
print("stub registry covers %d hooks / BIOS calls / placeholders" % len(names))
