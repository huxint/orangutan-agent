## [2026-05-15 22:30] | Task: `oran-storage` migrations

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI, repo on Linux/WSL2, GCC 16.1.1 system compiler.
- Linked plan: `docs/exec-plans/completed/2026-05-15-oran-storage-migrations.md`

### User Query

> 做 oran-storage migrations slice

### Changes Overview

- Areas: `oran-storage` public API, migration runner, tests/storage, bench/storage,
  storage docs.
- Key actions:
  - Added `storage::Migration`, `storage::MigrationReport`, and
    `storage::run_migrations(Connection&, std::span<const Migration>)`.
  - Added `schema_versions(version, name, applied_at)` setup and validation.
  - Enforced complete contiguous migration sets starting at version `1`.
  - Applied each pending migration in its own `BEGIN IMMEDIATE` transaction.
  - Rolled back failed migrations without recording the failed version.
  - Added idempotent no-op reruns and database-newer-than-binary checks.
  - Bumped the early `orangutan` binary checkpoint to `2.0.0-slice4`.
  - Added `tests/storage` coverage (16 cases / 150 assertions) and a storage migration
    bench.

### Design Intent

The migration runner stays synchronous and builds on the existing `Connection` /
`Statement` API so the future pool can run it on the writer connection without changing
the public contract. The runner requires a complete contiguous migration set instead of
accepting sparse fragments; that makes embedded migration lists self-validating and
prevents ambiguous database states.

### Files Modified

- `include/oran/storage.hpp`, `include/oran/storage/migrations.hpp`
- `src/oran-storage/migrations.cpp`
- `tests/storage/test_migrations.cpp`
- `bench/storage/main.cpp`, `bench/storage/scenarios/migrations.cpp`
- `src/main.cpp`
- Docs listed below.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/storage-runtime.md` — public migration API, schema table, and
  transaction semantics documented.
- `docs/design-docs/secrets-and-state.md` — database migration behavior and
  `schema_versions` shape updated.
- `docs/ARCHITECTURE.md` — `oran-storage` row and slice status updated.
- `docs/QUALITY_SCORE.md` — storage test, bench, and next-step rows updated.
- `docs/rules/testing-and-bench.md`, `docs/product-specs/0010-benchmark-harness.md`,
  `bench/README.md`, `bench/storage/README.md` — migration A-vs-B bench documented.
- `docs/releases/feature-release-notes.md` — storage migration release note added.

### Validation

- Commands run:
  ```sh
  xmake build oran-storage
  xmake run test-storage
  xmake test
  xmake run bench-storage
  xmake build orangutan
  xmake run orangutan
  make ci
  scripts/check-lib-parity.sh
  git diff --check
  xmake f -m release --analyze=y
  xmake build -r oran-storage
  xmake f -m release --analyze=n
  ```
- Tests added/changed:
  - `tests/storage/test_migrations.cpp`: applies pending migrations, reruns no-op,
    rejects invalid migration lists, rolls back failed migrations, rejects newer DBs,
    detects non-contiguous recorded versions, and fails expected-only on read-only DBs
    that need schema setup.
- Bench impact:
  - `bench/storage`: cold migration apply vs. no-op migration check.
  - Latest local run:
    - `storage.migrations_cold_apply`: 260,715.22 ns/batch
    - `storage.migrations_noop_check`: 4,328.80 ns/batch
    - `storage.execute_literal_inserts`: 133,831.05 ns/batch
    - `storage.prepared_statement_inserts`: 65,019.45 ns/batch
  - Nanobench flagged `storage.migrations_cold_apply` as noisy in this local run; the
    command still exited successfully.
- Compile-budget delta:
  - `xmake build oran-storage` completed locally in 0.034 s from a warm release tree.
  - Focused `xmake build -r oran-storage` with GCC `-fanalyzer` completed in 4.071 s.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md` (2026-05 row).
