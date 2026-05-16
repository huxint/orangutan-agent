# `oran-storage` — Pool Slot Statement Cache Slice

## Goal

Wire the just-landed `StatementCache` into `Pool` slot lifetimes. Each writer
and reader slot owns one cache for the same lifetime as its SQLite connection,
and `WriterLease` / `ReaderLease` expose that cache so future repositories can
reuse prepared statements without maintaining their own connection-to-cache map.

## Scope

- In scope:
  - Add a `statement_cache_capacity` option to `PoolOptions` with a non-zero
    default.
  - Allocate one `StatementCache` for the writer connection and one per reader
    slot during `Pool::open`.
  - Expose `StatementCache& statement_cache()` from `WriterLease` and
    `ReaderLease`.
  - Add tests proving writer/reader cache persistence across lease releases,
    slot isolation, and invalid capacity rejection.
  - Add a pool-level fresh-prepare vs. pooled-cache bench comparison.
  - Update storage design docs, architecture/quality status, release notes, and
    history in the same change.
- Out of scope:
  - First domain repository for sessions, memory, automation, or audit logs.
  - SQL-file loading from `src/oran-storage/migrations/`.
  - Changing `StatementCache` eviction policy or making it thread-safe.
  - A disable-cache mode; zero capacity remains invalid.

## Context

- Relevant docs:
  - `docs/design-docs/storage-runtime.md` (`Pool`, `Statement Cache`, and
    `Future Slices`).
  - `docs/QUALITY_SCORE.md` storage row next step.
  - `docs/rules/critical-rules.md` C4/C6/C12/C16/C17.
  - `docs/rules/testing-and-bench.md` A-vs-B bench requirement.
- Relevant code paths:
  - `include/oran/storage/{pool,statement_cache}.hpp`
  - `src/oran-storage/{pool,statement_cache}.cpp`
  - `tests/storage/test_pool.cpp`
  - `bench/storage/scenarios/`
- Constraints:
  - Public API remains expected-only; `Pool::open` returns invalid-argument
    errors for invalid capacity.
  - Pool leases already expose `Connection&`; cache access follows the same
    lease-scoped ownership model. Callers must not keep connection or cache
    work past the lease lifetime.
  - No new third-party dependencies.
- Compile-budget impact (if any):
  - `pool.hpp` can forward-declare `StatementCache`; the implementation already
    depends on storage internals and will include `statement_cache.hpp`.
  - One extra bench scenario TU; no new library target.

## Risks

- Risk: callers let `CachedStatement` outlive the pool lease and later reuse the
  same slot. Mitigation: document cache work as lease-scoped, matching the
  existing `Connection&` reference contract; tests use nested statement scopes.
- Risk: public header include weight grows if `pool.hpp` includes
  `statement_cache.hpp`. Mitigation: forward-declare `StatementCache` in
  `pool.hpp`; umbrella `storage.hpp` still includes the full cache header.
- Risk: reader caches accidentally share state. Mitigation: one cache per reader
  slot and tests that slot-local hit/miss counters persist independently.

## Milestones

1. Public API and implementation update.
2. Pool tests for capacity validation, writer cache persistence, reader cache
   persistence, and slot isolation.
3. Pool-level bench scenario and registration.
4. Docs/history/release notes sync.
5. Validation and plan move to `completed/`.

## Validation

- Commands:
  - `git diff --check`
  - `xmake build test-storage`
  - `xmake run test-storage`
  - `xmake build bench-storage`
  - `xmake run bench-storage`
  - `xmake test`
  - `xmake build orangutan`
  - `make ci`
- Manual checks:
  - Acquire a writer, use `lease.statement_cache()` for an insert, release the
    lease, reacquire the writer, and confirm the same SQL is a cache hit.
  - Acquire a single reader slot twice and confirm the second query is a hit.
  - Acquire two reader slots and confirm their cache counters are independent.
- Observability checks:
  - None; storage cache counters are the observability surface for this slice.
- Bench comparison (if perf-relevant):
  - `bench/storage` compares fresh prepare through a writer pool lease vs.
    cached prepare through the writer lease's slot cache.
  - Local result: `storage.pool_fresh_prepare_insert` ~326.8 μs per 64-row
    batch vs. `storage.pool_cached_prepare_insert` ~285.4 μs per 64-row batch.

## Progress Log

- [x] Confirm scope and constraints.
- [x] Implement pool-owned statement caches.
- [x] Add tests for pool cache behavior.
- [x] Add pool-level cache bench.
- [x] Update docs that this slice invalidates in the same PR.
- [x] Run validation and record results.
- [x] Write history entry.
- [x] Add release note.
- [x] Move plan to `docs/exec-plans/completed/` before finish.

## Decision Log

- 2026-05-16: expose cache by lease reference, not as a `Pool` lookup API.
  Rationale: repositories already operate on a lease-scoped connection; adding
  `lease.statement_cache()` keeps the unit of ownership explicit.
- 2026-05-16: cache capacity is non-zero and configured at pool open. Rationale:
  the standalone `StatementCache` rejects zero capacity, and a no-cache mode
  should be a deliberate later option if needed.

## Linked Artifacts

- Related design doc: `docs/design-docs/storage-runtime.md`
- Related product spec: none (foundational storage layer).
- PRs: local change.
- History entry: `docs/histories/2026-05/20260516-1218-oran-storage-pool-cache.md`
- Release note: `docs/releases/feature-release-notes.md`
