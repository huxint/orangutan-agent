## [2026-06-30 11:50] | Task: Workspace walk filter and display labels

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI
- Linked plan: none; small Workspace v1.1 follow-up from `docs/STATUS.md`

### User Query

> Continue advancing the next most valuable project implementation.

### Changes Overview

- Areas: `oran-tool` Workspace, `FileSearch`, `DirectoryList`, docs/status.
- Key actions:
  - Added `WorkspaceWalkFilter` and `WorkspaceWalkOptions` as the shared
    recursive filesystem filter for dotfile, built-in low-signal directory, and
    `.gitignore` / `.ignore` decisions.
  - Added `Workspace::display_path(...)` for stable `<workspace>/...`,
    `<read-root-N>/...`, and `<write-root-N>/...` labels.
  - Migrated recursive `FileSearch` off its private ignore-stack copy.
  - Extended `DirectoryList recursive=true` to honor source-controlled ignore
    files and use workspace display labels when a Workspace is supplied.

### Design Intent

The project frontier named Workspace v1.1's shared ignore/display helper as the
best next slice after recursive `DirectoryList` and unified `FileDelete`. This
change chooses consolidation over another tool feature: recursive search and
recursive listing now make the same hidden/ignore decisions, and workspace-backed
outputs stop exposing temp absolute roots. The ignore parser remains in
`oran-tool` with a pimpl-backed public filter so `<filesystem>` and fnmatch
details stay out of the public header.

### Files Modified

- `include/oran/tool/workspace.hpp`
- `src/oran-tool/workspace.cpp`
- `src/oran-tool/file_search.cpp`
- `src/oran-tool/directory_list.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/tool/test_registry.cpp`
- `tests/tool/test_workspace.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 266, refreshed latest slice and counts.
- `docs/ROADMAP.md` — moved Tools/Workspace frontier and next step.
- `docs/ARCHITECTURE.md` — documented `WorkspaceWalkFilter`, display labels,
  and updated `FileSearch` / `DirectoryList` inventory.
- `docs/design-docs/tool-runtime.md` — updated built-in behavior.
- `docs/product-specs/0002-tool-registry.md` — updated built-in status.
- `docs/product-specs/0013-workspace-and-path-policy.md` — marked shared
  filter/display as shipped and kept the remaining v1.1 override pending.
- `docs/product-specs/0014-structured-tool-output.md` — documented workspace
  display labels in path fields.
- `docs/QUALITY_SCORE.md` — refreshed `oran-tool` test counts and coverage note.
- `docs/releases/feature-release-notes.md` — added the user-visible release row.

### Validation

- Commands run:
  - `xmake build test-tool && xmake run test-tool`
  - `xmake test`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed:
  - Workspace filter unit coverage for hidden, built-in, basename, slash,
    negation, and nested `.ignore` rules.
  - Workspace display label coverage for primary and extra read roots.
  - Workspace-backed `FileSearch` / `DirectoryList` output label coverage.
  - Recursive `DirectoryList` source-controlled ignore-file coverage.
- Bench impact: no new benchmark; this is a structure/consistency slice, not a
  competing implementation choice.
- Compile-budget delta: public header adds a pimpl and `<memory>` only; heavy
  filesystem/fnmatch/ignore parsing remains in `workspace.cpp`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#workspace-walk-filter-display`
