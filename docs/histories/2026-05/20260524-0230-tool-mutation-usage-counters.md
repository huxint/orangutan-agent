## [2026-05-24 02:30] | Task: Tool Mutation Usage Counters

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `none — narrow spec-0014 built-in migration slice`

### User Query

Continue slice 61 by completing docs/version sync, validation, and commit for the
current `oran-tool` mutation-output work.

### Changes Overview

- Areas: `oran-tool` built-ins, structured tool output docs, release/status docs.
- Key actions: `file.write`, `file.edit`, and `file.delete` now fill measured
  `Output::usage` counters while keeping the existing text summaries and leaving
  `data_json` empty for the v1 migration path.

### Design Intent

Spec 0014 deliberately migrates built-ins one at a time so provider adapters,
scheduler caps, and audit fan-out can consume the same envelope without forcing a
large transport rewrite. Mutation tools already know the relevant byte/replacement
counts at the handler boundary, so recording them in `Output::usage` removes
string-parsing pressure for future hooks and schedulers while preserving the
current model-facing text.

### Files Modified

- `include/oran/tool/builtins.hpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-tool/file_delete.cpp`
- `src/oran-tool/file_edit.cpp`
- `src/oran-tool/file_write.cpp`
- `tests/tool/test_registry.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice/version pointer, next-slice summary, and `oran-tool`
  test/assertion count.
- `docs/ARCHITECTURE.md` — current `oran-tool` inventory and mutation built-in
  rows now describe usage counters.
- `docs/design-docs/tool-runtime.md` — output-shape and mutation built-in
  descriptions now reflect the shipped counters.
- `docs/product-specs/0014-structured-tool-output.md` — marks the
  file.write/edit/delete migration item as slice-61 complete for usage counters.
- `docs/QUALITY_SCORE.md` — refreshed `oran-tool` assertion count and registry
  summary.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build test-tool`
  - `xmake run test-tool` (145 cases / 1279 assertions)
  - `xmake build orangutan`
  - `xmake build bench-tool`
  - `xmake run bench-tool`
  - `xmake test`
  - `xmake run orangutan -- --help`
  - `make ci`
  - `git diff --check`
- Tests added/changed: `tests/tool/test_registry.cpp` asserts mutation usage
  counters and pins `data_json == nullopt` for this v1 migration.
- Bench impact: no new bench scenario; existing mutation benches cover the same
  handler path and the added counter assignments are constant-time metadata
  writes.
- Compile-budget delta: no public heavy include changes; `tool::Output` remains
  serialized-string based.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
