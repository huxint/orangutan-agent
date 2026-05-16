## [2026-05-16 02:27] | Task: `oran-storage` async connection pool

### Execution Context

- Agent: Claude Code (Opus 4.7, 1M context)
- Base model: claude-opus-4-7
- Runtime: Claude Code CLI, repo on Linux/WSL2, GCC 16.1.1 system compiler.
- Linked plan: `docs/exec-plans/completed/2026-05-16-oran-storage-pool.md`

### User Query

> 根据项目当前的实现，继续推进项目的下一步实现

### Changes Overview

- Areas: `oran-storage` public API, `oran-storage` implementation, xmake target
  graph, storage tests, storage bench, design docs, architecture map, quality
  score, release notes.
- Key actions:
  - Added `Pool`, `WriterLease`, `ReaderLease` to `oran-storage` with one
    writer connection (exclusive) and N reader connections (FIFO hand-off),
    driven by an `asio::any_io_executor` supplied by the caller.
  - Acquisition is async only: `acquire_writer` / `acquire_reader` return
    `Awaitable<Result<Lease>>` and honor coroutine cancellation via
    `asio::this_coro::cancellation_state` plus per-waiter cancellation slots.
  - Wired `oran-storage` to depend on `oran-async` in `xmake/targets.lua` and
    re-exported the new public header from `include/oran/storage.hpp`.
  - Added `tests/storage/test_pool.cpp` covering option validation, slot
    layout, writer exclusion + migrations through the pool, FIFO reader
    resumption, reader read-only enforcement, cross-lease visibility,
    cancellation, and idempotent release.
  - Added `bench/storage/scenarios/pool_acquire.cpp` registering a direct
    `Connection` vs. `Pool` acquire+query comparison driven by an `io_context`.

### Design Intent

This slice fulfills the pool requirement listed under
`docs/design-docs/storage-runtime.md#Future Slices` ("`Pool` with one writer
connection on a strand and reader connections round-robin"), and closes the
`Use oran-async from oran-storage` next-step row in `docs/QUALITY_SCORE.md`.

The lease/queue split keeps SQLite's single-writer/multi-reader contract
explicit in the type system: callers cannot create a `Connection` of the wrong
mode through the pool. Async acquisition replaces a per-call connection open
with a wait-for-slot, which keeps the per-query SQLite cost dominant. The
pool intentionally exposes only async accessors — synchronous `try_acquire_*`
variants can land later if a sync caller appears, but the current async
runtime is the canonical execution context.

Compile-time impact is limited to a single new public include set
(`<asio/any_io_executor.hpp>` and `<oran/async/awaitable_fwd.hpp>`) in the
storage public surface, mirroring how `oran-async` already exposes asio. TUs
that consume only `Connection` / `Statement` keep their existing include cost.

### Files Modified

- `include/oran/storage/pool.hpp` (new)
- `include/oran/storage.hpp`
- `src/oran-storage/pool.cpp` (new)
- `tests/storage/test_pool.cpp` (new)
- `bench/storage/scenarios/pool_acquire.cpp` (new)
- `bench/storage/main.cpp`
- `xmake/targets.lua`
- Docs listed below.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/storage-runtime.md` — pool surface, semantics, error model,
  compile-time cost; future-slices list updated to remove the now-landed pool.
- `docs/ARCHITECTURE.md` — slice status line updated for 2026-05-16 with the
  pool; storage row dependency table now lists `oran-async`.
- `docs/QUALITY_SCORE.md` — storage row updated to reflect the new pool + bench;
  async row's next-step reframed because the storage-from-async edge is live;
  test framework + bench rows now record the new assertion totals.
- `docs/releases/feature-release-notes.md` — added 2026-05-16 storage-pool row.
- `docs/exec-plans/active/2026-05-16-oran-storage-pool.md` → moved to
  `docs/exec-plans/completed/2026-05-16-oran-storage-pool.md`.

### Validation

- Commands run:
  ```sh
  xmake f -m release -y
  xmake build oran-storage
  xmake build orangutan
  xmake build test-storage
  xmake build bench-storage
  xmake run test-storage
  xmake run bench-storage
  xmake test
  make ci
  scripts/check-lib-parity.sh
  git diff --check
  ```
- Tests added/changed:
  - `tests/storage/test_pool.cpp`: 9 new cases (option validation, slot
    distinctness, writer exclusion + migration, reader hand-off, FIFO
    resumption, reader-write rejection, cross-lease visibility, cancellation,
    idempotent release).
  - `test-storage` total grew from 16 cases / 150 assertions to 25 cases /
    229 assertions.
- Bench impact:
  - `bench/storage` adds two scenarios:
    - `storage.direct_connection_query`: 34.95 μs / 16-query batch.
    - `storage.pool_acquire_query`: 39.07 μs / 16-query batch.
  - Pool acquire overhead vs. direct re-use is ~12% on a hot in-memory schema.
- Compile-budget delta: `oran-storage` now drags in the asio public include
  set via `oran-async`. The storage TUs that already touched async via the
  test harness do not see new costs; storage-only TUs gain the asio fwd path
  (`any_io_executor.hpp` + `awaitable_fwd.hpp`). No per-target budget file
  changes yet (none exists for storage).

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md` (2026-05-16
  storage-pool row).
