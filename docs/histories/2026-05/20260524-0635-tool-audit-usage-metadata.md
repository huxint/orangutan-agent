## [2026-05-24 06:35] | Task: tool audit usage metadata

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `CLI / workspace-write`
- Linked plan: none — slice stayed inside the STATUS next-slice boundary.

### User Query

> Continue advancing the project after reading the docs, keep docs in sync, and
> ship one version per commit.

### Changes Overview

- Areas: `oran-tool`, `oran-permission`, `oran-storage`, docs.
- Key actions: added same-row audit metadata updates, wired successful tool
  usage into `metadata_json.usage`, preserved pre-handler permission decision
  recording, and bumped `orangutan` to slice 67.

### Design Intent

Spec 0014 needed usage counters to fan out beyond hooks without creating
duplicate audit decisions. The chosen shape keeps the existing durable
record-before-handler invariant: `Registry::dispatch` records one
`AuditEvent`, runs the handler only on allow / ask-approved paths, applies
output caps, then best-effort updates that row's metadata when usage is
non-empty. Custom sinks can ignore enrichment through the default no-op
`AuditSink::update_metadata`, while storage-backed runtimes get a precise
`AuditRepository::update_event_metadata` operation.

### Files Modified

- `include/oran/permission/audit.hpp`
- `include/oran/permission/storage_audit_sink.hpp`
- `include/oran/storage/audit_repository.hpp`
- `src/oran-permission/audit.cpp`
- `src/oran-permission/storage_audit_sink.cpp`
- `src/oran-storage/audit_repository.cpp`
- `src/oran-tool/_impl/audit_metadata.hpp`
- `src/oran-tool/audit_metadata.cpp`
- `src/oran-tool/registry.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/permission/test_audit.cpp`
- `tests/storage/test_audit_repository.cpp`
- `tests/tool/test_registry.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — advances to slice 67, links this history, updates test counts, and names provider mapping / scheduler correlation as the remaining work.
- `docs/ARCHITECTURE.md` — updates the storage, permission, and tool inventory rows for metadata update and audit usage enrichment.
- `docs/design-docs/tool-runtime.md` — records direct-dispatch audit usage enrichment after output caps.
- `docs/design-docs/permissions-and-hooks.md` — records `AuditMetadataUpdate` and same-row update semantics.
- `docs/design-docs/storage-runtime.md` — documents `AuditRepository::update_event_metadata`.
- `docs/product-specs/0008-permissions.md` — updates the audit criterion for the new sink/repository metadata update path.
- `docs/product-specs/0012-tool-scheduler-and-state.md` — carries forward the scheduler correlation responsibility for batched calls.
- `docs/product-specs/0014-structured-tool-output.md` — marks audit usage fan-out shipped for direct dispatch.
- `docs/QUALITY_SCORE.md` — updates test counts and current quality notes.
- `docs/releases/feature-release-notes.md` — adds the slice 67 release note.

### Validation

- Commands run:
  - `git diff --check`
  - `make check-docs`
  - `scripts/check-status-fresh.sh`
  - `xmake build orangutan`
  - `xmake run orangutan`
  - `xmake test`
  - `make ci`
  - `xmake build test-storage`
  - `xmake build test-permission`
  - `xmake build test-tool`
  - `xmake run test-storage "[unit][storage][audit_repository]"`
  - `xmake run test-permission "[unit][permission][audit]"`
  - `xmake run test-tool "[unit][tool][hook][output]"`
- Tests added/changed:
  - Storage repository metadata-update regression and validation failure.
  - Recording/storage audit sink metadata-update regressions.
  - Registry audit metadata assertions for usage and output-cap flags.
- Bench impact: no new benchmark; this is metadata plumbing on the existing audit/write path, with no new performance claim.
- Compile-budget delta: no public heavy include added; JSON usage serialization lives in a private `oran-tool` translation unit.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md#2026-05`
