## [2026-05-23 23:25] | Task: `oran-io` line-offset index

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none - scoped follow-up to spec 0011 v1.1's line-offset
  index item.

### User Query

> Continue the current Orangutan v2 slice work and commit each slice.

### Changes Overview

- Areas: `oran-io`, tests, slice version, docs/status.
- Key actions:
  - Line-range reads of files larger than 256 KiB now lazily build a
    bounded in-memory line-offset index keyed by canonical path plus the
    cheap `(size_bytes, mtime_ns)` fingerprint.
  - Indexed line ranges seek directly to the requested byte span instead
    of scanning from line 1.
  - Successful in-process `write_text_file` and `delete_file` calls clear
    the index cache so mutations cannot keep stale offsets alive.
  - `xmake run orangutan` now reports `2.0.0-slice50`.

### Design Intent

This is the first `BoundedCache` consumer in `oran-io`. The cache stores
offsets instead of file bodies, so memory cost scales with line count rather
than file size. The implementation stays private to `file.cpp` because the
future file-view cache, singleflight read sharing, and watcher-backed
external-edit awareness still need their own slice boundaries. Cache
invalidation is intentionally conservative: clear the whole cache after
successful writes/deletes rather than add a precise remove API before there
are multiple consumers that justify it.

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

- `docs/STATUS.md` - slice 50, new history pointer, refreshed `oran-io`
  test counts (39 / 182), and the next-intended-slice narrative now removes
  line-offset indexing from the remaining 0011 v1.1 work.
- `docs/ARCHITECTURE.md` - `oran-io` inventory documents the large-file
  line-offset index and conservative write/delete invalidation.
- `docs/QUALITY_SCORE.md` - Test framework and IO runtime rows refreshed.
- `docs/design-docs/io-runtime.md` - current-slice status and future-slice
  notes document the bounded index.
- `docs/product-specs/0011-file-view-and-caching.md` - slice-50 status block
  marks the line-offset index shipped and leaves file-view cache, regex cache,
  singleflight reads, and external-edit awareness as remaining v1.1 work.
- `docs/releases/feature-release-notes.md` - user-visible behavior note.

### Validation

- Commands run:
  - `xmake build test-io`
  - `xmake run test-io`
  - `xmake build orangutan`
  - `xmake test`
  - `xmake run orangutan --help`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - `tests/io/test_file.cpp` adds three `[range][lines]` cases for indexed
    large-file reads plus write/delete invalidation where the file size and
    mtime are restored to the old fingerprint but the content changes.
  - Focused `test-io` reports 39 cases / 182 assertions.
- Bench impact: not measured. The existing read-range bench already covers
  the high-level whole-file vs. range-read split; a dedicated indexed vs.
  streaming line-range bench can land if the future file-view cache needs
  measured tuning.

### Follow-Ups

- Continue spec 0011 v1.1 with file-view cache, regex compile cache,
  singleflight reads, and external-edit awareness.
