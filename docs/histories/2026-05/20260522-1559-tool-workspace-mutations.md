## [2026-05-22 15:59] | Task: Tool workspace mutating built-ins

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none — scoped continuation of spec 0013

### User Query

> Continue implementing the next development stage.

### Changes Overview

- Areas: `oran-tool`, workspace/path policy, docs/status.
- Key actions: migrated `file.write`, `file.edit`, and `file.delete` to resolve
  through `DispatchContext::workspace` when supplied. The tools still keep their
  existing JSON surfaces and `oran-io` semantics; the change is that relative
  workspace paths, traversal rejection, and mutating-symlink rejection now share
  the `tool::Workspace` policy before any write/edit/delete reaches the I/O
  layer.

### Design Intent

Spec 0013 is being closed in narrow slices to keep the build and docs legible.
Slice 37 landed the resolver and the first read-side adoption. This slice moves
the mutating filesystem tools to the same policy seam without taking on the
larger pre-permission dispatch boundary or bootstrap/config ownership. `file.write`
passes its parsed mode and parent-creation flag into `WriteIntent`; `file.edit`
uses `resolve_write` before its read+atomic rewrite because the operation is
effectful; `file.delete` uses `resolve_delete` before unlinking.

### Files Modified

- `src/oran-tool/file_write.cpp`
- `src/oran-tool/file_edit.cpp`
- `src/oran-tool/file_delete.cpp`
- `tests/tool/test_workspace.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 38, new history pointer, next intended workspace
  migration, and `oran-tool` test counts.
- `docs/ARCHITECTURE.md` — workspace-aware mutating built-ins in the current
  library inventory.
- `docs/design-docs/tool-runtime.md` — slice 38 workspace status and remaining
  raw-path built-ins.
- `docs/product-specs/0013-workspace-and-path-policy.md` — shipped/pending split
  now names the mutating-tool migration.
- `docs/SECURITY.md` — clarifies that read/write/edit/delete are workspace-aware
  only when the runtime supplies the workspace seam; search/list and bootstrap
  wiring remain pending.
- `docs/QUALITY_SCORE.md` — refreshed tool test count.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build test-tool`
  - `xmake run test-tool`
- Tests added/changed:
  - `tests/tool/test_workspace.cpp` adds dispatch regressions for workspace
    `file.write`, `file.edit`, and `file.delete`: relative in-root success,
    traversal refusal, and mutating symlink refusal.
- Bench impact: no bench added; this is correctness/security plumbing.
- Compile-budget delta: not measured. Local `xmake build test-tool` rebuilt the
  touched tool files and linked in 33.645s on this environment; this is not the
  reference hardware gate.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: no new row. Remaining spec 0013 work is `file.search`,
  `directory.list`, bootstrap/config ownership, audit metadata, and moving
  resolution to the pre-permission dispatch boundary.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
