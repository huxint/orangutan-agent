# `oran-storage` — Migrations

## Goal

Land the expected-only migration runner on top of the SQLite core. This slice gives
future config, session, memory, automation, and audit repositories a shared
`schema_versions` contract, monotonic migration validation, transactional apply, and
idempotent no-op reruns.

## Scope

- In scope:
  - Add public migration types under `include/oran/storage/` and export them from
    `include/oran/storage.hpp`.
  - Implement `run_migrations(Connection&, std::span<const Migration>)`.
  - Create and maintain `schema_versions` inside each database.
  - Validate migration lists before applying them: positive versions, version `1`
    start, contiguous monotonic order, non-empty names, and non-empty SQL.
  - Verify existing database versions are contiguous and do not exceed the provided
    migration set.
  - Apply each pending migration in its own transaction and roll back a failed
    migration without recording the version.
  - Add storage tests and a storage migration bench comparison.
  - Update storage docs, benchmark docs, quality score, release notes, and history.
- Out of scope:
  - Async connection pool / writer strand.
  - Loading SQL files from `src/oran-storage/migrations/`.
  - Domain schemas for sessions, memory, automation, or audit logs.
  - Prepared statement cache.
  - Backup tooling.

## Context

- Relevant docs:
  - `docs/ARCHITECTURE.md`
  - `docs/design-docs/storage-runtime.md`
  - `docs/design-docs/secrets-and-state.md`
  - `docs/rules/error-handling.md`
  - `docs/rules/testing-and-bench.md`
  - `docs/rules/docs-in-sync.md`
- Relevant code paths:
  - `include/oran/storage/*`, `src/oran-storage/*`
  - `tests/storage/*`, `bench/storage/*`
  - `docs/histories/2026-05/`, `docs/releases/feature-release-notes.md`
- Constraints:
  - Public APIs return `core::Result<T>` only; no throwing wrappers and no `must_ok`.
  - Migration code uses the existing `Connection` / `Statement` surface and does not
    expose SQLite C API from public headers.
  - This slice stays synchronous; later pools can call it on the writer connection.
- Compile-budget impact:
  - Adds a small non-SQLite-heavy storage TU. The `oran-storage` per-TU hard cap remains
    2.5 s.

## Risks

- Risk: partially applied migration leaves user DB in a split state. Mitigation: one
  explicit transaction per migration with rollback on failure and tests for rollback.
- Risk: a caller passes a partial or reordered migration list. Mitigation: validate
  positive, contiguous versions starting at `1` before touching the DB.
- Risk: database state is ahead of the binary. Mitigation: report a storage conflict
  when recorded versions exceed the provided migration set.

## Milestones

1. Add active plan and public API shape.
2. Implement migration validation, schema-version reads, transactional apply, and report.
3. Add storage tests for apply, no-op rerun, validation failures, rollback, newer DB, and
   read-only failure.
4. Add storage migration bench: cold apply vs. no-op schema check.
5. Update docs, quality score, release notes, and history.
6. Run validation gates, audit against the objective, and move this plan to completed.

## Validation

- Commands:
  - `make ci`
  - `xmake build oran-storage`
  - `xmake build orangutan`
  - `xmake run test-storage`
  - `xmake test`
  - `xmake run bench-storage`
  - `xmake f -m release --analyze=y`
  - `xmake build -r oran-storage`
  - `xmake f -m release --analyze=n`
  - `scripts/check-lib-parity.sh`
  - `git diff --check`
- Manual checks:
  - Public docs match shipped signatures.
  - `git status --short --ignored` only shows expected ignored generated files.
  - Public headers do not include `<sqlite3.h>`.
- Observability checks: none yet; `oran-log` is not implemented.
- Bench comparison:
  - `bench/storage` compares cold migration apply vs. no-op migration check.

## Progress Log

- [x] Confirm scope: synchronous migration runner, not pool or file-loader.
- [x] Implement migration public API and runner.
- [x] Add tests and bench.
- [x] Update docs that this slice invalidates in the same PR
      (`docs/rules/docs-in-sync.md`).
- [x] Run validation and record results.
- [x] Update `docs/QUALITY_SCORE.md`.
- [x] Write history entry.
- [x] Add release note.
- [x] Move this plan to `docs/exec-plans/completed/` before finish.

## Decision Log

- 2026-05-15: Keep migrations synchronous and connection-local. Rationale: the future
  pool can run migrations through the writer connection without changing the migration
  contract.
- 2026-05-15: Require a complete contiguous migration set starting at version `1`.
  Rationale: this catches partial embedded migration lists before they can create an
  ambiguous database state.

## Linked Artifacts

- Related design doc: `docs/design-docs/storage-runtime.md`,
  `docs/design-docs/secrets-and-state.md`
- Related product spec:
- PRs:
- History entry: `docs/histories/2026-05/20260515-2230-oran-storage-migrations.md`
- Release note: `docs/releases/feature-release-notes.md`
