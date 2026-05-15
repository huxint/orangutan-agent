# `oran-storage` — SQLite Core

## Goal

Land the first expected-only SQLite foundation for Orangutan v2. This slice ships
`oran-storage` with a move-only `Connection`, move-only `Statement`, SQLite error
mapping into `core::Result<T>`, WAL/foreign-key setup, and a query helper that is
sufficient for config, memory, sessions, and automation to build on later.

## Scope

- In scope:
  - Add `sqlite3 3.51.0+0` to xmake package requirements and keep package docs/lockfile
    aligned.
  - Add public headers under `include/oran/storage/` plus umbrella
    `include/oran/storage.hpp`.
  - Implement `Connection::open`, `execute`, `prepare`, `query`, and `close`.
  - Implement `Statement` binding, stepping, reset/clear-bindings, and simple column
    readers.
  - Configure `busy_timeout`, `PRAGMA foreign_keys = ON`, and file-backed
    `PRAGMA journal_mode = WAL` from `ConnectionOptions`.
  - Add `tests/storage` and `bench/storage`.
  - Update storage/build/test/bench docs, release notes, and history.
- Out of scope:
  - Async connection pool / writer strand.
  - Migration runner and schema version tracking.
  - Prepared statement cache.
  - Domain repositories for sessions, memory, automation, or audit logs.
  - Backup tooling.

## Context

- Relevant docs:
  - `docs/ARCHITECTURE.md`
  - `docs/design-docs/secrets-and-state.md`
  - `docs/design-docs/module-boundaries.md`
  - `docs/rules/error-handling.md`
  - `docs/rules/libraries.md`
  - `docs/rules/testing-and-bench.md`
  - `docs/rules/docs-in-sync.md`
- Relevant code paths:
  - `xmake/{packages,targets,tests,bench}.lua`, `xmake-requires.lock`
  - New `include/oran/storage/*`, `src/oran-storage/*`
  - New `tests/storage/*`, `bench/storage/*`
- Constraints:
  - Public APIs return `core::Result<T>` only; no throwing wrappers and no `must_ok`.
  - Public headers do not include `<sqlite3.h>`; the SQLite handle is hidden behind
    pimpl.
  - `sqlite3` use stays inside `oran-storage`.
  - This slice stays synchronous; later async pools wrap this core behind strands.
- Compile-budget impact:
  - `oran-storage` has a 1.0 s median / 2.0 s p95 / 2.5 s hard cap per TU. SQLite
    includes are confined to one implementation file for the MVP.

## Risks

- Risk: `sqlite3` package fetch/version shape differs from docs. Mitigation: pin the
  documented version and update `xmake-requires.lock`; if xmake resolves a different
  published version, update docs in the same change.
- Risk: statement lifetime can outlive the connection object. Mitigation: internal
  shared DB handle keeps SQLite alive until all statements finalize.
- Risk: WAL cannot be enabled on in-memory databases. Mitigation: `enable_wal` is an
  option and tests verify WAL on a file-backed temp DB.

## Milestones

1. Add active plan and package/build wiring.
2. Implement `Connection` / `Statement` core.
3. Add storage tests and prepared-vs-direct bench.
4. Update production docs, quality score, release notes, and history.
5. Run validation gates, review generated files, and move the plan to `completed/`.

## Validation

- Commands:
  - `make ci`
  - `xmake f -m release`
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
  - `git status --short --ignored` shows only expected generated files ignored.
  - Public docs match shipped signatures.
- Observability checks: none yet; `oran-log` is not implemented.
- Bench comparison:
  - `bench/storage` compares literal `execute` inserts vs. prepared statement binding.

## Progress Log

- [x] Confirm scope: expected-only SQLite core, not full pool/migrations.
- [x] Implement the storage core.
- [x] Update docs that this slice invalidates in the same PR
      (`docs/rules/docs-in-sync.md`).
- [x] Run validation and record results.
- [x] Update `docs/QUALITY_SCORE.md`.
- [x] Write history entry.
- [x] Add release note.
- [x] Move this plan to `docs/exec-plans/completed/` before finish.

## Decision Log

- 2026-05-15: Start storage with synchronous SQLite core. Rationale: the future pool,
  writer strand, migrations, and caches need a small, tested, expected-only primitive
  underneath them.
- 2026-05-15: Keep statement lifetime safe with a shared internal DB handle. Rationale:
  callers can move statements independently without relying on connection object
  lifetime.
- 2026-05-15: Pin `sqlite3 3.51.0+0`, not the previously planned 3.52.0. Rationale:
  the locally available xmake-repo metadata resolves the current selectable SQLite
  package to 3.51.0+0.

## Linked Artifacts

- Related design doc: `docs/design-docs/storage-runtime.md`,
  `docs/design-docs/secrets-and-state.md`
- Related product spec:
- PRs:
- History entry: `docs/histories/2026-05/20260515-2152-oran-storage-sqlite-core.md`
- Release note: `docs/releases/feature-release-notes.md`
