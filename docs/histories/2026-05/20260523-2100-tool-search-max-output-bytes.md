## [2026-05-23 21:00] | Task: `max_output_bytes` cap on `file.search` (closes spec 0011 v1.1 "Output cap on `file.search`")

### Execution Context

- Agent: `Claude Code`
- Base model: `Opus 4.7`
- Runtime: `Claude Code CLI`
- Linked plan: none — scoped to the spec-0011 v1.1 "Output cap on
  `file.search`" item; v1.1's other items (line-offset index,
  file-view cache, regex compile cache, singleflight,
  external-edit awareness, built-in ignore predicate) remain
  follow-ups.

### User Query

> Deeply understand the project architecture and current
> implementation progress, continue advancing the project, two
> slices, one commit per slice; ultrathink.

### Changes Overview

- Areas: `oran-tool` (`file.search` byte-cap), `tests/tool`, version
  banner, the docs describing the new wire surface.
- Key actions:
  - `src/oran-tool/file_search.cpp`: schema grows
    `max_output_bytes: integer (minimum 1)`. `SearchOptions` adds
    `std::size_t max_output_bytes{kDefaultMaxOutputBytes}` with the
    default fixed at 1 MiB. Truncation moves from a `bool` to a
    `TruncReason { none, matches, bytes }` enum on `SearchOutcome`.
    Two small helpers — `count_digits` and `rendered_cost` — compute
    each candidate match's exact rendered byte cost
    (`path + ":" + line_number + ":" + text` plus the inter-match
    `\n` separator) inline so the cap can be enforced before the
    match is appended. `scan_text` now consumes the byte budget and
    a running `accumulated_bytes` counter, returns
    `TruncReason::bytes` when the next candidate would overshoot,
    and breaks out cleanly. `walk_and_scan` propagates the byte
    truncation across the directory walk and uses
    `outcome.truncated != TruncReason::bytes` as a second loop
    guard. `render` switches on `TruncReason` to emit the right
    trailing summary; when both caps could have fired, the
    final `if (outcome.matches.size() > opts.max_matches)`
    deliberately overwrites `TruncReason::bytes` with
    `TruncReason::matches` so the match-cap message dominates a
    tie (documented invariant — agents that set both caps can rely
    on a single dominant message per call).
  - `include/oran/tool/builtins.hpp` docstring carries the new
    `max_output_bytes` field and the two possible truncation
    summaries.
  - `tests/tool/test_registry.cpp`: three new
    `[max_output_bytes]` cases plus two new bad-type rows on the
    malformed-input matrix:
    - "caps rendered output at max_output_bytes": writes a 5-line
      `NEEDLE` file, picks a budget that fits exactly the first
      match (`path.size() + ":1:NEEDLE".size()`) and asserts only
      the first match plus the `(truncated; output capped at ...
      bytes)` summary survive.
    - "default max_output_bytes leaves small responses untouched":
      proves the default-shape callsite (no field supplied) is
      unchanged.
    - "match-cap wins when both caps could fire": pins the
      documented tie-break by giving a generous byte budget and a
      tight `max_matches=2`.
    - Bad-type matrix gains `"max_output_bytes":"big"` and
      `"max_output_bytes":0`; the sink-event tally bumps 10 → 12.
  - `src/oran-bootstrap/bootstrap.cpp` version banner bumped to
    `2.0.0-slice47`.

### Design Intent

The byte-cap is enforced at the *scan* layer, not after rendering.
A post-render slice would force us to buffer every candidate match
just to know whether the budget was exceeded — and the worst-case
matchers (a permissive regex over a directory with many large
files) are exactly the ones the cap exists to protect. Computing
each candidate's exact cost inline keeps the steady-state cost a
handful of integer ops per match (`count_digits` is one decimal
loop on `std::size_t`) and lets the scan stop the moment the budget
would be overshot, never materialising the over-cap match.

The exact-cost computation is deliberate. An estimate (e.g.,
`path.size() + 16 + text.size()`) would either overcharge (causing
early truncation on long paths whose digits fit in a byte) or
undercharge (letting the rendered output drift past `max_output_bytes`
by up to N bytes per match). Either of those would make the cap
useless for callers who pin the byte budget against a downstream
prompt cache invariant. The cost computation is constant-time per
match — `count_digits` is at most ~20 iterations on a 64-bit
`std::size_t`, and `rendered_cost` is a single arithmetic expression
that already reflects every byte the renderer will emit.

The tie-break rule — match-cap wins when both could have fired —
is a UX call, not a perf call. Agents that set both caps are
typically setting `max_matches` as a "how much I want to see" limit
and `max_output_bytes` as a "but never more than this" guardrail.
The match-cap message is the one they want to read; the byte-cap
message is the one they want to never see. Surfacing the byte cap
when the match cap would have fired anyway just confuses the
recovery path (raise `max_output_bytes`? but that wouldn't help —
the matches were already capped). The deterministic precedence is
pinned by a dedicated test so a future refactor cannot quietly
invert it.

The default of 1 MiB is generous enough that none of the slice-20 /
24 / 39 / 47 happy-path tests trip it, but tight enough that a
single multi-megabyte rendered output can never escape into the
prompt by default. Agents that want stricter or looser caps set
the field; the default exists so the worst-case never silently
escapes the agent's attention.

### Files Modified

- `src/oran-tool/file_search.cpp` (schema, options, scan helpers,
  walk gate, render switch)
- `include/oran/tool/builtins.hpp` (docstring)
- `tests/tool/test_registry.cpp` (3 new `[max_output_bytes]` cases
  + 2 new bad-type rows; sink-event tally 10 → 12)
- `src/oran-bootstrap/bootstrap.cpp` (version banner)
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/product-specs/0011-file-view-and-caching.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 47, new history pointer, refreshed
  `oran-tool` test counts (124 / 1047), and the next-intended-slice
  narrative now names the remaining v1.1 follow-ups.
- `docs/ARCHITECTURE.md` — `file.search` inventory row gains the
  new `max_output_bytes?` field and the truncation-precedence note.
- `docs/QUALITY_SCORE.md` — Test framework row refreshed
  (`oran-tool` 124 / 1047).
- `docs/product-specs/0011-file-view-and-caching.md` — Status block
  gains a slice-47 entry; v1.1's "Output cap on `file.search`"
  item is marked complete.
- `docs/releases/feature-release-notes.md` — user-visible release
  note.

### Validation

- Commands run:
  - `xmake build oran-tool`
  - `xmake build test-tool` / `xmake run test-tool '[file_search]'`
  - `xmake test` (all 10 buckets pass)
- Tests added/changed:
  - `tests/tool/test_registry.cpp` adds 3 `[max_output_bytes]`
    cases plus 2 bad-type rows. `tests/tool` reports 124 / 1047
    (was 121 / 1022).
- Bench impact: not measured — the new cost computation is a
  constant-time integer expression per matching line; the
  default-shape callsite (no field supplied) generates the same
  scan as before. The slice-20 / 24 bench numbers in
  `QUALITY_SCORE.md` still apply.
- Compile-budget delta: not measured. No new headers; the changes
  are localised to one TU (`file_search.cpp`) and one docstring.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none. v1.1's remaining items (built-in ignore
  predicate / `.gitignore` honor for `file.search` directory walks,
  line-offset index, file-view cache, regex compile cache,
  singleflight, external-edit awareness) are tracked in
  `docs/product-specs/0011-file-view-and-caching.md` as the next
  0011 milestone.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
