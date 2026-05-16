# `oran-storage` SQL-file migration loading

## Goal

Ship the file-backed migration loader for `oran-storage`: migration SQL files
under `src/oran-storage/migrations/<db>/` load into the existing
`storage::Migration` model, run through the same expected-only transactional
runner, and replace the embedded sessions schema SQL used by
`SessionRepository::migrate()`.

## Scope

- In scope:
  - Public API to load numbered `.sql` files from a directory into sorted
    `Migration` objects.
  - Public convenience API to run migrations directly from a directory.
  - Strict filename convention for source-tree migration files:
    `0001-slug.sql`, `0002-next-change.sql`, and so on.
  - Move the current sessions initial schema into
    `src/oran-storage/migrations/sessions/0001-sessions-initial.sql`.
  - Wire `SessionRepository::migrate()` through SQL-file loading while preserving
    the existing `SessionRepository(Pool&)` constructor shape.
  - Tests, storage bench coverage, docs, release notes, and a history entry.
- Out of scope:
  - Build/install packaging for migration assets outside the source checkout.
  - Generated schema documentation.
  - New memory/automation/audit domain repositories.
  - Live bootstrap startup wiring for every database file.

## Context

- Relevant docs:
  - `docs/design-docs/storage-runtime.md`
  - `docs/design-docs/secrets-and-state.md`
  - `docs/rules/docs-in-sync.md`
  - `docs/rules/critical-rules.md`
- Relevant code paths:
  - `include/oran/storage/migrations.hpp`
  - `src/oran-storage/migrations.cpp`
  - `src/oran-storage/session_repository.cpp`
  - `tests/storage/test_migrations.cpp`
  - `bench/storage/scenarios/migrations.cpp`
- Constraints:
  - Keep the existing `run_migrations(Connection&, std::span<const Migration>)`
    semantics unchanged.
  - No throwing migration APIs; filesystem failures return `core::Result`
    errors with path context.
  - Public headers stay light; filesystem and file-stream includes remain in
    `.cpp` / test / bench code.
  - File-loaded migrations must still be validated as contiguous from version
    `1` before touching the database.
- Compile-budget impact:
  - One existing storage TU gains filesystem/file-read code. No new public heavy
    dependency beyond `std::string_view`.

## Risks

- Risk: silently skipping malformed or mistyped migration files. Mitigation:
  reject any regular file in the migration directory that does not match the
  numbered `.sql` convention.
- Risk: session repository becomes source-CWD dependent. Mitigation: keep the
  no-argument constructor default aligned with the checked-in source tree and add
  a migration-directory override option for bootstrap/install wiring later.
- Risk: file loading weakens migration invariants. Mitigation: reuse the same
  validation path that `run_migrations` uses and cover gaps, bad names, missing
  directories, and empty SQL files in `test-storage`.

## Milestones

1. Add file-loader parsing and convenience runner.
2. Move sessions schema SQL into the migration directory and wire
   `SessionRepository::migrate()`.
3. Add/extend tests and storage migration benchmarks.
4. Update storage docs, quality score, release notes, and history.
5. Run validation and commit the slice.

## Validation

- Commands:
  - `xmake build test-storage`
  - `xmake run test-storage`
  - `xmake build bench-storage`
  - `xmake run bench-storage`
  - `xmake test`
  - `xmake build orangutan`
  - `git diff --check`
  - `make ci`
- Manual checks:
  - Confirm `SessionRepository::migrate()` no longer embeds the sessions schema
    SQL and calls the SQL-file runner.
  - Confirm migration filename parsing rejects malformed source files before
    database writes.
- Observability checks:
  - N/A; this slice does not add runtime logging or metrics.
- Bench comparison:
  - Extend storage migration bench with compiled-span apply/no-op vs.
    file-directory load+apply/no-op.
  - Local result: compiled cold apply ~229.8 μs, compiled no-op ~3.7 μs,
    file cold apply ~276.8 μs, file no-op ~45.6 μs per batch. The compiled
    no-op scenario was marked noisy by nanobench in this run; the process still
    completed successfully.

## Progress Log

- [x] Confirm scope and constraints from the storage docs and current migration
  runner.
- [x] Implement SQL-file loading and directory runner APIs.
- [x] Move sessions schema SQL into `src/oran-storage/migrations/sessions/`.
- [x] Add tests for loading, parsing failures, empty SQL, missing directories,
  and session repository file-backed migration.
- [x] Extend storage migration bench with file-backed scenarios.
- [x] Update docs and quality/release metadata.
- [x] Run validation and record results.
- [x] Move this plan to `docs/exec-plans/completed/`.
- [x] Write history entry with docs and validation evidence.

## Decision Log

- 2026-05-16: Keep `run_migrations(Connection&, span)` as the core runner and
  add file loading as a feeder API, so existing tests and repositories keep the
  same transactional semantics.
- 2026-05-16: Use `0001-slug.sql` source filenames and reject malformed regular
  files, so typos do not silently reduce the migration set.
- 2026-05-16: Give `SessionRepository` an optional migrations-directory override
  while preserving `SessionRepository(Pool&)`, so source-tree defaults work now
  and bootstrap/install packaging can pass a different root later.

## Linked Artifacts

- Related design doc: `docs/design-docs/storage-runtime.md`
- Related product spec:
- PRs:
- History entry:
  `docs/histories/2026-05/20260516-1345-oran-storage-sql-file-migrations.md`
- Release note: `docs/releases/feature-release-notes.md`
