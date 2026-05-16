## [2026-05-16 12:18] | Task: `oran-storage` pool statement cache

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI, repo on Linux/WSL2, GCC 16.1.1 system compiler.
- Linked plan: `docs/exec-plans/completed/2026-05-16-oran-storage-pool-cache.md`

### User Query

> 查看当前进度，然后继续开始开发项目的下一个阶段. 执行完毕需要进一步 review,
> 并给出下一个执行任务的建议. 先 commit 当前的实现

### Changes Overview

- Areas: `oran-storage` pool public API, pool implementation, storage tests,
  storage benches, design docs, architecture map, quality score, release notes.
- Key actions:
  - Added `PoolOptions::statement_cache_capacity` with a non-zero default and
    invalid-argument validation in `Pool::open`.
  - Created one `StatementCache` for the writer slot and one cache per reader
    slot during pool open.
  - Exposed `statement_cache()` from `WriterLease` and `ReaderLease` next to
    the existing `connection()` accessors.
  - Added storage tests for invalid cache capacity, writer cache persistence
    across leases, reader cache persistence across releases, and per-slot
    reader cache isolation.
  - Added a pool-level fresh-prepare vs. cached-prepare insert bench scenario.

### Design Intent

The previous `StatementCache` slice made caching available but left callers to
own the connection-to-cache mapping. Future session/memory/audit repositories
will operate on `Pool` leases, so this slice moves that mapping into the pool:
each SQLite slot owns exactly one cache for the same lifetime as its
connection. Exposing the cache through the lease keeps the unit of ownership
visible and avoids a global or sidecar cache registry.

Cached statement work is expected to stay nested inside the pool lease scope,
matching the existing `Connection&` access contract. The cache is not
thread-safe and the pool does not add a disable-cache mode in this slice; zero
capacity remains invalid, consistent with standalone `StatementCache`.

### Files Modified

- `include/oran/storage/pool.hpp`
- `src/oran-storage/pool.cpp`
- `tests/storage/test_pool.cpp`
- `bench/storage/main.cpp`
- `bench/storage/scenarios/pool_statement_cache.cpp`
- Docs listed below.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/storage-runtime.md` — updated storage status, future-slice
  list, `PoolOptions`, lease cache accessors, pool semantics, error model, and
  compile-time note.
- `docs/ARCHITECTURE.md` — updated storage slice status and library inventory
  row for pool-owned per-slot caches.
- `docs/QUALITY_SCORE.md` — updated storage/test/bench rows and next step.
- `bench/README.md` — updated storage A-vs-B spotlight for cached prepare.
- `docs/releases/feature-release-notes.md` — added the storage-pool-cache row.
- `docs/exec-plans/active/2026-05-16-oran-storage-pool-cache.md` → moved to
  `docs/exec-plans/completed/2026-05-16-oran-storage-pool-cache.md`.

### Validation

- Commands run:
  ```sh
  git diff --check
  xmake build test-storage
  xmake build bench-storage
  xmake run test-storage
  xmake run bench-storage
  xmake test
  xmake build orangutan
  make ci
  ```
- Tests added/changed:
  - `tests/storage/test_pool.cpp`: invalid cache capacity, writer cache reuse
    after writer lease release/reacquire, reader slot cache reuse after release,
    and reader slot cache isolation.
  - `test-storage` total grew from 37 cases / 335 assertions to 40 cases /
    403 assertions.
- Bench impact:
  - `bench/storage` adds two pool-level scenarios:
    - `storage.pool_fresh_prepare_insert`: ~326.8 μs / 64-row batch.
    - `storage.pool_cached_prepare_insert`: ~285.4 μs / 64-row batch.
  - Cached prepare saves ~13% per batch in the pool path on the local in-memory
    benchmark, while preserving the existing pool acquire/release overhead.
- Compile-budget delta:
  - `pool.hpp` only adds a `StatementCache` forward declaration and one
    `std::size_t` option field; the full cache include stays in `pool.cpp`.
    The new bench scenario adds one bench TU only.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Next suggested slice: first storage domain repository (sessions first), using
  `Pool` writer/reader leases and `lease.statement_cache()` for hot SQL.
- Linked release note: `docs/releases/feature-release-notes.md` (2026-05-16
  storage-pool-cache row).
