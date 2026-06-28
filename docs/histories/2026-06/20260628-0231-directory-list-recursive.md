## [2026-06-28 02:31] | Task: Recursive DirectoryList

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex API`
- Linked plan: none; small ROADMAP Tools slice.

### User Query

> Continue advancing the project implementation.

### Changes Overview

- Areas: `oran-tool`, bootstrap version tag, Tools/Workspace docs.
- Key actions: extended the existing `DirectoryList` built-in with an optional
  `recursive` boolean, preserving single-level default behavior; recursive
  calls walk a whole tree, skip nested symlinks, apply the built-in low-signal
  directory skip list, keep the existing text/entries output shape, and mark
  `data_json.recursive`.

### Design Intent

The previous frontier called for a recursive project list plus a unified delete
reshape. This slice takes the lower-risk half first: a recursive listing is
read-only, keeps the existing tool name and capability, and supplies the second
recursive consumer needed to unblock Workspace v1.1's shared ignore predicate.
The unified delete half remains separate because directory deletion needs an
explicit recursion intent and a more careful failure/rollback story.

### Files Modified

- `src/oran-tool/directory_list.cpp`
- `include/oran/tool/builtins.hpp`
- `tests/tool/test_registry.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/ARCHITECTURE.md`
- `docs/ROADMAP.md`
- `docs/STATUS.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/tool-runtime.md`
- `docs/product-specs/0002-tool-registry.md`
- `docs/product-specs/0013-workspace-and-path-policy.md`
- `docs/product-specs/0014-structured-tool-output.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/tool-runtime.md` — `DirectoryList` input/output contract.
- `docs/product-specs/0002-tool-registry.md` — built-in catalog status.
- `docs/product-specs/0013-workspace-and-path-policy.md` — second recursive
  consumer and shared ignore-predicate follow-up.
- `docs/product-specs/0014-structured-tool-output.md` — `data_json.recursive`
  and recursive usage semantics.
- `docs/ARCHITECTURE.md` — library inventory and tool narrative.
- `docs/ROADMAP.md` — Tools frontier, Workspace predependency, Dependency
  Frontier #4.
- `docs/STATUS.md` / `docs/QUALITY_SCORE.md` — slice and `test-tool` counts.
- `docs/releases/feature-release-notes.md` — user-visible tool behavior.

### Validation

- Commands run:
  - `xmake build test-tool`
  - `xmake run test-tool "[unit][tool][directory_list]"`
  - `xmake run test-tool`
  - `xmake test` (sandboxed run failed because local HTTP/WebSocket tests could
    not open loopback sockets)
  - `xmake test` (rerun outside the sandbox; 19/19 passed)
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed:
  - Recursive listing includes nested directory entries and structured
    `recursive=true`.
  - Recursive listing skips `.git` / `node_modules` even with hidden opt-in.
  - Recursive listing skips nested symlink entries and does not expose their
    targets.
  - Recursive listing enforces `max_entries` across the tree.
  - Malformed `recursive` input rejects as `invalid_argument`.
- Bench impact: no new bench; this is a read-only operator tool path and stays
  bounded by `max_entries`.
- Compile-budget delta: not measured; one existing `oran-tool` TU gained a
  small recursive walk branch. Incremental `test-tool` build completed in
  134.547 s on this environment, which is not the reference hardware.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
