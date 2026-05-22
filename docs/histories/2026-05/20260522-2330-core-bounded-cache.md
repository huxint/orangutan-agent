## [2026-05-22 23:30] | Task: core::BoundedCache<Key,Value> generic primitive (spec 0012 + spec 0011 v1.1 prerequisite)

### Execution Context

- Agent: `Claude Code`
- Base model: `Opus 4.7`
- Runtime: `Claude Code CLI`
- Linked plan: none — scoped to a single primitive add

### User Query

> Deeply understand the project architecture and current implementation
> progress, continue advancing the project, two slices, one commit per
> slice; ultrathink.

### Changes Overview

- Areas: `oran-core` (new header-only template + tests + bench), version
  banner, spec 0011 v1.1 status, spec 0012 status.
- Key actions:
  - Added `core::BoundedCache<Key, Value, ByteSizeOf =
    BoundedCacheNoByteBudget, Hash = std::hash<Key>, KeyEqual =
    std::equal_to<Key>>` in `<oran/core/bounded_cache.hpp>`.
  - The cache is **single-threaded** by contract (documented in the
    header). Storage is `std::list<Entry>` for LRU order plus an
    `std::unordered_map<Key, list_iterator>` for O(1) lookup. Eviction
    order is documented in the type name: LRU on access, insert-based
    TTL on age, byte-budget on payload.
  - `get(key, now)` returns `Value*` (non-owning, nullable). This
    diverges from the spec sketch's `std::optional<Value>` because the
    spec's own example explicitly names
    `BoundedCache<(pattern, options), unique_ptr<re2::RE2>>` — and
    `std::optional<unique_ptr<re2::RE2>>` cannot be populated by copy.
    The pointer is invalidated by the next non-const operation.
  - `put(key, value, now)` enforces both the entry cap and the byte
    cap. An item whose `ByteSizeOf{}(value)` exceeds `max_bytes` is
    rejected (`rejected_oversize++`) AND any prior entry under the
    same key is removed — a "refuses to cache" outcome must not leave
    a stale value behind.
  - `reap(now)` walks all entries and evicts those past TTL (O(n);
    cheap for the cache sizes spec 0012 contemplates — 64-entry regex
    cache, 256-entry tool catalog cache, etc.).
  - `Stats { hits, misses, evictions_lru, evictions_ttl,
    evictions_bytes, rejected_oversize, current_entries, current_bytes }`
    is exposed via `stats()` for the future `oran-log` periodic tick.
  - Byte-size customisation is via template parameter
    (`ByteSizeOf`) carried in the class with
    `[[no_unique_address]]` so the default (no byte budget) costs no
    extra storage; callers wanting byte-aware eviction supply their
    own stateless functor (e.g., `struct StringByteCost { std::size_t
    operator()(const std::string& s) const noexcept { return s.size();
    } };`).
  - Added 14 Catch2 cases covering basic put/get, LRU eviction, TTL on
    get, bulk `reap`, no-op `reap` without TTL, byte budget eviction
    over cap, oversize rejection, oversize re-put invalidates prior
    entry, re-put refresh, move-only `unique_ptr<string>` Value, clear
    preserves lifetime counters, stats correctness across the full
    transition matrix, miss with no other state change, and LRU
    position refresh on `get`. `tests/core` grows from 54 / 370 to
    68 / 435.
  - Added `bench/core/scenarios/bounded_cache.cpp` with three
    scenarios: raw `unordered_map` insert+lookup (~7.7 µs / 1024 ops),
    `BoundedCache` insert+lookup with the same working set (~16.5 µs
    — ~2× LRU bookkeeping overhead), and `BoundedCache` with constant
    overflow (~30.8 µs — pins the eviction-on-every-put hot path).
  - Bumped the version banner to slice 44.

### Design Intent

The spec calls for `BoundedCache` as a prerequisite of spec 0011 v1.1
(line-offset index, file-view cache, regex cache) AND of spec 0012 (tool
scheduler bounded state). Landing the primitive **before** the first
consumer means later cache slices can be tiny — each only writes the
specialisation, never the eviction machinery.

`oran-core` is the right home because (a) the primitive is generic with
no domain dependencies (stdlib only), (b) downstream libs already
depend on `oran-core`, and (c) the spec explicitly says "lifted into
`oran-core` once a second call site appears" — we have two known
call-site clusters, so we lift now rather than parking the type in
whichever lib happens to consume it first.

The `Value*`-from-`get` deviation from the spec sketch was forced by
move-only Value types. Documenting the deviation in the header AND in
spec 0012's status block keeps the next agent honest. The alternative
(forcing every caller to wrap their value in `shared_ptr`) would have
imposed a heap allocation per inserted regex / opened file index — not
free, and the spec's own example contradicts that wrapping.

The `[[no_unique_address]]` on the size functor keeps the default
"no byte budget" cache as cheap as a hand-rolled LRU map. Stateful
functors (carrying a per-instance multiplier or a clock) work the same
way as stateless ones; they just take their share of `sizeof`.

A separate `rejected_oversize` stat was added (not in the spec sketch)
because the spec acceptance criterion 8 of 0011 v1 explicitly mentions
"refuses to cache items larger than its byte budget" as observable
behavior — a single integer counter is the cheapest way to surface it,
and the alternative (silently dropping the put) would have hidden a
sizing bug.

Single-strand instead of internally locked because every named consumer
in spec 0012 runs on the agent strand and an internal mutex hides cost
from callers that do not need it. If a future multi-threaded consumer
appears we add `BoundedCacheMt<...>` as a thin lock-and-forward wrapper
rather than retrofitting the primitive.

### Files Modified

- `include/oran/core/bounded_cache.hpp` (new)
- `tests/core/test_bounded_cache.cpp` (new)
- `bench/core/scenarios/bounded_cache.cpp` (new)
- `bench/core/main.cpp`
- `src/oran-bootstrap/bootstrap.cpp` (slice banner bump)
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/product-specs/0011-file-view-and-caching.md`
- `docs/product-specs/0012-tool-scheduler-and-state.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 44, new history pointer, refreshed
  `oran-core` test count (68/435), and the next-intended-slice
  narrative.
- `docs/ARCHITECTURE.md` — slice-status block + `oran-core` library
  inventory row gain the slice-44 `BoundedCache` callout.
- `docs/product-specs/0012-tool-scheduler-and-state.md` —
  `BoundedCache<Key, Value>` status block bumped to "shipped in
  oran-core (slice 44)" with the shipped/spec deltas documented
  (single-strand, `Value*` from `get`, `rejected_oversize` stat).
- `docs/product-specs/0011-file-view-and-caching.md` — v1.1
  prerequisite note recording that `BoundedCache` is now available in
  `oran-core` for the future line-offset / file-view / regex caches.
- `docs/QUALITY_SCORE.md` — Test framework row refreshed (`oran-core`
  68 / 435) and Bench harness row gains the slice-44 `BoundedCache`
  A/B summary.
- `docs/releases/feature-release-notes.md` — user-visible release
  note.

### Validation

- Commands run:
  - `xmake build oran-core`
  - `xmake build test-core` / `xmake run test-core`
  - `xmake build bench-core` / `xmake run bench-core`
  - `xmake test` (all 10 buckets pass)
- Tests added/changed:
  - `tests/core/test_bounded_cache.cpp` adds 14 cases. `tests/core`
    now reports 68 cases / 435 assertions (was 54 / 370 after the
    last `oran-core` slice).
- Bench impact:
  - `core.unordered_map_insert_lookup_256` ~7.7 µs.
  - `core.bounded_cache_insert_lookup_256` ~16.5 µs (~2× the raw
    `unordered_map` cost; the delta is LRU bookkeeping plus stats).
  - `core.bounded_cache_overflow_lru_eviction` ~30.8 µs (~4× the raw
    cost; constant eviction on every put).
- Compile-budget delta: not measured. Header-only template adds no
  TU; existing TUs that include the header (only `test-core` +
  `bench-core` so far) link in under a couple seconds on this
  environment; this is not the reference hardware gate.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none. Future slices wire the regex compile cache
  (`file.search`), the tool catalog rendered-block cache, the
  line-offset index, and the file-view cache on top of this primitive.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
