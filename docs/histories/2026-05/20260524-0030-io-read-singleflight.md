## [2026-05-24 00:30] | Task: `oran-io` read singleflight

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none - scoped follow-up to spec 0011 v1.1's
  singleflight read item.

### User Query

> Continue the current Orangutan v2 slice work and keep docs/history in sync.

### Changes Overview

- Areas: `oran-io`, tests, slice version, docs/status.
- Key actions:
  - `io::read_text_file_ranged` now prepares validation, fingerprint, and
    hot file-view cache lookup before entering a private singleflight table.
  - Concurrent cold reads with the same canonical path, range, `max_bytes`,
    and cheap `(size_bytes, mtime_ns)` fingerprint collapse behind one
    leader read.
  - Followers await the same `ReadTextResult` or error result; hot file-view
    cache hits return before touching the table.
  - The table is bounded to 64 in-flight entries and exposes only aggregate
    `ReadTextSingleflightStats`.
  - `xmake run orangutan` now reports `2.0.0-slice53`.

### Design Intent

Singleflight belongs below the tool layer because the stampede source is the
range-aware file-view primitive itself: future tools, skills, and agent-loop
callers should share the same cold-read collapse without each reimplementing a
per-tool lock table. The key includes the cheap file fingerprint and the full
read shape, so two calls only share work when they would read the same file
view. The table is intentionally bounded and observable, and it sits after the
file-view cache so steady-state hot reads do not pay the in-flight-table path.

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

- `docs/STATUS.md` - slice 53, new history pointer, refreshed `oran-io`
  test counts (42 / 212), and the next-intended-slice narrative now removes
  singleflight reads from the remaining 0011 v1.1 work.
- `docs/ARCHITECTURE.md` and `docs/design-docs/io-runtime.md` - `oran-io`
  documents the bounded singleflight table and public stats snapshot.
- `docs/product-specs/0011-file-view-and-caching.md` - slice-53 status
  block marks singleflight reads shipped and leaves watcher-backed
  external-edit awareness.
- `docs/product-specs/0012-tool-scheduler-and-state.md` - bounded-state and
  singleflight sections record the lower-level `oran-io` consumer while
  leaving scheduler-level `(tool_name, input_hash)` singleflight future work.
- `docs/releases/feature-release-notes.md` - user-visible performance note.
- `docs/QUALITY_SCORE.md` - refreshed `oran-io` coverage and IO-runtime state.

### Validation

- Commands run:
  - `xmake build test-io`
  - `./build/linux/x86_64/release/test-io "[range][singleflight]"`
  - `xmake run test-io`
  - `xmake build orangutan`
  - `xmake run orangutan --help`
  - `git diff --check`
  - `make ci`
  - `xmake test`
- Tests added/changed:
  - `tests/io/test_file.cpp` adds one `[range][singleflight]` regression
    covering two concurrent cold reads collapsing to one leader plus one
    follower, shared fingerprints, and empty table/waiter counts after
    completion.
  - Focused binary-level Catch2 filter reports 1 case / 14 assertions.
  - Full `test-io` reports 42 cases / 212 assertions.
- Bench impact: not measured in this slice. The behavior targets concurrent
  stampede avoidance rather than a single-call speedup; a future IO bench can
  compare N cold concurrent reads before/after if the scheduler workload makes
  it hot.
- Compile-budget delta: not measured in this slice; public header impact is a
  small aggregate stats struct and one accessor declaration.

### Follow-Ups

- Continue spec 0011 v1.1 with watcher-backed external-edit awareness.
