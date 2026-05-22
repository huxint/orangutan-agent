## [2026-05-22 14:56] | Task: tool registry transparent lookup

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI
- Linked plan: none — small deep-review P0 follow-up from `docs/STATUS.md`

### User Query

> Understand the current project architecture, goals, and progress, then continue implementation.

### Changes Overview

- Areas: `oran-tool`, bootstrap version, docs.
- Key actions: `Registry::entries_` now uses transparent string hashing, and
  `Registry::remove`, `Registry::find`, and `Registry::dispatch` look up the
  incoming `std::string_view` name directly. `xmake run orangutan` now reports
  slice 36.

### Design Intent

The remaining deep-review P0 was a hot-path allocation in registry lookup:
`dispatch`, `find`, and `remove` materialised a temporary `std::string` even
though their public API already receives `std::string_view`. This slice keeps
the owned `std::string` keys for storage, but adds a transparent hasher so
heterogeneous lookup removes that allocation without changing caller behavior.

### Files Modified

- `include/oran/tool/registry.hpp`
- `src/oran-tool/registry.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/tool-runtime.md` — records the slice 36 registry lookup shape.
- `docs/ARCHITECTURE.md` — updates the `oran-tool` inventory and slice-status summary.
- `docs/QUALITY_SCORE.md` — removes transparent lookup from the tool-registry next step.
- `docs/STATUS.md` — bumps slice/history pointer and refreshes the open tech-debt summary.
- `docs/exec-plans/tech-debt-tracker.md` — closes the transparent lookup P0 bullet.
- `docs/releases/feature-release-notes.md` — adds the slice 36 release-note row.

### Validation

- Commands run:
  - `xmake build test-tool`
- Tests added/changed: none; public behavior is unchanged and existing registry
  lookup/dispatch coverage exercises the new heterogeneous lookup path.
- Bench impact: no new scenario; `bench-tool` already has `registry.lookup` and
  `registry.dispatch_allow` coverage for this path.
- Compile-budget delta: no new TU and no heavy public include; the public header
  adds only `<cstddef>` plus a small transparent hasher.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: no remaining P0 items in the `deep-review-2026-05-21` row;
  P1/P2/P3 follow-ups remain in the tracker.
- Linked release note: `docs/releases/feature-release-notes.md`
