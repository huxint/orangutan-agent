## [2026-05-22 17:00] | Task: Tool workspace file.search adoption

### Execution Context

- Agent: `Claude Code`
- Base model: `Opus 4.7`
- Runtime: `Claude Code CLI`
- Linked plan: none — scoped continuation of spec 0013

### User Query

> Continue advancing the project, one slice per commit; ultrathink.

### Changes Overview

- Areas: `oran-tool`, workspace/path policy, docs/status.
- Key actions: migrated `file.search` to resolve the input path through
  `tool::Workspace::resolve_list` when `DispatchContext::workspace` is
  supplied. The handler keeps its single-file / recursive-walk surface and
  the existing literal/regex matcher; the new resolution step sits between
  `parse_input` and the executor hop so the blocking walk operates on a
  canonical absolute path. Root-side symlinks now route through the same
  workspace policy that already governs `file.read`, narrowing the
  pre-migration root-vs-nested divergence to a single workspace-aware seam;
  nested entries continue to skip symlinks wholesale during the walk, a
  stricter form of the same "symlinks may only follow when they stay inside
  the workspace" rule.

### Design Intent

Spec 0013 keeps closing in narrow slices so neither the docs nor the
review surface balloon. Slice 37 shipped the resolver and migrated
`file.read`; slice 38 migrated the mutating built-ins. This slice picks
up the first read/list-side migration outside `file.read`. The resolve
runs synchronously on the calling strand (same shape as `file.read`),
because the workspace's stat-level operations are cheap relative to the
subsequent walk — and the walk continues to hop to the runtime executor
afterwards. `resolve_list` is the right intent: a search may inspect a
single file or recursively enumerate a directory, both of which match
the read/list policy (follow inside-workspace symlinks, reject escapes).

`directory.list` migration follows in the next slice. Once both read/list
built-ins are workspace-aware, the remaining spec 0013 work narrows to
bootstrap/config ownership, pre-permission resolver boundary, and audit
metadata.

### Files Modified

- `src/oran-tool/file_search.cpp`
- `tests/tool/test_workspace.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 39, new history pointer, narrowed remaining
  workspace work, and refreshed `oran-tool` test counts.
- `docs/ARCHITECTURE.md` — workspace-aware `file.search` in the library
  inventory; `directory.list` is the last remaining raw-path read built-in.
- `docs/design-docs/tool-runtime.md` — slice 39 status note + updated
  "Workspace Handle" section.
- `docs/product-specs/0013-workspace-and-path-policy.md` — shipped/pending
  split records the `file.search` migration; remaining work narrows to
  `directory.list` plus bootstrap/config/audit/pre-permission wiring.
- `docs/SECURITY.md` — workspace-aware tool list extended to
  `file.search`.
- `docs/QUALITY_SCORE.md` — refreshed `oran-tool` test count.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build test-tool`
  - `xmake run test-tool`
- Tests added/changed:
  - `tests/tool/test_workspace.cpp` adds three dispatch regressions for
    workspace `file.search`: relative in-root hit + traversal refusal,
    root-side symlink-escape refusal, and `extra_read_roots` override
    expansion. `tests/tool` reports 109 cases / 911 assertions.
- Bench impact: no new bench scenario; the resolve cost is small relative
  to the existing `bench-tool` walk numbers and the matcher work
  dominates per-call latency.
- Compile-budget delta: not measured. Local `xmake build test-tool` after
  the change linked in 13.6s on this environment (incremental — only
  `file_search.cpp` and the test bucket rebuilt); this is not the
  reference hardware gate.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: no new row. Remaining spec 0013 work is
  `directory.list`, bootstrap/config ownership, audit metadata, and
  moving resolution to the pre-permission dispatch boundary.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
