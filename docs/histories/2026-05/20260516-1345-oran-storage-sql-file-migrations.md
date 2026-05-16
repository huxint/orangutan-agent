## [2026-05-16 13:45] | Task: `oran-storage` SQL-file migrations

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local xmake release build, GCC 16.1 baseline
- Linked plan: `docs/exec-plans/completed/2026-05-16-oran-storage-sql-file-migrations.md`

### User Query

> SQL-file migration loading

### Changes Overview

- Areas:
  - `oran-storage` migration runner API.
  - Sessions repository schema loading.
  - Storage migration tests and benchmarks.
  - Storage/memory/state docs and release metadata.
- Key actions:
  - Added `load_migrations_from_directory(std::string_view)` and
    `run_migrations_from_directory(Connection&, std::string_view)`.
  - Implemented strict source-file naming: `0001-name.sql`,
    `0002-next-name.sql`, sorted by version and validated before database
    writes.
  - Moved the sessions initial schema into
    `src/oran-storage/migrations/sessions/0001-sessions-initial.sql`.
  - Rewired `SessionRepository::migrate()` to load the sessions schema from the
    file-backed migration directory. The default resolver walks upward from the
    current directory to support repo-root and xmake build-dir execution, and
    `SessionRepositoryOptions::migrations_directory` provides an explicit
    override for later packaging.
  - Extended `test-storage` and `bench-storage` with file-backed migration
    coverage.

### Design Intent

The storage migration foundation already enforced contiguous versions,
transactional application, idempotent no-op checks, and expected-only errors. This
slice keeps that runner as the single source of truth and adds file loading only as
a feeder API. That avoids duplicating migration semantics while letting future
domain repositories keep DDL in checked-in SQL files instead of embedded C++
strings.

The strict filename convention intentionally rejects malformed regular files in a
migration directory. That makes typoed files fail fast instead of silently dropping
schema changes from a runtime.

### Files Modified

- `include/oran/storage/migrations.hpp`
- `include/oran/storage/session_repository.hpp`
- `src/oran-storage/migrations.cpp`
- `src/oran-storage/session_repository.cpp`
- `src/oran-storage/migrations/sessions/0001-sessions-initial.sql`
- `tests/storage/test_migrations.cpp`
- `bench/storage/scenarios/migrations.cpp`
- `bench/README.md`
- `bench/storage/README.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/memory-system.md`
- `docs/design-docs/secrets-and-state.md`
- `docs/design-docs/storage-runtime.md`
- `docs/product-specs/0005-memory-system.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/storage-runtime.md` — public API, file naming,
  validation/error semantics, session repository migration lookup, and future
  slice list.
- `docs/design-docs/secrets-and-state.md` — storage implementation status and
  migration directory semantics.
- `docs/design-docs/memory-system.md` — sessions schema now loads from the SQL
  migration file.
- `docs/product-specs/0005-memory-system.md` — storage foundation scope now
  names the checked-in sessions migration directory.
- `docs/ARCHITECTURE.md` — storage inventory includes SQL-file migration
  loading.
- `docs/QUALITY_SCORE.md` — storage test/bench coverage and next step updated.
- `bench/README.md` and `bench/storage/README.md` — storage bench comparison
  descriptions include file-backed migrations.
- `docs/releases/feature-release-notes.md` — user-visible slice note.

### Validation

- Commands run:
  ```sh
  xmake build test-storage
  xmake run test-storage
  xmake build bench-storage
  xmake run bench-storage
  xmake test
  xmake build orangutan
  git diff --check
  make ci
  ```
- Tests added/changed:
  - `tests/storage/test_migrations.cpp` adds file-loader coverage for sorted
    load, directory runner idempotence, empty path, missing path, non-directory
    path, empty directory, non-SQL regular files, malformed filenames, gaps,
    duplicate versions, empty SQL files, and "preflight before DB touch".
  - Existing `SessionRepository` tests now exercise the file-backed default
    sessions schema migration, and a new case covers the explicit migration
    directory override.
  - `test-storage` total is now 50 cases / 582 assertions.
- Bench impact:
  - `bench/storage/scenarios/migrations.cpp` adds file-backed migration
    scenarios.
  - Local `xmake run bench-storage` migration results:
    - `storage.migrations_cold_apply`: ~229.8 μs / batch.
    - `storage.migrations_noop_check`: ~3.7 μs / batch
      (nanobench marked this pre-existing fast scenario noisy in this run).
    - `storage.migrations_file_cold_apply`: ~276.8 μs / batch.
    - `storage.migrations_file_noop_check`: ~45.6 μs / batch.
- Compile-budget delta:
  - Existing `src/oran-storage/migrations.cpp` gains filesystem/file-read logic.
    Public header impact is limited to `<string_view>` and two function
    declarations.
  - Existing `src/oran-storage/session_repository.cpp` gains source-tree
    migration directory lookup; the public header adds a small options struct.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
