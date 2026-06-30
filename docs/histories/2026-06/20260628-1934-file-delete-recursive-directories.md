## [2026-06-28 19:34] | Task: FileDelete recursive directories

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local CLI workspace
- Linked plan: none — small Tools/Workspace slice under the ROADMAP frontier

### User Query

> Continue advancing the project by implementing the most worthwhile next slice.

### Changes Overview

- Areas: `oran-io`, `oran-tool`, Tools/Workspace docs.
- Key actions: added `io::delete_path(executor, path, DeletePathOptions)` with
  explicit directory recursion intent, kept `io::delete_file` as the
  regular-file wrapper, extended `FileDelete` input to `{path, recursive?}`,
  preserved symlink refusal, and filled `usage.files_touched` from the removed
  path count.

### Design Intent

The roadmap called for consolidation, not another per-kind delete tool. This
slice keeps the public tool name stable and makes destructive directory removal
explicit at the callsite: files delete directly, directories require
`recursive=true`, and symlinks still reject before any filesystem follow can
occur. Recursive directory deletes conservatively clear private range-read
caches so child file views cannot survive a removed tree.

### Files Modified

- `include/oran/io/file.hpp`
- `src/oran-io/file.cpp`
- `include/oran/tool/builtins.hpp`
- `src/oran-tool/file_delete.cpp`
- `tests/io/test_file.cpp`
- `tests/tool/test_registry.cpp`
- `tests/tool/test_workspace.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/io-runtime.md` — documents `delete_path`, recursion intent,
  and cache invalidation behavior.
- `docs/design-docs/tool-runtime.md` — updates `FileDelete` semantics and usage
  counters.
- `docs/product-specs/0002-tool-registry.md` — records the unified delete shape.
- `docs/product-specs/0011-file-view-and-caching.md` — records recursive delete
  cache invalidation.
- `docs/product-specs/0013-workspace-and-path-policy.md` — records that
  recursive deletes keep the existing `resolve_delete` boundary.
- `docs/product-specs/0014-structured-tool-output.md` — updates
  `FileDelete` usage semantics.
- `docs/ARCHITECTURE.md` — refreshes `oran-io` and `oran-tool` inventory rows.
- `docs/ROADMAP.md` — moves the unified delete reshape to resolved and points
  the frontier at shared ignore/display helpers.
- `docs/STATUS.md` — bumps slice/history pointer and test counts.
- `docs/QUALITY_SCORE.md` — refreshes `oran-io` / `oran-tool` counts and
  coverage notes.
- `docs/releases/feature-release-notes.md` — adds the user-visible release row.

### Validation

- Commands run:
  - `xmake build test-io`
  - `xmake run test-io` — 58 cases / 328 assertions
  - `xmake build test-tool`
  - `xmake run test-tool` — 213 cases / 2246 assertions
  - `xmake build orangutan`
  - `xmake run orangutan -- --help` — reports `2.0.0-slice265`
  - `xmake test` — failed inside the sandbox on `test-http` and
    `test-bootstrap` due loopback HTTP/WebSocket restrictions, then passed
    19/19 outside the sandbox
  - `make ci`
- Tests added/changed: IO coverage for recursive directory deletes,
  target-path symlink refusal, and nested symlink targets surviving recursive
  tree deletion; tool coverage for `recursive` schema validation, recursive
  directory usage counts, and workspace-relative recursive delete.
- Bench impact: no new bench; delete performance is dominated by filesystem
  calls and no implementation tradeoff was introduced.
- Compile-budget delta: no new target or heavy public include; `test-tool`
  rebuilt due the touched public header.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
