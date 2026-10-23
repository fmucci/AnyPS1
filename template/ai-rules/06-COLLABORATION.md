# 06 — Collaboration

Remote: the game repository's own (set it with `git remote add origin ...`). Default branch: `main`.

## Ownership & claims (avoids two agents porting the same thing)
- Work is split by **subsystem** (e.g. `loader`, `recompiler`, `lib/gpu`, `lib/spu`, `lib/cd`, `fmt/lev`, `fmt/vrm`, `audio`, `fmv`).
- Before starting, add a row to `docs/CLAIMS.md` (`subsystem | owner | branch | date | status`; include the queue step IDs) in its own tiny PR/commit,
  and wait for it to land (or the owner's OK). One owner per subsystem at a time.
- Touch another subsystem's files only via an issue/PR comment to its owner.
- Release a claim when done or when you stop (set status `released`).

## The queue
`docs/port-queue.md` lists the numbered steps and their status. Pick work from it (status `ready`), claim it in
`docs/CLAIMS.md` using the step ID, and **update the queue in the same commit/PR as any finding, tool change or
push** (status, findings column, change log). Never renumber steps. Reviewers reject PRs that skip this.

## Branches & commits
- Branch: `<subsystem>/<short-topic>`, e.g. `fmt/lev-header`. Collaborators never commit to `main` directly (the owner, fmucci, has authorised direct commits to `main` for himself and the agent he runs).
- Small commits; message = what + why + evidence reference (`docs/findings/NNNN-...`).
- **Do not commit, push, rebase, force-push or open PRs unless the owner or the task explicitly says so.**
- Each PR must: link its findings, list what is OBSERVED/INFERRED/UNKNOWN, show the verification command + output,
  and confirm `git status` has no `og-files/`/`work/` content.

## Findings are the shared memory
- One finding file per conclusion: `docs/findings/NNNN-short-title.md` (template in 07). Numbers are allocated by
  taking the next free number at PR time; on conflict, renumber yours.
- Never edit someone else's finding to change its conclusion. Add a new finding that supersedes it and link both
  (`Supersedes: NNNN` / `Superseded-by: MMMM`). Typo/clarity fixes are fine.
- If you discover a finding is wrong, say so immediately in a new finding and flag dependants in `docs/CLAIMS.md`.

## Shared files with high conflict risk
`00-CONTEXT.md` (Open questions), `docs/CLAIMS.md`, `docs/stubs.md`, `docs/symbols/*.csv`: append-only where
possible, keep rows sorted by address/ID, small PRs.

## Review checklist (reviewers apply it literally)
0. `docs/port-queue.md` updated (status, findings, change log) when the PR changes a step.
1. Every non-trivial claim has an evidence class and backing (01 §1).
2. No assumed SDK/BIOS/hardware behaviour without a cited source + binary confirmation.
3. Unsupported paths fail loudly; no silent stubs; stubs registered in `docs/stubs.md`.
4. Reproducible from `tools/setup.sh` + documented commands.
5. No game data or long listings; no secrets; no absolute personal paths in committed files.
6. Verification output is quoted, not asserted.

## Agent conduct
- Keep a running log in your PR description of what you did and what you could not verify.
- If instructions from a user and these rules conflict, follow the user and propose an edit to `ai-rules/`.
- When you finish a session, leave the repo in a state where the next agent can start from `ai-rules/README.md` alone.
