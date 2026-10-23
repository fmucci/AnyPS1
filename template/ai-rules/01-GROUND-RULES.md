# 01 — Ground rules (no assumptions)

## 1. Evidence classes
Every claim in code comments, docs, commits, PRs and chat about the game must be one of:

| Class | Meaning | Required backing |
|-------|---------|------------------|
| **OBSERVED** | Directly read from the artifact or a tool result | REA evidence ID, or `file:offset`, or a command that re-prints it |
| **INFERRED** | Reasoned from observations | the observations it rests on + what would falsify it |
| **UNKNOWN** | Not yet established | an entry in `00-CONTEXT.md` Open questions or a finding |

Words like "probably", "typically", "standard PsyQ", "like other PS1 games" mean the claim is at best INFERRED
and usually UNKNOWN. Write it as such. **Code that depends on an INFERRED/UNKNOWN fact must say so in a comment and
must fail loudly (see 3) rather than proceed.**

## 2. Things you must verify, never recall from memory
- Register meanings, memory-map addresses, BIOS call numbers, GPU/GTE/SPU/CD command semantics: take them from
  documentation **you cite** (name + section) *and* confirm against the binary's actual usage.
- Struct layouts, field offsets, file formats: derive from data/code, cross-check with at least two independent
  samples (e.g. two levels), and record the result as a finding.
- What a function does: read the decompilation **and** the disassembly of the same function; Ghidra's decompiler is
  a hypothesis generator, not a source of truth, especially for delay slots, GTE ops and hand-written asm.
- Names: auto names like `FUN_8003ab10` stay as-is until a finding justifies renaming. Do not rename on a hunch.

## 3. Fail loudly
Port code (recompiled output and host library implementations) must treat unsupported/unknown states as errors:
print what/where (guest PC, call, args) to stderr and abort. No silent default return values, no `/* TODO */`
stubs that return 0. A stub is allowed only if it (a) logs once with guest PC, (b) is listed in
`docs/stubs.md`, and (c) is justified by a finding showing the game ignores the result.

## 4. Reproducibility
- Anything derived from the disc must be regenerable by `tools/setup.sh` + documented commands.
- Pin versions (Ghidra, REA, Python script behaviour). Record versions in findings.
- Never hand-edit derived files in `work/`; change the generator in `tools/` instead.
- Run Python on untrusted/derived inputs as `python3 -I` (isolated mode). Keep scripts outside the directory that
  contains the data they process; pass paths as arguments.

## 5. Legal / content rules (hard)
- No game binaries, assets, extracted data, disc images, or long disassembly/decompilation listings in git,
  issues, PRs or chat logs shared outside the project. Short snippets (a few instructions) to illustrate a
  finding are fine. Prefer addresses + descriptions to listings.
- No passwords, keys or personal data in the repo. The RAR password is deliberately **not** documented here;
  the owner provides it via `PS1_ARCHIVE_PASSWORD`.
- Do not use or request commercially distributed copies/ROM sites. Work only from the owner's disc.
- Do not copy code from other projects without checking its license; AnyPS5/REA are references for
  *method*, not code to paste.

## 6. Hard "never" list
- Never `git add -A` blindly; check `git status` for `og-files/` / `work/` leaks (they are ignored — keep it so).
- Never commit/push/force-push/rewrite history without the owner's instruction (see 06).
- Never overwrite files in `og-files/`. Treat them as read-only originals.
- Never claim "done/working/verified" without running the verification and quoting its output.
- Never continue past a failed/contradictory observation by "adjusting expectations". Stop, record it, ask.
- Never broaden an investigation target without saying so (REA runtime tools execute the target).

## 7. When unsure
Ask the owner a specific question with the options you see, or write the UNKNOWN down and work on something else.
A wrong guess in the foundation (e.g. a mis-decoded format) costs every downstream collaborator.
