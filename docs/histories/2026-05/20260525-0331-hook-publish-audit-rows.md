## [2026-05-25 03:31] | Task: Hook Publish Audit Rows

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `CLI`
- Linked plan: none; this followed the spec-0015/0018 sequencing contract already recorded in `docs/STATUS.md`.

### User Query

> Continue understanding the architecture and implementation state, then keep advancing the project one version per commit with docs kept in sync.

### Changes Overview

- Areas: storage audit schema, permission audit propagation, direct tool dispatch, bootstrap trace inspection, docs.
- Key actions: added audit DB migration version 4 for `audit_events.event_kind`, threaded `event_kind` through storage/permission audit APIs, wrote traced blocking `tool_before` `hook_publish` rows from `Registry::dispatch`, and made `--trace` print each joined audit row kind.

### Design Intent

Spec 0018 AC5 needs hook publishes to be visible in the same cause-chain query as trace rows and permission decisions. Reusing `audit_events` keeps the operator join simple, but it needs an explicit row discriminator so normal permission-row metadata enrichment cannot accidentally update a same-tool hook row. The slice therefore adds `event_kind` as an additive schema migration with a default of `permission_decision`, records `hook_publish` rows only when a traced dispatch consulted blocking sinks, and keeps the ordinary permission-decision row as the durable permission record.

### Files Modified

- `src/oran-storage/migrations/audit/0004-audit-event-kind.sql`
- `include/oran/storage/audit_repository.hpp`
- `src/oran-storage/audit_repository.cpp`
- `src/oran-storage/migration_assets.cpp`
- `include/oran/permission/audit.hpp`
- `src/oran-permission/audit.cpp`
- `src/oran-permission/storage_audit_sink.cpp`
- `include/oran/tool/registry.hpp`
- `src/oran-tool/_impl/audit_metadata.hpp`
- `src/oran-tool/audit_metadata.cpp`
- `src/oran-tool/registry.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/storage/test_audit_repository.cpp`
- `tests/storage/test_trace_repository.cpp`
- `tests/tool/test_registry.cpp`
- `tests/bootstrap/test_bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped slice 93, pointed at this history, and refreshed validation counts/open hook debt.
- `docs/ARCHITECTURE.md` — updated storage, permission, hook, and bootstrap inventory rows for `event_kind` and trace output.
- `docs/QUALITY_SCORE.md` — refreshed storage/tool/bootstrap/hook summaries and focused test counts.
- `docs/design-docs/bootstrap-runtime.md` — documented `--trace` joined audit row kind output.
- `docs/design-docs/permissions-and-hooks.md` — updated hook-bus status for slice 93 hook-publish rows.
- `docs/design-docs/storage-runtime.md` — documented audit DB version 4 and `event_kind` matching/filtering.
- `docs/design-docs/tool-runtime.md` — documented direct-dispatch hook-publish row emission.
- `docs/product-specs/0015-blocking-hook-decisions.md` — narrowed remaining v1 work to the operator-prompt sink.
- `docs/product-specs/0018-first-loop-observability.md` — marked AC5 and mixed-row trace inspection shipped for direct dispatch.
- `docs/exec-plans/tech-debt-tracker.md` — removed the hook-publish writer from the 2026-05-18 hook follow-up row.
- `docs/releases/feature-release-notes.md` — added the slice 93 user-facing release note.

### Validation

- Commands run:
  - `xmake run test-storage`
  - `xmake run test-permission`
  - `xmake run test-tool`
  - `xmake run test-bootstrap`
  - `make ci`
- Tests added/changed: storage migration/event-kind filtering/join tests, tool traced hook-publish row tests, bootstrap trace-output fixture coverage.
- Bench impact: none; one extra audit insert occurs only for traced direct dispatches with consulted blocking sinks.
- Compile-budget delta: not measured; public header additions are small strings/options and no new heavy includes were added.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: 2026-05-18 hook row now leaves only the `permission_ask_rendered` operator-prompt sink.
- Linked release note: `docs/releases/feature-release-notes.md#2026-05`
