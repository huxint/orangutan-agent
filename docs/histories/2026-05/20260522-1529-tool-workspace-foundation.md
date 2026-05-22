## [2026-05-22 15:29] | Task: Tool workspace resolver foundation

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none — scoped 0013 foundation slice

### User Query

> Understand the current architecture/progress, continue implementation, then commit
> with a clear standard commit message.

### Changes Overview

- Areas: `oran-tool`, workspace/path policy, docs/status.
- Key actions: added `tool::Workspace` + `ResolvedPath` in a `<filesystem>`-free
  public header, implemented canonical root/override resolution in
  `src/oran-tool/workspace.cpp`, added optional `DispatchContext::workspace`,
  and made `file.read` resolve through it when supplied.

### Design Intent

Spec 0013 is too large for one clean slice, so this lands the reusable resolver and
the first built-in migration without expanding `oran-bootstrap`'s dependency graph
yet. The resolver has intent-specific methods (`resolve_read`, `resolve_write`,
`resolve_delete`, `resolve_list`) so follow-up tool migrations cannot accidentally
apply read semantics to mutating paths. Remaining 0013 work is explicit in
`docs/STATUS.md`: migrate the other filesystem built-ins, move resolution/audit
metadata to the pre-permission boundary, and wire config/bootstrap ownership.

### Files Modified

- `include/oran/tool/workspace.hpp`
- `src/oran-tool/workspace.cpp`
- `include/oran/tool/registry.hpp`
- `include/oran/tool/builtins.hpp`
- `include/oran/tool.hpp`
- `src/oran-tool/file_read.cpp`
- `tests/tool/test_workspace.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 37, next intended workspace migration, tool test counts.
- `docs/ARCHITECTURE.md` — `oran-tool` inventory includes `tool::Workspace`.
- `docs/design-docs/tool-runtime.md` — current workspace seam and slice 37 status.
- `docs/product-specs/0013-workspace-and-path-policy.md` — shipped/pending split.
- `docs/SECURITY.md` — clarifies workspace confinement is partial, not complete.
- `docs/QUALITY_SCORE.md` — tool test counts and workspace surface.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build test-tool`
  - `xmake run test-tool`
- Tests added/changed:
  - `tests/tool/test_workspace.cpp` covers traversal, symlink policy, override
    roots, independent workspace values, and `file.read` dispatch through
    `ctx.workspace`.
- Bench impact: no bench added; this is correctness/security plumbing, not a
  measured hot-path choice.
- Compile-budget delta: not measured. Local `xmake build test-tool` rebuilt the
  touched tool files and linked in 27.815s on this environment; this is not the
  reference hardware gate.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: no new row; the remaining work stays under spec 0013 and
  `docs/STATUS.md`'s next intended slice.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
