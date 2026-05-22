## [2026-05-24 01:30] | Task: `oran-io` read-cache path invalidation

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none - scoped follow-up to spec 0011 v1.1's remaining
  watcher-backed external-edit awareness item.

### User Query

> Continue the current slice toward watcher-backed external-edit awareness.

### Changes Overview

- Areas: `oran-core`, `oran-io`, tests, slice version, docs/status.
- Key actions:
  - Added `core::BoundedCache::erase_if(predicate)` for explicit
    caller-driven invalidation that updates current occupancy / byte totals
    without counting the removal as LRU, TTL, or byte-budget eviction.
  - Added public `io::invalidate_read_text_file_ranged_cache(path)`.
  - The `oran-io` invalidation path canonicalises through the same private
    cache-key helper used by reads, then erases matching entries from both the
    line-offset index and file-view cache without exposing paths, keys, or
    file contents.
  - Successful `io::write_text_file` and `io::delete_file` now reuse that
    path-scoped seam instead of clearing unrelated range-read cache entries.
  - `xmake run orangutan` now reports `2.0.0-slice57`.

### Design Intent

Spec 0011's watcher-backed external-edit awareness needs a stable way to mark
one canonical path stale. The previous cache invalidation primitive was
`clear()`, which was safe but too broad: a write to one file flushed every
cached line-offset index and file view. Adding `erase_if` to `BoundedCache`
keeps the generic cache primitive simple while letting `oran-io` invalidate
only entries whose private key canonical path matches the changed file.

This slice deliberately does not implement the filesystem watcher itself.
It lands the public seam a watcher will call, while retaining the existing
`stat` validation before file-view hits for correctness when no watcher exists.

### Files Modified

- `include/oran/core/bounded_cache.hpp`
- `tests/core/test_bounded_cache.cpp`
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

- `docs/STATUS.md` - slice 57, new history pointer, refreshed
  `oran-core` (69 / 446) and `oran-io` (46 / 260) test counts, and
  next-intended-slice narrative now names the path-stale seam while leaving
  concrete watcher event wiring downstream.
- `docs/ARCHITECTURE.md` and `docs/design-docs/io-runtime.md` - public
  surfaces document `BoundedCache::erase_if` and
  `invalidate_read_text_file_ranged_cache(path)`.
- `docs/product-specs/0011-file-view-and-caching.md` - slice-57 status block
  records path-scoped invalidation and narrows the remaining work to watcher
  registration / event source.
- `docs/product-specs/0012-tool-scheduler-and-state.md` - `BoundedCache`
  status and bounded-state observability notes record explicit invalidation.
- `docs/releases/feature-release-notes.md` - user-visible release note.
- `docs/QUALITY_SCORE.md` - refreshed core/io coverage and IO-runtime state.

### Validation

- Commands run:
  - `xmake build test-core`
  - `xmake build test-io`
  - `xmake run test-core`
  - `xmake run test-io`
  - `git diff --check`
  - `make ci`
  - `xmake build orangutan`
  - `xmake run orangutan --help`
  - `xmake test`
- Tests added/changed:
  - `tests/core/test_bounded_cache.cpp` adds one `erase_if` regression.
  - `tests/io/test_file.cpp` adds three cache invalidation regressions:
    direct file-view path invalidation, direct line-offset-index path
    invalidation, and write invalidation preserving a hot unrelated file view.
  - Full `test-core` reports 69 cases / 446 assertions.
  - Full `test-io` reports 46 cases / 260 assertions.
- Bench impact: not measured. Path invalidation is mutation/watch-event side
  work and uses bounded cache scans over small configured cache sizes
  (32 line-offset entries and 64 file-view entries).
- Compile-budget delta: not measured; public header impact is one small
  template member and one `oran-io` function declaration.

### Follow-Ups

- Wire a concrete filesystem watcher / event source that calls
  `io::invalidate_read_text_file_ranged_cache(path)` for changed paths.
