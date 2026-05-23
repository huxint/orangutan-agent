## [2026-05-24 04:20] | Task: Tool File Search Structured Output

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code CLI`
- Linked plan: `none — narrow spec-0014 built-in migration slice`

### User Query

Continue the long-running Orangutan v2 implementation by reading the project
docs first, moving one coherent version forward, keeping docs in sync, validating
the result, and committing it as its own version.

### Changes Overview

- Areas: `oran-tool` file-search built-in, structured tool output docs,
  release/status docs.
- Key actions: `file.search` now fills `Output::data_json` with a serialized
  `file_search` payload carrying the searched root, pattern, regex flag,
  full `matches[]` array (one `{path, line_number, text}` object per match),
  the post-truncation match count, the `truncated` flag plus a precise
  `truncation_reason` (`null` / `"matches"` / `"bytes"`), the cumulative
  scanned file byte count, and the count of non-binary files actually
  scanned. The slice-20 `path:line:text` text rendering plus the
  slice-47/20 trailing truncation summary are unchanged.
- Key actions: successful searches now also fill `Output::usage.bytes_read`
  (cumulative scanned file bytes — includes binary files we paid the IO
  for and then skipped), `files_touched` (count of files actually run
  through the matcher; binary files do not count), `match_count`
  (post-truncation match count), and the `truncated` cap flag, so audit
  fan-out, schedulers, and provider adapters do not have to parse prose
  to learn search cost.

### Design Intent

Spec 0014 migrates built-ins incrementally because the project does not yet
have provider adapters, scheduler caps, or audit fan-out consumers.
`file.search` is the natural next migration after slice 62's `file.read`
because (1) it already returns a structured-feeling list of matches that
provider adapters and the future web UI want to render directly, (2) the
trailing `(truncated; ...)` summary line is hard to parse reliably, and
(3) the per-walk cost figures (`bytes_read`, `files_touched`) are exactly
the metrics spec 0012's scheduler needs for cost-aware preemption. Mirror
of the slice-62 pattern: text fallback unchanged, structured data emitted
in `Output::data_json`, usage counters filled. Pinning `truncation_reason`
in `data_json` (in addition to the boolean `truncated`) so callers do not
have to guess which cap fired matches the existing precedence rule already
encoded in the text rendering.

### Files Modified

- `include/oran/tool/builtins.hpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-tool/file_search.cpp`
- `tests/tool/test_registry.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice/version pointer, current structured-output summary,
  and `oran-tool` test/assertion count.
- `docs/ARCHITECTURE.md` — current `oran-tool` inventory and `file.search` row
  now describe `data_json` plus usage counters; planned-work list trims the
  closed `file.search` migration.
- `docs/design-docs/tool-runtime.md` — output-shape policy now records the
  shipped `file.search` JSON payload and remaining built-in migration work.
- `docs/product-specs/0014-structured-tool-output.md` — status slice pointer
  and migration list now mark `file.search` as the second structured built-in
  payload.
- `docs/QUALITY_SCORE.md` — refreshed `oran-tool` assertion count and registry
  summary now covers the new `file.search` structured-output coverage.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build oran-tool`
  - `xmake build test-tool`
  - `xmake run test-tool "[structured]"`
  - `xmake run test-tool`
- Tests added/changed: `tests/tool/test_registry.cpp` asserts single-file
  matches, recursive walk usage, match-cap truncation, byte-cap truncation,
  the empty-matches structured payload, and the `regex` flag round-trip in
  `data_json`. `test-tool` reports 152 cases / 1415 assertions.
- Bench impact: no new bench scenario; the change serializes already-collected
  match data plus two new counters on the existing success path.
- Compile-budget delta: no public heavy include changes; JSON serialization
  stays in the `.cpp` handler and the public built-in comment remains
  stdlib-only.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
