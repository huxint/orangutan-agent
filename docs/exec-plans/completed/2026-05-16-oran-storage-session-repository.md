# `oran-storage` — Session Repository Slice

## Goal

Land the first storage domain repository: a sessions repository that persists
conversation messages into `sessions.db` through `Pool` writer/reader leases and
uses `lease.statement_cache()` for hot SQL. The repository is a storage-layer
foundation for the later `oran-memory::session::Store`; it stores message
payloads as opaque JSON strings until `core::Message` and memory-layer
serialization land.

## Scope

- In scope:
  - Add `include/oran/storage/session_repository.hpp` and
    `src/oran-storage/session_repository.cpp`.
  - Add a `SessionRepository` over `Pool&` with async `migrate`,
    `append_message`, `load_messages`, `get_session`, and `list_sessions`.
  - Add session schema migration version 1 with `sessions` and
    `session_messages` tables plus trigger-maintained session timestamps.
  - Use `Pool::acquire_writer` / `Pool::acquire_reader` and
    `lease.statement_cache().acquire(...)` for repository hot SQL.
  - Re-export the header from `include/oran/storage.hpp`.
  - Add storage tests covering migration idempotence, append/load ordering,
    session listing, agent scoping, and invalid input errors.
  - Add a storage bench comparing raw pool SQL vs. repository append+load on
    the same schema.
  - Update storage/memory docs, architecture, quality score, release notes,
    bench docs, and history in the same change.
- Out of scope:
  - `oran-memory::session::Store` and `core::Message` serialization.
  - Long-term memory, FTS5, decay, hooks, and permissions.
  - SQL-file loading from `src/oran-storage/migrations/`.
  - Config/bootstrap wiring for `sessions.db`.
  - Attachments or conversation DAG tables.

## Context

- Relevant docs:
  - `docs/design-docs/storage-runtime.md`
  - `docs/design-docs/memory-system.md`
  - `docs/design-docs/secrets-and-state.md`
  - `docs/product-specs/0005-memory-system.md`
  - `docs/product-specs/0001-core-react-loop.md`
  - `docs/rules/critical-rules.md`
  - `docs/rules/testing-and-bench.md`
- Relevant code paths:
  - `include/oran/storage/{pool,statement_cache,migrations}.hpp`
  - `src/oran-storage/{pool,statement_cache,migrations}.cpp`
  - `tests/storage/`
  - `bench/storage/`
- Constraints:
  - Public APIs return `core::Result<T>` inside `async::Awaitable`.
  - No JSON dependency in `oran-storage`; `content_json` and `metadata_json`
    remain opaque strings.
  - Public header must not include SQLite or heavy JSON headers.
  - The repository must not bypass `Pool` or use raw connections.
- Compile-budget impact (if any):
  - One public header and one storage implementation TU. The header uses stdlib
    containers/strings plus existing async/result/storage types.
  - One test TU and one bench scenario TU.

## Risks

- Risk: this duplicates future `oran-memory::session::Store` API. Mitigation:
  keep this storage repository payload-oriented and opaque; memory-layer typed
  message serialization wraps it later.
- Risk: append sequence races. Mitigation: writes go through the single pool
  writer; the INSERT computes `MAX(sequence)+1` under that writer path.
- Risk: schema timestamps are too coarse for list ordering. Mitigation: order
  by `updated_at DESC, session_id ASC` for stable ties; tests avoid relying on
  sub-millisecond ordering.
- Risk: public header pulls in too much. Mitigation: forward declare `Pool`
  and include only `awaitable_fwd`, `result`, and `migrations` where needed.

## Milestones

1. Add active plan and settle MVP API.
2. Implement repository schema/API using pool leases and statement caches.
3. Add focused tests.
4. Add raw-pool vs repository bench.
5. Update docs/history/release notes.
6. Run validation and move plan to `completed/`.

## Validation

- Commands:
  - `git diff --check`
  - `xmake build test-storage`
  - `xmake run test-storage`
  - `xmake build bench-storage`
  - `xmake run bench-storage`
  - `xmake test`
  - `xmake build orangutan`
  - `make ci`
- Manual checks:
  - Confirm implementation uses `Pool::acquire_writer` / `acquire_reader`.
  - Confirm hot SQL goes through `lease.statement_cache().acquire`.
  - Confirm docs no longer say all storage domain repositories are downstream.
- Observability checks:
  - Repository exposes stored session/message counts through list/load results;
    no logging/metrics surface in this slice.
- Bench comparison (if perf-relevant):
  - `bench/storage` compares raw pool SQL append+load with
    `SessionRepository` append+load.
  - Local result: `storage.session_raw_pool_append_load` ~698.4 μs per
    64-message batch vs. `storage.session_repository_append_load` ~859.0 μs.

## Progress Log

- [x] Confirm scope and constraints.
- [x] Implement `SessionRepository` public API and storage TU.
- [x] Add storage tests.
- [x] Add storage bench scenario and registration.
- [x] Update docs that this slice invalidates in the same PR.
- [x] Run validation and record results.
- [x] Write history entry.
- [x] Add release note.
- [x] Move plan to `docs/exec-plans/completed/` before finish.

## Decision Log

- 2026-05-16: store `content_json` and `metadata_json` as opaque strings.
  Rationale: `oran-core` does not yet expose `core::Message` / `core::Content`,
  and adding JSON parsing to `oran-storage` would violate the storage boundary.
- 2026-05-16: keep this in `oran-storage` as a domain repository rather than
  `oran-memory`. Rationale: the current objective asks for the first storage
  domain repository; `oran-memory::session::Store` will wrap this API later.

## Linked Artifacts

- Related design doc: `docs/design-docs/storage-runtime.md`
- Related product spec: `docs/product-specs/0005-memory-system.md`
- PRs: local change.
- History entry: `docs/histories/2026-05/20260516-1313-oran-storage-session-repository.md`
- Release note: `docs/releases/feature-release-notes.md`
