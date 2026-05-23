## [2026-05-24 05:40] | Task: Tool Directory List Structured Output

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

- Areas: `oran-tool` directory-list built-in, structured tool output docs,
  release/status docs.
- Key actions: `directory.list` now fills `Output::data_json` with a
  serialized `directory_list` payload carrying the resolved root path, the
  resolved `include_hidden` and `max_entries` inputs, the entry count, and
  an `entries[]` array of `{name, path, kind, size_bytes}` (with the wire
  spelling `regular_file` / `directory` / `symlink` / `other` for `kind`
  and JSON null `size_bytes` for non-regular kinds). The slice-29
  `<path>:<kind>:<size_bytes or '-'>` text rendering is unchanged.
- Key actions: successful listings now also fill `Output::usage`
  (`files_touched=1` for the directory itself, `match_count=entry_count`)
  so audit fan-out and the future scheduler can see directory-walk cost
  without parsing prose.
- With this slice every shipped filesystem built-in has completed its v1
  migration to spec 0014's structured envelope (`file.read` slice 62,
  `file.search` slice 63, `directory.list` slice 64, mutation tools
  slice 61). Provider adapter mapping, scheduler byte caps, audit usage
  fan-out, and hook raw-data redaction remain spec-0014 follow-ups.

### Design Intent

Spec 0014 migrates built-ins incrementally because the project does not yet
have provider adapters, scheduler caps, or audit fan-out consumers.
`directory.list` is the natural last read-side built-in to migrate after
`file.read` and `file.search` because (1) its current text rendering uses
the literal `-` sentinel for missing sizes — JSON null is the better wire
shape that callers do not need to special-case, (2) the entries array is
exactly the structure a future web UI / channel adapter wants to render as
a directory listing, and (3) `entry_count` is the most useful per-call
cost figure for the scheduler. Mirror of the slice-62/63 pattern: text
fallback unchanged, structured data emitted in `Output::data_json`, usage
counters filled. Choosing `files_touched=1` rather than `entry_count`
keeps the semantic clean — the tool only opened one directory, even though
it observed N children.

### Files Modified

- `include/oran/tool/builtins.hpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-tool/directory_list.cpp`
- `tests/tool/test_registry.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice/version pointer, current structured-output summary,
  and `oran-tool` test/assertion count; "Next intended slice" now records
  that the built-in side of spec 0014 is complete.
- `docs/ARCHITECTURE.md` — current `oran-tool` inventory and `directory.list`
  row now describe `data_json` plus usage counters; planned-work list trims
  the closed `directory.list` migration.
- `docs/design-docs/tool-runtime.md` — output-shape policy now records the
  shipped `directory.list` JSON payload and notes that every filesystem
  built-in has completed its v1 migration.
- `docs/product-specs/0014-structured-tool-output.md` — status slice pointer
  and migration list now mark `directory.list` as the third structured
  built-in payload and note that the built-in side is complete.
- `docs/QUALITY_SCORE.md` — refreshed `oran-tool` assertion count and
  registry summary now covers the new `directory.list` structured-output
  coverage.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build oran-tool`
  - `xmake build test-tool`
  - `xmake run test-tool`
  - `xmake test`
- Tests added/changed: `tests/tool/test_registry.cpp` asserts the happy-path
  entries[] (with correct sort order, regular/directory kinds, and the
  null `size_bytes` for non-regular kinds), the empty-directory empty
  array, the `include_hidden` round-trip in `data_json`, and the
  `max_entries` echo. `test-tool` reports 156 cases / 1470 assertions.
- Bench impact: no new bench scenario; the change serializes
  already-collected directory entries plus two new counters on the existing
  success path.
- Compile-budget delta: no public heavy include changes; JSON serialization
  stays in the `.cpp` handler and the public built-in comment remains
  stdlib-only.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
