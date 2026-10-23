# 07 — Findings template

Copy to `docs/findings/NNNN-short-title.md`. Keep it short and falsifiable.

```markdown
# NNNN — <title>

- Date: YYYY-MM-DD
- Author: <human/agent + model>
- Subsystem: <loader | fmt/lev | ...>
- Target: <file> sha256 <hash>   (e.g. work/elf/fb2.elf)
- Tools & versions: <Ghidra 12.1.4, rea-agents 6.3.0, tools/psx2elf.py @ <git sha>>
- Supersedes / Superseded-by: <none | NNNN>

## Question
<the exact Open question this answers>

## Observed (each line cites evidence)
- <fact>  — evidence: <REA evidence_id | file:offset | command>
## Inferred (each line lists the observations it rests on and how to falsify it)
- <inference>  — rests on: <...>; falsified if: <...>
## Unknown / not checked (explicit search boundary)
- <what you did NOT look at>

## Conclusion
<one paragraph; what downstream code may now rely on, and what it may not>

## Reproduce
<exact commands / tool calls with parameters>
```

Rules: no listings of game code beyond a few instructions; addresses + descriptions instead.
A finding with an empty "Unknown / not checked" section is suspicious — state the boundary of your search.
