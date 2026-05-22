## [2026-05-22 17:30] | Task: Tool workspace directory.list adoption

### Execution Context

- Agent: `Claude Code`
- Base model: `Opus 4.7`
- Runtime: `Claude Code CLI`
- Linked plan: none — scoped continuation of spec 0013

### User Query

> Continue advancing the project, one slice per commit; ultrathink.

### Changes Overview

- Areas: `oran-tool`, workspace/path policy, docs/status.
- Key actions: migrated `directory.list` to resolve the input path through
  `tool::Workspace::resolve_list` when `DispatchContext::workspace` is
  supplied. The tool's `oran-io::list_directory` semantics, output shape,
  and option handling are unchanged; the new resolution step sits between
  `parse_input` and the `co_await io::list_directory` call so the listing
  always operates on a canonical absolute path. After this slice every
  filesystem built-in (`file.read`, `file.write`, `file.edit`, `file.delete`,
  `file.search`, `directory.list`) consumes the workspace seam at the
  handler entry, which closes the dispatch-time half of spec 0013.

### Design Intent

Slice 37 shipped the resolver and migrated `file.read`; slice 38 migrated
the mutating built-ins; slice 39 migrated `file.search`. This slice
finishes the per-tool migration by routing the last remaining read/list
built-in through the same seam. `resolve_list` is the right intent:
`directory.list` enumerates directory children, which matches the
follow-inside-workspace / reject-`symlink_escape` rule that already
governs `file.read` and `file.search`.

Remaining spec 0013 work now narrows to *structural* moves rather than
per-tool migration: bootstrap/config ownership for
`permissions.workspace.extra_{read,write}_roots`, moving resolution to the
pre-permission dispatch boundary, and adding the resolved-path metadata
to the audit pipeline. Those land in follow-up slices once the agent loop
foundation work (specs 0011-0018) is closer.

### Files Modified

- `src/oran-tool/directory_list.cpp`
- `tests/tool/test_workspace.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 40, new history pointer, the narrowed remaining
  workspace work (no per-tool migration left), and refreshed `oran-tool`
  test counts.
- `docs/ARCHITECTURE.md` — workspace-aware `directory.list` in the library
  inventory; the "raw-path" callouts are removed because every built-in
  now consumes the seam at handler entry.
- `docs/design-docs/tool-runtime.md` — slice 40 status note + the
  "Workspace Handle" section now reads "every filesystem built-in
  resolves through the seam".
- `docs/product-specs/0013-workspace-and-path-policy.md` — shipped/pending
  split records the `directory.list` migration; remaining work narrows to
  bootstrap/config/audit/pre-permission wiring.
- `docs/SECURITY.md` — workspace-aware tool list extended to
  `directory.list`; the pending bullet now names only the structural
  moves.
- `docs/QUALITY_SCORE.md` — refreshed `oran-tool` test count.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build test-tool`
  - `xmake run test-tool`
  - `scripts/ci.sh`
- Tests added/changed:
  - `tests/tool/test_workspace.cpp` adds two dispatch regressions for
    workspace `directory.list`: relative in-root + traversal refusal, and
    root-side symlink-escape refusal. `tests/tool` reports 111 cases /
    931 assertions.
- Bench impact: no new bench scenario; the resolve cost is small relative
  to the existing `bench/io` directory-iteration numbers.
- Compile-budget delta: not measured. Local `xmake build test-tool` after
  the change linked in 13.4s on this environment (incremental — only
  `directory_list.cpp` and the test bucket rebuilt); this is not the
  reference hardware gate.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: no new row. Remaining spec 0013 work is the
  structural moves (bootstrap/config ownership, audit metadata, and
  resolution at the pre-permission dispatch boundary), all already
  tracked in the spec's Status section.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
