# 04 — REA playbook

REA = MCP server. Tools appear as `mcp__rea__*`. Always load the `reverse-engineer-anything` skill first
(it states REA's own rules: summary-first, cite evidence IDs, separate observation from inference).

## Session lifecycle
1. `open_binary {path: "<abs>/work/elf/<name>.elf", provider_id: "ghidra"}` — **absolute path required**.
   Opening runs Ghidra auto-analysis; results are served only after it completes (can take minutes for `menu.elf`).
2. Work with focused tools (below).
3. `close_binary` when finished. One active target at a time per session; do not reopen the same file repeatedly.
   The Ghidra program is **ephemeral**: annotations (names, comments, bookmarks) vanish on close unless you export
   them. See "Persisting work".

## Tool cheat-sheet (parameter names verified on REA 6.3.0 in this project)
| Goal | Tool | Notes |
|------|------|-------|
| Segments, counts | `binary_overview` | no params |
| List/search strings | `list_strings`, `search_strings {pattern, mode}` | `list_strings` takes no `limit`; output is large and is auto-saved to a file → query it with `jq`. `search_strings` needs `pattern` — not `query`. Default `mode` is **literal**: pass `mode: "regex"` for regex (a regex in literal mode silently returns `[]`) |
| Find functions | `search_procedures`, `list_procedures` | |
| Understand one function | `analyze_function`, `procedure_pseudo_code {procedure}`, `procedure_assembly {procedure}` | read **both** pseudo-code and assembly (01 §2). Param is `procedure` (address string like `"0x8003d044"` or any address inside the function), **not** `address` |
| Callers / callees / xrefs | `procedure_callers`, `procedure_callees`, `xrefs`, `find_xrefs_to_name` | |
| Structured trace | `trace_call_path`, `build_call_path`, `trace_feature` | |
| Annotate | `set_address_name`, `set_comment`, `set_inline_comment`, `set_bookmark` | session-only, see below |
| Record unknowns | `record_unknown`, `update_unknown`, `verify_unknown_resolution` | map 1:1 to Open questions |
| Share evidence | `export_evidence_bundle`, `import_evidence_bundle`, `get_evidence_bundle` | |

Large outputs: REA often returns >100k chars and the harness saves them to a file. Never paste them; probe with
`jq 'type, length, keys'` then slice. Prefer summary views / paging parameters when offered.

## Ghidra/MIPS lane specifics (important)
REA's Ghidra MIPS lane only admits ELF32 executables whose headers declare: `EF_MIPS_ARCH_32R2`, an inspected
`.MIPS.abiflags` record with 32-bit GPR and CPR1, no CPR2, and FP ABI double/FPXX. PS1 code is MIPS-I with no FPU.
`tools/psx2elf.py` therefore **declares** MIPS32r2 / FP double to pass REA's admission checks. Consequences
(INFERRED, verify before relying on them):
- Plain R3000 integer instructions decode the same under MIPS32r2; but Ghidra may decode **GTE (COP2)** instructions
  incorrectly or as unknown. Treat any COP2 region in Ghidra as unreliable and read raw words yourself.
- Ghidra may mis-handle R3000 specifics (load delay slots; Ghidra models branch delay slots, not load delays).
  Cross-check suspicious data flow in assembly.
- The wrapper is a *view*: addresses equal the PS1 virtual addresses (KSEG0, 0x80xxxxxx); one segment `.text`
  holds code **and** data (no section split). Do not infer "this is code/data" from the segment.

## Persisting work (because the Ghidra DB is ephemeral)
Every confirmed conclusion goes into a finding (07) in `docs/findings/` with evidence IDs and addresses.
Names you assign should be recorded in `docs/symbols/<exe>.csv` (`address,name,kind,finding`) so the next session
can re-apply them with `set_addresses_names`. The CSV is the source of truth, the Ghidra DB is not.

## Evidence discipline
- Quote the `evidence_id` (e.g. `ev_aefcf42b…`) in the finding. Evidence IDs are session-bound; also record the
  command/parameters and the binary sha256 so another agent can regenerate it.
- Keep `limitations` from tool output in the finding when they bear on the conclusion.
- Runtime tools (`observe_*`, `capture_*`) **execute** things. This project has no host-runnable PS1 target; do not
  use them on the game. Static analysis only unless the owner says otherwise.
