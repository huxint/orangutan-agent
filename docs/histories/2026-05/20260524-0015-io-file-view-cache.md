## [2026-05-24 00:15] | Task: `oran-io` file-view cache

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none - scoped follow-up to spec 0011 v1.1's file-view
  cache item.

### User Query

> Continue the current Orangutan v2 slice work and commit each slice.

### Changes Overview

- Areas: `oran-io`, tests, slice version, docs/status.
- Key actions:
  - `io::read_text_file_ranged` now caches successful `ReadTextResult`
    payloads in a bounded process-local `core::BoundedCache`.
  - The cache key includes canonical path, range kind/values, `max_bytes`,
    and the cheap `(size_bytes, mtime_ns)` fingerprint.
  - Cache hits revalidate metadata with `stat` before returning; changed or
    uncertain metadata misses and falls back to the existing mid-read
    pre/post fingerprint path.
  - Successful `io::write_text_file` and `io::delete_file` clear both the
    file-view cache and the line-offset index.
  - `xmake run orangutan` now reports `2.0.0-slice52`.

### Design Intent

The cache belongs in `oran-io` rather than `oran-tool` so every future caller
of the range-aware file-view surface shares the same stale-file story. The
implementation stays private to `file.cpp`, uses the existing
`core::BoundedCache` primitive, and keeps an explicit mutex because callers
can provide arbitrary executors. The key embeds cheap file metadata, and a
hit still re-stats before returning, so external edits degrade to a miss
instead of serving stale bytes.

### Files Modified

- `src/oran-io/file.cpp`
- `tests/io/test_file.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/io-runtime.md`
- `docs/product-specs/0011-file-view-and-caching.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 52, new history pointer, refreshed `oran-io`
  test counts (41 / 198), and the next-intended-slice narrative now removes
  file-view caching from the remaining 0011 v1.1 work.
- `docs/ARCHITECTURE.md` and `docs/design-docs/io-runtime.md` - `oran-io`
  documents the bounded file-view cache and synchronous invalidation path.
- `docs/product-specs/0011-file-view-and-caching.md` - slice-52 status
  block marks the file-view cache shipped and leaves singleflight reads plus
  watcher-backed external-edit awareness.
- `docs/releases/feature-release-notes.md` - user-visible performance note.
- `docs/QUALITY_SCORE.md` - refreshed `oran-io` coverage and IO-runtime
  state.

### Validation

- Commands run:
  - `xmake build test-io`
  - `xmake run test-io "[range][cache]"`
  - `xmake run test-io`
  - `xmake build orangutan`
  - `xmake run orangutan --help`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - `tests/io/test_file.cpp` adds two `[range][cache]` regressions covering
    external rewrite refresh and in-process write invalidation even when
    size and mtime are restored to the old fingerprint.
  - Full `test-io` reports 41 cases / 198 assertions.
- Bench impact: not measured in this slice. The cache is bounded to
  64 entries / 16 MiB / 10 minutes and should be benchmarked with a
  cold-vs-hot file-view scenario once singleflight reads land.

### Follow-Ups

- Continue spec 0011 v1.1 with singleflight reads and watcher-backed
  external-edit awareness.
