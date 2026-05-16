## [2026-05-16 13:13] | Task: `oran-storage` session repository

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI, repo on Linux/WSL2, GCC 16.1.1 system compiler.
- Linked plan: `docs/exec-plans/completed/2026-05-16-oran-storage-session-repository.md`

### User Query

> implement the first storage domain repository, starting with sessions, using
> Pool writer/reader leases plus lease.statement_cache() for hot SQL.

### Changes Overview

- Areas: `oran-storage` public API, sessions schema, repository
  implementation, storage tests, storage bench, design docs, memory/product
  docs, architecture map, quality score, release notes.
- Key actions:
  - Added `SessionRepository` over `Pool&` with async `migrate`,
    `append_message`, `load_messages`, `get_session`, and `list_sessions`.
  - Added `SessionKey`, `AppendSessionMessageRequest`,
    `SessionMessageRecord`, `SessionRecord`, and `ListSessionsOptions`.
  - Added migration `1 / sessions_initial` creating `sessions`,
    `session_messages`, an agent/update index, and an insert trigger that
    creates/touches session rows when messages append.
  - Implemented hot-path SQL through `Pool::acquire_writer` /
    `Pool::acquire_reader` and `lease.statement_cache().acquire(...)`.
  - Added tests for migration idempotence, append/load ordering,
    get/list/session counts, agent scoping, missing sessions, and invalid
    inputs.
  - Added a bench comparing raw pool+cache SQL append/load to the repository
    wrapper.

### Design Intent

This is the first storage domain repository promised by
`docs/design-docs/storage-runtime.md`. It intentionally lives in
`oran-storage`, not `oran-memory`: the storage layer owns SQLite schema,
migrations, pooling, and prepared-statement reuse, while the later
`oran-memory::session::Store` will handle typed `core::Message` serialization.

Because `core::Message` / `core::Content` are not implemented yet, the
repository stores `content_json` and `metadata_json` as opaque strings. This
keeps `oran-storage` free of a JSON dependency and still gives the memory layer
a stable append/load/list substrate.

### Files Modified

- `include/oran/storage/session_repository.hpp` (new)
- `include/oran/storage.hpp`
- `src/oran-storage/session_repository.cpp` (new)
- `tests/storage/test_session_repository.cpp` (new)
- `bench/storage/scenarios/session_repository.cpp` (new)
- `bench/storage/main.cpp`
- Docs listed below.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/storage-runtime.md` — updated storage status/future slices
  and added the `SessionRepository` API/schema/semantics section.
- `docs/design-docs/memory-system.md` — noted that the storage foundation is
  implemented and the typed memory store remains downstream.
- `docs/design-docs/secrets-and-state.md` — updated storage API inventory and
  pool status.
- `docs/product-specs/0005-memory-system.md` — distinguished the shipped
  storage repository from the future typed memory store.
- `docs/ARCHITECTURE.md` — updated storage slice status and inventory row.
- `docs/QUALITY_SCORE.md` — updated storage/test/bench rows and next step.
- `bench/README.md` and `bench/storage/README.md` — updated storage A-vs-B
  scenario documentation.
- `docs/releases/feature-release-notes.md` — added the
  storage-session-repository row.
- `docs/exec-plans/active/2026-05-16-oran-storage-session-repository.md` →
  moved to `docs/exec-plans/completed/2026-05-16-oran-storage-session-repository.md`.

### Validation

- Commands run:
  ```sh
  xmake build test-storage
  xmake build bench-storage
  xmake run test-storage "SessionRepository list_sessions is scoped by agent and honors limits"
  xmake run test-storage
  xmake run bench-storage
  xmake test
  xmake build orangutan
  git diff --check
  make ci
  ```
- Tests added/changed:
  - `tests/storage/test_session_repository.cpp`: 5 cases covering migration
    idempotence, append/load round-trip ordering, get/list counts, agent
    scoping, missing sessions, and invalid inputs.
  - `test-storage` total grew from 40 cases / 403 assertions to 45 cases /
    474 assertions.
- Bench impact:
  - `bench/storage` adds two scenarios:
    - `storage.session_raw_pool_append_load`: ~698.4 μs / 64-message batch.
    - `storage.session_repository_append_load`: ~859.0 μs / 64-message batch.
  - The repository wrapper adds roughly 23% over raw pool SQL in the local
    microbench while centralizing validation, row mapping, and async API shape.
- Compile-budget delta:
  - One public header and one storage TU. The header keeps SQL, row mapping,
    and SQLite details out of the public surface.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Next suggested slice: wire `oran-memory::session::Store` on top of
  `SessionRepository` once the `core::Message` / `core::Content` surface lands,
  or add SQL-file migration loading before more domain repositories.
- Linked release note: `docs/releases/feature-release-notes.md` (2026-05-16
  storage-session-repository row).
