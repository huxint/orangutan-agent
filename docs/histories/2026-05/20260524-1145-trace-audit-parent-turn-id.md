## [2026-05-24 11:45] | Task: trace audit parent turn id

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `/home/huxint/projects/orangutan-refactor`
- Linked plan: none — focused spec-0018 cause-chain slice under the fake-provider-first loop sequencing contract.

### User Query

> Continue the project implementation after reading the current docs and state; keep one coherent version per commit, detailed code comments, and a standard detailed commit message.

### Changes Overview

- Areas: `oran-core`, `oran-storage`, `oran-permission`, `oran-tool`, `oran-agent`, spec-0018 trace/audit correlation.
- Key actions: added the shared `core::TurnId` value type; added the audit DB version-3 `audit_events.parent_turn_id` column; threaded optional parent turn ids through storage audit requests, permission audit events, storage sinks, registry dispatch, and the direct fake-provider agent loop; bumped the binary slice tag to `2.0.0-slice79`.

### Design Intent

Slice 78 made `trace_turns` durable, but tool audit rows still had no typed join key back to the future per-turn row. This slice deliberately threads only the join primitive through the pre-scheduler direct-dispatch path: callers may provide `RunTurnInputs::turn_id`, `agent::Loop` temporarily installs it on the reusable `tool::DispatchContext`, and `Registry::dispatch` records it on the permission audit row and later metadata update. When no turn id is supplied, the loop clears any stale context value for the dispatch duration so trace-disabled callers keep `audit_events.parent_turn_id = NULL`. The loop-owned trace writer, hook publish rows, trace config, and `--trace` inspector remain downstream.

### Files Modified

- `include/oran/core/turn_id.hpp`
- `include/oran/agent/loop.hpp`
- `include/oran/permission/audit.hpp`
- `include/oran/storage/audit_repository.hpp`
- `include/oran/storage/trace_repository.hpp`
- `include/oran/tool/registry.hpp`
- `src/oran-agent/loop.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-permission/audit.cpp`
- `src/oran-permission/storage_audit_sink.cpp`
- `src/oran-storage/audit_repository.cpp`
- `src/oran-storage/migration_assets.cpp`
- `src/oran-storage/migrations/audit/0003-audit-parent-turn-id.sql`
- `src/oran-tool/registry.cpp`
- `tests/core/test_turn_id.cpp`
- `tests/agent/test_loop.cpp`
- `tests/permission/test_audit.cpp`
- `tests/storage/test_audit_repository.cpp`
- `tests/storage/test_trace_repository.cpp`
- `tests/tool/test_registry.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/design-docs/agent-platform.md`
- `docs/design-docs/permissions-and-hooks.md`
- `docs/design-docs/storage-runtime.md`
- `docs/design-docs/tool-runtime.md`
- `docs/product-specs/0017-fake-provider-first-agent-loop.md`
- `docs/product-specs/0018-first-loop-observability.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 79, pointed at this history, recorded the new focused test counts, and named the remaining trace writer / CLI / hook / config work.
- `docs/ARCHITECTURE.md` — added `core::TurnId`, the audit parent-turn column, and the loop/registry propagation seam to the current inventory.
- `docs/design-docs/agent-platform.md` — documented `RunTurnInputs::turn_id` as the interim trace/audit join primitive for direct dispatch.
- `docs/design-docs/permissions-and-hooks.md` — documented typed parent turn ids on audit events, metadata updates, storage sinks, and the version-3 audit migration.
- `docs/design-docs/storage-runtime.md` — documented `parent_turn_id` in audit append/list/update records, the version-3 schema, validation, and metadata-update scoping.
- `docs/design-docs/tool-runtime.md` — documented `DispatchContext::parent_turn_id` and its audit usage-enrichment correlation role.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — marked the direct-loop audit join side as shipped while keeping loop-owned turn rows downstream.
- `docs/product-specs/0018-first-loop-observability.md` — marked the audit-row side of the cause-chain join as shipped and kept trace row writes / CLI inspection downstream.
- `docs/QUALITY_SCORE.md` — refreshed the affected test counts and next-step notes.
- `docs/releases/feature-release-notes.md` — added the slice-79 release note.

### Validation

- Commands run:
  - `xmake run test-core`
  - `xmake run test-storage`
  - `xmake run test-permission`
  - `xmake run test-tool`
  - `xmake run test-agent`
  - `xmake build orangutan`
  - `xmake run orangutan`
  - `make ci`
  - `git diff --check`
- Tests added/changed: `tests/core/test_turn_id.cpp` covers the shared value type; storage tests cover version-3 migration, BLOB round-trip, parent-turn-scoped metadata updates, and zero-id rejection; permission tests cover recording/storage sink propagation and update scoping; tool tests cover registry audit stamping; agent tests cover direct-loop stamping plus stale-context clearing/restoration. Focused counts: `test-core` 70 cases / 453 assertions, `test-storage` 70 cases / 856 assertions, `test-permission` 89 cases / 414 assertions, `test-tool` 166 cases / 1590 assertions, and `test-agent` 14 cases / 162 assertions.
- Bench impact: no new benchmark; this slice adds one optional 16-byte BLOB bind/read on audit rows and no new hot-loop algorithm.
- Compile-budget delta: not measured in this slice; the new public header is `<algorithm>` / `<array>` / `<cstddef>` only and heavy SQLite work remains in `.cpp`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md` row `trace-audit-parent-turn-id`.
