# `oran-storage` — Connection Pool Slice

## Goal

Land the next `oran-storage` slice: an async connection pool with one writer
connection (exclusive) and N reader connections (round-robin), driven by
`oran-async` executors. The pool keeps the existing `Connection` /
`Statement` core intact, returns RAII leases that release back to the pool on
destruction, and integrates with `run_migrations` through the writer lease.

## Scope

- In scope:
  - Add `include/oran/storage/pool.hpp` and `src/oran-storage/pool.cpp`.
  - Define `PoolOptions`, `Pool`, `WriterLease`, `ReaderLease` (move-only, RAII).
  - Async acquisition through `oran-async::Awaitable<core::Result<...>>`,
    cancel-aware via `asio::this_coro::cancellation_state`.
  - Hand-off serialization: at most one outstanding `WriterLease` at any time;
    at most `reader_count` outstanding reader leases; new waiters resume in FIFO
    order on release.
  - Wire `oran-storage` to depend on `oran-async` in `xmake/targets.lua`; carry
    `asio` package through the public dependency chain.
  - Update `include/oran/storage.hpp` to re-export pool symbols.
  - Add `tests/storage/test_pool.cpp` covering migrations via writer lease,
    multi-reader hand-off, writer exclusion, FIFO waiter resumption, cancellation,
    and option validation.
  - Add `bench/storage/scenarios/pool_acquire.cpp` comparing direct
    `Connection::query` vs. `Pool::acquire_reader` + `query`, registered from
    `bench/storage/main.cpp`.
  - Update `docs/design-docs/storage-runtime.md`, `docs/ARCHITECTURE.md` slice
    status, `docs/QUALITY_SCORE.md` storage + async rows, `docs/releases/feature-release-notes.md`,
    and a new history entry under `docs/histories/2026-05/`.
- Out of scope:
  - Prepared-statement cache (next storage slice).
  - SQL-file loading from `src/oran-storage/migrations/`.
  - Domain repositories (sessions, memory, audit).
  - Backpressure metrics / observability for waiters.
  - Synchronous `try_acquire_*` variants — defer until a sync caller appears.

## Context

- Relevant docs:
  - `docs/design-docs/storage-runtime.md` (current public surface + Future Slices).
  - `docs/design-docs/async-model.md` (executor topology, cancellation contract).
  - `docs/ARCHITECTURE.md` (slice status row + storage dep table).
  - `docs/rules/critical-rules.md` (C4 expected-only, C6 no heavy includes in public
    headers, C8 RAII, C11 cancel-aware, C12 parity, C16 docs-in-sync).
  - `docs/rules/error-handling.md` (Result<T> contract).
  - `docs/rules/testing-and-bench.md` (A-vs-B bench requirement).
- Relevant code paths:
  - `include/oran/storage/{sqlite,migrations}.hpp`, `src/oran-storage/{sqlite,migrations}.cpp`
  - `include/oran/async/{runtime,channel,awaitable_fwd}.hpp`, `src/oran-async/*`
  - `xmake/{targets,tests,bench}.lua`
- Constraints:
  - Public API returns `core::Result<T>`; no exceptions across boundaries.
  - Public headers stay forward-decl heavy — only `awaitable_fwd.hpp` +
    `<asio/any_io_executor.hpp>` permitted from public surface (same pattern as
    `oran-async/channel.hpp`).
  - All async functions check coroutine cancellation before waiting.
  - SQLite is single-writer multi-reader under WAL; readers open with
    `OpenMode::read_only`, writer with `OpenMode::read_write_create`.
- Compile-budget impact: `oran-storage` now drags in `asio` via the public
  `oran-async` dependency. The asio set is already required by every binary
  that links `oran-async`; storage TUs add one new public dependency edge.

## Risks

- Risk: lease destruction races with pool shutdown.
  - Mitigation: pool internals live behind `shared_ptr<State>`; leases hold the
    same `shared_ptr`. Pool dtor closes underlying connections but lease
    destruction is safe even after the `Pool` object moves or destructs.
- Risk: coroutine cancellation while waiting leaks a reader slot.
  - Mitigation: cancellation handler removes the waiter from the queue before
    completing it with `Error::cancelled()`; the slot is never marked as taken
    by a cancelled waiter.
- Risk: FIFO fairness assumed by tests is not actually enforced.
  - Mitigation: implementation uses `std::deque<Waiter>`; release pops front.
- Risk: writer connection serialization is conceptual only.
  - Mitigation: writer mutex enforced inside the pool state; `acquire_writer`
    blocks (asynchronously) until the single writer slot is free.
- Risk: bench A-vs-B becomes meaningless if the pool just wraps a single
  connection.
  - Mitigation: bench compares direct connection re-use vs. pool acquire/release
    on `reader_count = 4`. Both paths perform identical `SELECT` work; the
    bench measures pool overhead, which is the relevant tradeoff.

## Milestones

1. Active plan checked in; design surface confirmed against
   `docs/design-docs/storage-runtime.md`.
2. Public header + implementation land with xmake wiring.
3. Test bucket extended; bench scenario added.
4. Docs updated in the same change; history entry drafted.
5. Validation suite run and recorded.

## Validation

- Commands:
  - `xmake f -m release -y`
  - `xmake build oran-storage`
  - `xmake build test-storage`
  - `xmake build bench-storage`
  - `xmake run test-storage`
  - `xmake run bench-storage`
  - `xmake test`
  - `make ci`
  - `scripts/check-lib-parity.sh`
  - `git diff --check`
- Manual checks:
  - Run a pool with two readers; observe that two concurrent queries succeed and
    a third coroutine waits until one lease is released.
  - Apply migrations through `acquire_writer()` and observe a reader sees the new
    table.
- Bench comparison:
  - `bench/storage` adds direct vs. pool acquire scenarios for the same
    `SELECT COUNT(*)` workload, with `reader_count = 4`.

## Progress Log

- [x] Confirm next phase as storage pool slice.
- [x] Add active plan.
- [x] Implement Pool / WriterLease / ReaderLease and xmake wiring.
- [x] Add `tests/storage/test_pool.cpp`.
- [x] Add `bench/storage/scenarios/pool_acquire.cpp`.
- [x] Update docs that this slice invalidates in the same PR.
- [x] Run validation and record results.
- [x] Update `docs/QUALITY_SCORE.md`.
- [x] Write history entry.
- [x] Add release note.
- [x] Move this plan to `docs/exec-plans/completed/` before finish.

## Decision Log

- 2026-05-16: async-only acquisition. Rationale: `oran-storage` should integrate
  with the existing executor topology rather than expose a parallel
  mutex/condition-variable API. Synchronous `try_acquire_*` and blocking variants
  can be added later if a sync-only caller emerges.
- 2026-05-16: readers open read-only. Rationale: the writer holds exclusive write
  access; readers must not race the migration runner. SQLite enforces this when
  `OpenMode::read_only` is used.
- 2026-05-16: defer prepared-statement cache. Rationale: keep this slice
  focused on the lifetime contract; the cache touches the `Statement` shape and
  will land as the next dedicated slice.

## Linked Artifacts

- Related design doc: `docs/design-docs/storage-runtime.md`
- Related product spec: none (foundational layer).
- History entry: `docs/histories/2026-05/20260516-0227-oran-storage-pool.md`
- Release note: `docs/releases/feature-release-notes.md`
