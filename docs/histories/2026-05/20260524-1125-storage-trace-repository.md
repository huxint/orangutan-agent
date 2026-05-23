## [2026-05-24 11:25] | Task: storage trace repository

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `/home/huxint/projects/orangutan-refactor`
- Linked plan: none — focused spec-0018 storage foundation under the fake-provider-first loop sequencing contract.

### User Query

> Continue the project implementation after reading the current docs and state; keep one coherent version per commit, detailed code comments, and a standard detailed commit message.

### Changes Overview

- Areas: `oran-storage`, spec-0018 trace persistence, storage tests/bench/docs.
- Key actions: added SQLite BLOB bind/read support; added the embedded audit-db
  version-2 `trace_turns` migration and typed `TraceRepository`; bumped the
  binary slice tag to `2.0.0-slice78`.

### Design Intent

Slice 77 gave the agent loop stable `cancellation_phase` error context but had
nowhere durable to write it. This slice deliberately stops at the storage
primitive: `TraceRepository` owns the spec-0018 row shape and 16-byte BLOB id
contract before the agent loop starts threading turn ids through dispatch and
audit. The trace table lives in the existing audit database as migration
version 2, not in a separate migration namespace, so existing version-1 audit
DBs upgrade in place and future audit rows can join the trace row. Keeping the
storage schema separate from loop wiring makes the next slice smaller and lets
`test-storage` prove migration, audit-v1-to-trace-v2 upgrade, BLOB round-trip,
row validation, and query behavior independently.

### Files Modified

- `include/oran/storage/sqlite.hpp`
- `include/oran/storage/trace_repository.hpp`
- `include/oran/storage/migrations.hpp`
- `include/oran/storage.hpp`
- `src/oran-storage/sqlite.cpp`
- `src/oran-storage/trace_repository.cpp`
- `src/oran-storage/migration_assets.cpp`
- `src/oran-storage/migrations/audit/0002-trace-turns-initial.sql`
- `tests/storage/test_sqlite.cpp`
- `tests/storage/test_trace_repository.cpp`
- `tests/bootstrap/test_bootstrap.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`
- `bench/storage/main.cpp`
- `bench/storage/scenarios/trace_repository.cpp`
- `bench/storage/README.md`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/design-docs/permissions-and-hooks.md`
- `docs/design-docs/storage-runtime.md`
- `docs/product-specs/0018-first-loop-observability.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 78, pointed at this history, recorded `test-storage` counts, and named the next trace/audit join slice.
- `docs/ARCHITECTURE.md` — added `TraceRepository`, BLOB support, and the embedded trace migration to the `oran-storage` inventory.
- `docs/design-docs/storage-runtime.md` — documented BLOB APIs, `TraceRepository`, the audit-db version-2 `trace_turns` schema, and the default migration stream.
- `docs/product-specs/0018-first-loop-observability.md` — marked the storage foundation as shipped while keeping loop/audit/hook/CLI work downstream.
- `docs/QUALITY_SCORE.md` — refreshed storage/test counts and added the current storage trace note.
- `docs/releases/feature-release-notes.md` — added the user-visible slice-78 release note.

### Validation

- Commands run:
  - `xmake build test-storage`
  - `xmake run test-storage`
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
  - `xmake build bench-storage`
  - `xmake build orangutan`
  - `xmake run orangutan`
  - `make ci`
  - `git diff --check`
- Tests added/changed: `tests/storage/test_trace_repository.cpp` covers trace migration, explicit migration directories, audit-v1-to-trace-v2 upgrade, append/get/list/count, missing rows, and validation; `tests/storage/test_sqlite.cpp` covers BLOB round-trip; `tests/bootstrap/test_bootstrap.cpp` and `tests/bootstrap/test_runtime_assembly.cpp` assert the operator-facing audit-init / runtime-assembly paths create both `audit_events` and `trace_turns`. `test-storage` reports 68 cases / 827 assertions; `test-bootstrap` reports 48 cases / 173 assertions.
- Bench impact: `bench-storage` gains `storage.trace_raw_pool_insert` vs. `storage.trace_repository_insert`; the bucket builds, but the benchmark was not run in this slice.
- Compile-budget delta: not measured in this slice; the new public header is stdlib-only plus existing async/core/storage façades, and heavy SQLite work stays in `.cpp`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md` row `storage-trace-repository`.
