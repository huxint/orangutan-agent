## [2026-05-15 21:52] | Task: `oran-storage` SQLite core

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI, repo on Linux/WSL2, GCC 16.1.1 system compiler.
- Linked plan: `docs/exec-plans/completed/2026-05-15-oran-storage-sqlite-core.md`

### User Query

> /goal 实现 oran-storage expected-only SQLite core

### Changes Overview

- Areas: `oran-storage` library, SQLite package wiring, tests/storage, bench/storage,
  storage/build/docs.
- Key actions:
  - Added `oran-storage` with move-only `Connection` and `Statement` types.
  - Added expected-only APIs for open/execute/prepare/query, bind/step/reset, and
    text/int/double column reads.
  - Added SQLite error mapping to `core::ErrorKind::storage` with SQLite code,
    extended code, message, and SQL context.
  - Enabled busy timeout, foreign keys, and verified WAL by default for file-backed
    writable DBs.
  - Added statement row-position checks so column readers fail outside an active row.
  - Added `sqlite3 3.51.0+0` to xmake requirements and lockfile.
  - Added `tests/storage` (9 cases / 95 assertions) and `bench/storage`.

### Design Intent

Storage is a platform primitive for config, sessions, memory, automation, and audit
logs, so this slice starts with the smallest reusable SQLite core rather than jumping
straight to repositories or migrations. The public API exposes no `sqlite3.h` and no
throwing wrappers; future pooling, migration, and domain layers can compose around the
same `Result<T>` surface.

Statements retain a shared internal DB handle so callers can move a statement
independently of the `Connection` object that created it. This prevents a common
lifetime pitfall without exposing shared ownership in the public API.

### Files Modified

- `include/oran/storage.hpp`, `include/oran/storage/sqlite.hpp`
- `src/oran-storage/sqlite.cpp`
- `tests/storage/{main,test_sqlite}.cpp`
- `bench/storage/{README,main}.cpp`, `bench/storage/scenarios/sqlite_insert.cpp`
- `xmake/packages.lua`, `xmake/targets.lua`, `xmake/tests.lua`, `xmake/bench.lua`,
  `xmake-requires.lock`
- `src/main.cpp`
- Docs listed below.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/storage-runtime.md` — new canonical storage API/lifetime/error doc.
- `docs/design-docs/index.md` — catalogue entry for `storage-runtime.md`.
- `docs/design-docs/secrets-and-state.md` — expected-only API status and pool future
  boundary updated.
- `docs/ARCHITECTURE.md` — slice status and `oran-storage` row updated.
- `docs/BUILD_SYSTEM.md` — package list and target sample include SQLite/storage.
- `docs/rules/libraries.md` — SQLite package version aligned to xmake lock.
- `docs/QUALITY_SCORE.md` — storage/test/bench/compile rows updated.
- `docs/rules/testing-and-bench.md`, `docs/product-specs/0010-benchmark-harness.md`,
  `bench/README.md`, `bench/storage/README.md` — storage A-vs-B bench documented.
- `tests/README.md` — storage test bucket status updated.
- `include/README.md`, `src/README.md` — live library status corrected.
- `docs/releases/feature-release-notes.md` — storage release note added.

### Validation

- Commands run:
  ```sh
  xmake f -m release -y
  xmake build oran-storage
  xmake build orangutan
  xmake run orangutan
  xmake run test-storage
  xmake test
  xmake run bench-storage
  xmake f -m release --analyze=y
  xmake build -r oran-storage
  xmake f -m release --analyze=n
  make ci
  scripts/check-lib-parity.sh
  git diff --check
  ```
- Tests added/changed:
  - `tests/storage/test_sqlite.cpp`: 9 cases / 95 assertions covering file-backed
    open/verified WAL/foreign-keys, invalid inputs, execute/query round trip, prepared
    binds, column-reader row-position checks, reset/reuse, SQLite failure mapping,
    statement lifetime after connection close, and read-only write failure.
- Bench impact:
  - `bench/storage`: literal `Connection::execute` inserts vs. prepared
    `Statement` binding.
  - Latest local run:
    - `storage.execute_literal_inserts`: 118,998.12 ns/batch
    - `storage.prepared_statement_inserts`: 57,533.24 ns/batch
- Compile-budget delta:
  - `xmake build oran-storage` completed locally in 1.983 s from a warm release
    tree; no per-TU baseline file exists yet.
  - Focused `xmake build -r oran-storage` with GCC `-fanalyzer` completed in 3.258 s.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md` (2026-05 row).
- Next slice candidate: storage migrations (`schema_versions` + monotonic SQL files),
  because config, memory, automation, and audit repositories need a schema foundation.
