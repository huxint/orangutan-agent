# `oran-storage` — Prepared Statement Cache Slice

## Goal

Land the next `oran-storage` slice: a per-connection prepared-statement cache
that reuses `Statement` objects keyed by SQL text. The cache hands out
move-only RAII `CachedStatement` leases that automatically `sqlite3_reset` +
`sqlite3_clear_bindings` the underlying statement on release, evicts least-
recently-used unleased entries when at capacity, and exposes hit / miss /
eviction counters so future repositories and the pool can quantify reuse.

## Scope

- In scope:
  - Add `include/oran/storage/statement_cache.hpp` and
    `src/oran-storage/statement_cache.cpp`.
  - Define `StatementCacheOptions`, `StatementCache`, `CachedStatement`
    (move-only, RAII).
  - `StatementCache::acquire(Connection&, std::string_view)` returns
    `Result<CachedStatement>`; cache miss prepares a new statement via
    `Connection::prepare`, cache hit returns the existing entry and marks it
    most-recently-used.
  - LRU eviction at `capacity`; if all entries are currently leased on a new
    miss, the new statement is kept *transient* (not inserted into the cache)
    so cache size never exceeds `capacity`.
  - `clear()` purges every unleased entry, marks leased entries orphaned (they
    finalize on lease release), and resets counters.
  - Update `include/oran/storage.hpp` to re-export the new header.
  - Add `tests/storage/test_statement_cache.cpp` covering option validation,
    hit/miss/eviction counters, LRU policy, transient overflow, lease idempotent
    release, double-acquire rejection, and clear semantics.
  - Add `bench/storage/scenarios/statement_cache.cpp` registering
    `storage.fresh_prepare_insert` vs. `storage.cached_prepare_insert` on the
    same in-memory schema; wire from `bench/storage/main.cpp`.
  - Update `docs/design-docs/storage-runtime.md` (add Statement Cache section,
    drop the cache row from Future Slices), `docs/ARCHITECTURE.md` slice
    status, `docs/QUALITY_SCORE.md` storage + test/bench rows,
    `docs/releases/feature-release-notes.md`, and a history entry under
    `docs/histories/2026-05/`.
- Out of scope:
  - SQL-file loading from `src/oran-storage/migrations/`.
  - Domain repositories (sessions, memory, audit).
  - Wiring the cache into `Pool` (writer / reader leases) — a follow-up slice
    will compose them once the cache surface is stable.
  - Multi-thread cache access. Per-connection ownership matches SQLite's
    `SQLITE_OPEN_NOMUTEX` mode used by `Connection::open`.

## Context

- Relevant docs:
  - `docs/design-docs/storage-runtime.md` (Future Slices row for the cache).
  - `docs/QUALITY_SCORE.md` (storage row next-step).
  - `docs/rules/critical-rules.md` (C4 expected-only, C6 no heavy public
    includes, C7 explicit, C8 RAII, C12 parity, C16 docs-in-sync, C17 C++26).
  - `docs/rules/error-handling.md` (Result<T> contract).
  - `docs/rules/testing-and-bench.md` (A-vs-B bench requirement; the
    "SQLite insert path" row already names prepared-vs-literal — this slice
    adds a cached-prepared-vs-fresh-prepare comparison).
- Relevant code paths:
  - `include/oran/storage/{sqlite,pool}.hpp`, `src/oran-storage/{sqlite,pool}.cpp`
  - `tests/storage/test_sqlite.cpp` (reset + clear_bindings reuse pattern is
    the same one the cache wraps).
- Constraints:
  - Public API returns `core::Result<T>`; no exceptions across boundaries.
  - Public header stays forward-decl heavy. The cache header only pulls in
    stdlib + `oran/core/result.hpp` + `oran/storage/sqlite.hpp` (already
    transitively in the umbrella header).
  - No new third-party packages.
- Compile-budget impact: one new header + one new TU. The header pulls in
  `<string>`, `<string_view>`, `<memory>`, and `<cstddef>`, all already in
  `oran/_pch.hpp`. The impl needs `<list>` + `<unordered_map>`, confined to
  the cpp. No public dependency edge added.

## Risks

- Risk: a lease outlives the cache (cache moved or destroyed first).
  - Mitigation: state lives in `std::shared_ptr<State>`. Leases hold a
    `weak_ptr<State>` and a `shared_ptr<Entry>` so the entry stays alive even
    if the cache is gone; on lease release we lock the cache and short-circuit
    when expired.
- Risk: caller passes a different `Connection` on a subsequent `acquire`.
  - Mitigation: the cache caches the prepared statement; SQLite statements are
    handle-bound, so the second-connection caller would get the first-
    connection's statement and likely misbehave. We document "one cache per
    connection". A runtime check would require exposing the SQLite handle from
    `Connection`, which would force `<sqlite3.h>` into a public include or
    add a new `void*` accessor; deferring to a follow-up if a real bug appears.
- Risk: misuse where a caller acquires the same SQL twice while the first
  lease is outstanding (the same `Statement*` would be in two hands).
  - Mitigation: `acquire` checks the entry's `leased` flag and returns
    `ErrorKind::conflict` ("statement is already leased").
- Risk: bench A-vs-B is trivial if the SQL is too short / the prepare cost is
  dominated by syscalls.
  - Mitigation: use 64-row batched INSERT in a single in-memory connection;
    bind+step is the dominant cost in the cached path, vs. prepare+bind+step
    in the fresh-prepare path.

## Milestones

1. Active plan checked in; design surface confirmed.
2. Public header + implementation land.
3. Test bucket extended; bench scenario added; bench registered.
4. Docs updated in the same change; history entry drafted.
5. Validation suite run and recorded; plan moved to `completed/`.

## Validation

- Commands:
  - `xmake f -m release -y`
  - `xmake build oran-storage`
  - `xmake build orangutan`
  - `xmake build test-storage`
  - `xmake build bench-storage`
  - `xmake run test-storage`
  - `xmake run bench-storage`
  - `xmake test`
  - `make ci`
  - `scripts/check-lib-parity.sh`
  - `git diff --check`
- Manual checks:
  - Acquire a SQL twice in succession with intermediate release; observe
    `hits == 1`, `misses == 1`, prepared-statement identity preserved.
  - Fill cache to capacity, acquire a new SQL, observe `evictions == 1` and
    the LRU entry gone from `size`.
- Bench comparison:
  - `bench/storage` adds fresh-prepare vs. cached-prepare insert scenarios on
    a single in-memory connection with `capacity = 4`.

## Progress Log

- [x] Confirm next phase as storage statement cache.
- [x] Add active plan.
- [x] Implement `StatementCache` / `CachedStatement` and update umbrella header.
- [x] Add `tests/storage/test_statement_cache.cpp`.
- [x] Add `bench/storage/scenarios/statement_cache.cpp` + register in main.
- [x] Update docs that this slice invalidates in the same PR.
- [x] Run validation and record results.
- [x] Update `docs/QUALITY_SCORE.md`.
- [x] Write history entry.
- [x] Add release note.
- [x] Move this plan to `docs/exec-plans/completed/` before finish.

## Decision Log

- 2026-05-16: cache is a standalone helper, not a `Connection` member.
  Rationale: keeps `Connection`'s surface (and its `_pch` impact) unchanged
  and lets the future `Pool` integration own composition. Callers that want
  caching opt in.
- 2026-05-16: overflow is transient, not rejection. Rationale: callers should
  not have to handle "all leased + new SQL" as a special case; the cache stays
  bounded and degrades to no-cache for that one statement. The eviction
  counter only increments when an actual non-leased entry is dropped.
- 2026-05-16: no per-cache mutex. Rationale: SQLite `Connection` is single-
  threaded (`SQLITE_OPEN_NOMUTEX`); the cache shares that ownership model.
  Future pool integration will hand a cache to whichever lease holder owns
  the connection at the moment, so the cache inherits the single-owner rule.
- 2026-05-16: defer pool wiring. Rationale: the cache lifecycle on writer
  release vs. reader release is a separate design decision (per-slot vs.
  shared); landing the cache surface first lets the pool integration follow
  with a focused PR.

## Linked Artifacts

- Related design doc: `docs/design-docs/storage-runtime.md`
- Related product spec: none (foundational layer).
- History entry: `docs/histories/2026-05/20260516-1134-oran-storage-statement-cache.md`.
- Release note: `docs/releases/feature-release-notes.md`
