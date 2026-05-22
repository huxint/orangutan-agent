## [2026-05-24 01:45] | Task: `oran-io` read-cache watcher

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none - scoped follow-up to spec 0011 v1.1's IO-layer
  watcher-backed external-edit awareness item.

### User Query

> Continue the current slice toward watcher-backed external-edit awareness.

### Changes Overview

- Areas: `oran-io`, tests, slice version, docs/status.
- Key actions:
  - Added public `io::ReadTextFileWatchOptions`,
    `io::ReadTextFileWatchStats`, and
    `io::watch_read_text_file_ranged_cache(executor, root, options)`.
  - Implemented the Linux/inotify backend in `src/oran-io/file.cpp` with
    system headers confined to the `.cpp`: the helper validates and
    canonicalises a directory root, registers that directory or an existing
    recursive tree, drains events through `asio::posix::stream_descriptor`,
    and invalidates the changed event path through
    `io::invalidate_read_text_file_ranged_cache(path)`.
  - Queue-overflow events conservatively invalidate all private range-read
    caches so a lost event cannot leave a restored-size/mtime external edit
    hot in memory.
  - Kept watcher output path-free: callers see only aggregate
    `directories_watched`, `events_seen`, and `invalidations`.
  - Made the production shape run until coroutine cancellation while
    allowing tests to set `max_events` for bounded drains.
  - `xmake run orangutan` now reports `2.0.0-slice58`.

### Design Intent

Slice 57 created the path-scoped invalidation seam; this slice supplies the
event source that calls it for external edits. The implementation stays in
`oran-io` because cache keys and canonicalisation are private to that library,
and because the watcher must not expose changed paths through public stats.

This slice deliberately does not auto-start the watcher from bootstrap/config.
A long-lived watcher needs a runtime-owned background task and shutdown story;
that belongs with the future service wiring rather than a policy-free IO
helper. Correctness without a started watcher is still preserved by the
existing `stat` validation before file-view cache hits.

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

- `docs/STATUS.md` - slice 58, new history pointer, refreshed `oran-io`
  test count (49 / 286), and next-intended-slice narrative now marks the
  IO-layer watcher item complete while leaving bootstrap/config startup
  wiring downstream.
- `docs/ARCHITECTURE.md` and `docs/design-docs/io-runtime.md` - public
  surfaces document the watcher options/stats/API and the Linux/inotify
  backend boundary.
- `docs/product-specs/0011-file-view-and-caching.md` - slice-58 status
  block records the concrete watcher event source and marks spec 0011
  v1.1's IO-layer cache/watch work complete.
- `docs/product-specs/0012-tool-scheduler-and-state.md` - bounded-state
  observability notes record aggregate watcher stats.
- `docs/releases/feature-release-notes.md` - user-visible release note.
- `docs/QUALITY_SCORE.md` - refreshed IO coverage and IO-runtime state.

### Validation

- Commands run:
  - `xmake build oran-io`
  - `xmake build test-io`
  - `xmake run test-io`
  - `git diff --check`
  - `make ci`
  - `xmake build orangutan`
  - `xmake run orangutan --help`
  - `xmake test`
- Tests added/changed:
  - `tests/io/test_file.cpp` adds three `[watch]` regressions: external
    rewrite invalidation with size/mtime restored, recursive child-directory
    registration, and cancellation while waiting.
  - Full `test-io` reports 49 cases / 286 assertions.
- Bench impact: not measured. The watcher runs off filesystem events and
  calls the existing bounded path-invalidation seam; cache scans remain
  capped at 32 line-offset entries and 64 file-view entries.
- Compile-budget delta: not measured; public header impact is two small
  aggregate structs and one `oran-io` function declaration. Linux-only
  inotify/asio descriptor headers are isolated in `src/oran-io/file.cpp`.

### Follow-Ups

- Add bootstrap/config runtime-service wiring that starts the watcher for a
  workspace when a long-lived runtime owns background task cancellation.
