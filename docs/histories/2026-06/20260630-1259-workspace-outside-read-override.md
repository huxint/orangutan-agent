## [2026-06-30 12:59] | Task: workspace outside read/list override

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: API / local workspace
- Linked plan: none; small Workspace v1.1 follow-up named by `STATUS.md`.

### User Query

Continue the unfinished slice.

### Changes Overview

- Areas: `oran-tool` workspace/path policy, filesystem built-in schemas, docs/status.
- Key actions:
  - Added `allow_outside_workspace` to `FileRead`, `FileSearch`, and
    `DirectoryList`.
  - Added read/list-only `Workspace::resolve_*_outside_workspace(...)`
    helpers for existing outside targets.
  - Promoted successful per-call outside read/list resolution to an `ask`
    decision with reason `outside_workspace_override`.
  - Added explicit audit metadata for outside overrides:
    `resolved_display_path` and `per_call_outside_workspace_override`.

### Design Intent

The override is deliberately narrow: it only applies after normal read/list
workspace resolution fails with `outside_workspace` or `symlink_escape`, and it
only resolves existing targets. That keeps ordinary workspace reads and
configured extra-read-root paths fast and non-interactive, while making a
one-off outside escape impossible to execute without an approval row. Mutating
tools do not get a per-call escape; writes/deletes still require configured
extra roots.

### Files Modified

- `include/oran/tool/builtins.hpp`
- `include/oran/tool/registry.hpp`
- `include/oran/tool/workspace.hpp`
- `src/oran-tool/_impl/path_resolution.hpp`
- `src/oran-tool/path_resolution.cpp`
- `src/oran-tool/registry.cpp`
- `src/oran-tool/workspace.cpp`
- `src/oran-tool/file_read.cpp`
- `src/oran-tool/file_search.cpp`
- `src/oran-tool/directory_list.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/tool/test_registry.cpp`
- `tests/tool/test_workspace.cpp`

### Docs Updated In This PR (Prime Directive)

- `docs/product-specs/0013-workspace-and-path-policy.md` — marks the v1.1
  outside read/list override shipped and documents audit fields.
- `docs/design-docs/tool-runtime.md` — documents the new read/list schemas and
  dispatch approval semantics.
- `docs/ARCHITECTURE.md` — refreshes the `oran-tool` inventory and Workspace
  helper status.
- `docs/ROADMAP.md` — moves the Tools and Workspace frontiers to slice 267.
- `docs/STATUS.md` — bumps the current slice, history pointer, next intended
  slice, and `oran-tool` test counts.
- `docs/QUALITY_SCORE.md` — refreshes `oran-tool` test counts and coverage note.
- `docs/releases/feature-release-notes.md` — adds the user-visible release row.

### Validation

- Commands run:
  - `xmake build test-tool`
  - `xmake run test-tool` — 219 cases / 2379 assertions.
  - `xmake test` — 19/19 buckets passed.
  - `xmake run orangutan -- --help` — reports `orangutan v2.0.0-slice267`.
  - `make ci` — passed.
- Tests added/changed:
  - Workspace unit coverage for read/list outside resolution.
  - Registry dispatch coverage for approval-required audit metadata.
  - Broker-approved coverage for `FileRead`, `FileSearch`, and
    `DirectoryList`.
- Bench impact: not measured; this is an approval-gated cold path.
- Compile-budget delta: no new target or dependency; existing `oran-tool`
  translation units grew small schema/resolution branches.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: 2026-06-30 `workspace-outside-read-override`.
