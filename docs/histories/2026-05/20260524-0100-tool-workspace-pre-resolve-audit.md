## [2026-05-24 01:00] | Task: Tool workspace pre-resolve audit

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none - scoped completion of spec 0013 v1's registry-boundary
  workspace resolver and audit metadata.

### User Query

> Continue the docs-in-sync close-out for spec 0013.

### Changes Overview

- Areas: `oran-tool`, workspace/path policy, audit metadata, docs/status.
- Key actions:
  - Added the private `src/oran-tool/path_resolution.cpp` helper that
    recognises current filesystem built-ins, parses the `path` field, chooses
    the intent-specific `Workspace` resolver, and emits redacted audit
    metadata.
  - Extended `DispatchContext` with `resolved_path`; `Registry::dispatch`
    clears it on entry, pre-resolves known filesystem paths after
    `tool_before` and before permission evaluation, records
    `metadata_json.path_resolution`, and returns resolver failures before
    handlers run or ask-approval replay is spent.
  - Updated all six filesystem handlers to consume `ctx.resolved_path` when
    present while retaining the previous in-handler workspace fallback for
    workspace-less callers.
  - Added audit/pre-resolution regressions for denied reads, override-root
    metadata, path-policy failure before ask approval, and malformed
    `file.write` options staying on the handler validation path.
  - `xmake run orangutan` now reports `2.0.0-slice55`.

### Design Intent

Spec 0013's remaining v1 gap was not another per-tool migration; it was the
ordering boundary. Path policy has to sit below permissions so a broad allow
rule cannot silently approve an escaped path, while audit still needs to show
the permission context for the failed attempt. The registry boundary is the
smallest shared place that can make that true for all current built-ins without
inventing `tool::Runtime` early.

The pre-resolver deliberately skips malformed JSON, missing `path`, and
malformed optional `file.write` intent fields. That keeps existing handler
schema errors stable instead of turning bad JSON into path-policy errors.
Raw input paths are not stored; audit metadata uses SHA-256 hex hashes for the
input path and workspace root plus relative/display-safe resolver data.

### Files Modified

- `include/oran/tool/registry.hpp`
- `src/oran-tool/_impl/path_resolution.hpp`
- `src/oran-tool/path_resolution.cpp`
- `src/oran-tool/registry.cpp`
- `src/oran-tool/file_read.cpp`
- `src/oran-tool/file_write.cpp`
- `src/oran-tool/file_edit.cpp`
- `src/oran-tool/file_delete.cpp`
- `src/oran-tool/file_search.cpp`
- `src/oran-tool/directory_list.cpp`
- `tests/tool/test_workspace.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 55, new history pointer, refreshed `oran-tool`
  test counts (136 / 1170), and next-intended-slice narrative now treats
  spec 0013 v1 confinement plumbing as closed.
- `docs/ARCHITECTURE.md` - `oran-tool` inventory and slice-status block
  describe registry-boundary pre-resolution and audit metadata.
- `docs/design-docs/tool-runtime.md` - Workspace Handle section records
  `DispatchContext::resolved_path`, `metadata_json.path_resolution`, and the
  handler fallback for workspace-less callers.
- `docs/product-specs/0013-workspace-and-path-policy.md` - Status,
  acceptance criteria, and audit-visibility text record slice 55.
- `docs/SECURITY.md` - workspace-confinement posture now names
  pre-permission path policy and audit metadata as shipped.
- `docs/QUALITY_SCORE.md` - refreshed `oran-tool` test counts and current
  Tool registry state.
- `docs/releases/feature-release-notes.md` - user-visible release note.

### Validation

- Commands run:
  - `xmake build test-tool`
  - `./build/linux/x86_64/release/test-tool "[workspace][audit]"`
  - `xmake run test-tool`
- Tests added/changed:
  - `tests/tool/test_workspace.cpp` adds four `[workspace][audit]` /
    approval regressions. Focused filter reports 4 cases / 66 assertions.
  - Full `test-tool` reports 136 cases / 1170 assertions.
- Bench impact: not measured. This adds one registry-boundary JSON parse for
  workspace-backed filesystem built-ins before the existing permission/audit
  path. A future dispatch bench can pin the exact delta once scheduler work
  starts using the canonical resolved path.
- Compile-budget delta: not measured; the new heavy JSON code is isolated in
  `src/oran-tool/path_resolution.cpp`, not a public header.

### Follow-Ups

- Issues opened: none.
- Tech-debt entries: no new row. Remaining spec 0013 work is v1.1 structure
  (`Workspace::is_ignored`, display helper) and the future capability-gated
  `tool::Runtime::workspace()` accessor.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
