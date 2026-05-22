## [2026-05-24 00:45] | Task: `oran-io` range-read cache stats

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none - scoped follow-up to spec 0011 v1.1's bounded-state
  observability requirement.

### User Query

> Continue the current Orangutan v2 slice work and keep docs/history in sync.

### Changes Overview

- Areas: `oran-io`, tests, slice version, docs/status.
- Key actions:
  - Added `ReadTextBoundedCacheStats` and `ReadTextFileCacheStats` to the
    public `oran-io` file surface.
  - Added `io::read_text_file_ranged_cache_stats()` as a read-only snapshot
    for the private line-offset index and file-view cache.
  - The accessor maps each underlying `core::BoundedCache::Stats` value while
    holding the cache's existing mutex, and exposes no canonical paths, ranges,
    keys, or file contents.
  - Added a regression that forces one cold+hot file-view read and one
    cold+hot large-file line-offset-index sequence, then asserts the public
    counters moved as expected.
  - `xmake run orangutan` now reports `2.0.0-slice54`.

### Design Intent

Spec 0012 requires bounded runtime state to be observable, but the line-offset
index and file-view cache were previously private black boxes. This slice keeps
the data-plane caches private and only publishes aggregate health counters,
matching the singleflight stats privacy boundary from slice 53. The shape mirrors
`core::BoundedCache::Stats` instead of exporting that template type through the
IO API so callers stay decoupled from the cache implementation.

### Files Modified

- `include/oran/io/file.hpp`
- `src/oran-io/file.cpp`
- `tests/io/test_file.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/io-runtime.md`
- `docs/product-specs/0011-file-view-and-caching.md`
- `docs/product-specs/0012-tool-scheduler-and-state.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 54, new history pointer, refreshed `oran-io`
  test counts (43 / 228), and next-intended-slice narrative now names the
  new cache stats accessor.
- `docs/ARCHITECTURE.md` and `docs/design-docs/io-runtime.md` - `oran-io`
  documents the `ReadTextFileCacheStats` public snapshot and the updated
  public surface.
- `docs/product-specs/0011-file-view-and-caching.md` - slice-54 status
  block marks cache stats shipped and leaves watcher-backed external-edit
  awareness.
- `docs/product-specs/0012-tool-scheduler-and-state.md` - bounded-state
  inventory records the line-offset index and file-view cache policies plus
  their public stats accessor.
- `docs/releases/feature-release-notes.md` - user-visible observability note.
- `docs/QUALITY_SCORE.md` - refreshed `oran-io` coverage and IO-runtime state.

### Validation

- Commands run:
  - `xmake build test-io`
  - `./build/linux/x86_64/release/test-io "[range][cache][stats]"`
  - `xmake run test-io`
  - `xmake build orangutan`
  - `xmake run orangutan --help`
  - `git diff --check`
  - `make ci`
  - `xmake test`
- Tests added/changed:
  - `tests/io/test_file.cpp` adds one `[range][cache][stats]` regression
    covering file-view cache hit/miss counters and large-file line-offset
    index hit/miss counters. Focused binary-level Catch2 filter reports
    1 case / 16 assertions.
  - Full `test-io` reports 43 cases / 228 assertions.
- Bench impact: not measured. This is an observability accessor over existing
  cache state and does not alter read execution policy.
- Compile-budget delta: not measured; public header impact is two small stats
  aggregates and one accessor declaration.

### Follow-Ups

- Continue spec 0011 v1.1 with watcher-backed external-edit awareness.
