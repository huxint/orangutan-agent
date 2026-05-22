## [2026-05-24 00:05] | Task: `file.search` regex compile cache

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none - scoped follow-up to spec 0011 v1.1's regex cache item.

### User Query

> Continue the current Orangutan v2 slice work and commit each slice.

### Changes Overview

- Areas: `oran-tool`, tests, slice version, docs/status.
- Key actions:
  - `file.search` with `regex=true` now reuses compiled
    `permission::InputPattern` values from a process-local
    `core::BoundedCache`.
  - The cache is bounded to 64 entries / 64 KiB / 10 minutes and keyed by
    pattern plus the current partial-line-match mode.
  - The literal search path is unchanged.
  - `xmake run orangutan` now reports `2.0.0-slice51`.

### Design Intent

Regex searches previously paid compile cost on every dispatch even when an
agent repeated the same pattern across files. The slice keeps the cache private
to `file_search.cpp`, uses the existing `core::BoundedCache` primitive instead
of a new map, and wraps it in an explicit mutex because the search handler hops
onto an executor before the blocking walk. The value is a `shared_ptr<const
InputPattern>` so matchers can keep using a cached compiled pattern after the
cache lock is released.

### Files Modified

- `src/oran-tool/file_search.cpp`
- `tests/tool/test_registry.cpp`
- `include/oran/tool/builtins.hpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/tool-runtime.md`
- `docs/product-specs/0011-file-view-and-caching.md`
- `docs/releases/feature-release-notes.md`
- `bench/tool/README.md`
- `bench/tool/scenarios/file_search.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 51, new history pointer, refreshed `oran-tool`
  test counts (132 / 1104), and the next-intended-slice narrative now removes
  regex compile caching from the remaining 0011 v1.1 work.
- `docs/ARCHITECTURE.md` and `docs/design-docs/tool-runtime.md` - `file.search`
  documents the bounded regex cache.
- `docs/product-specs/0011-file-view-and-caching.md` - slice-51 status block
  marks the regex compile cache shipped and leaves file-view cache,
  singleflight reads, and external-edit awareness.
- `docs/releases/feature-release-notes.md` - user-visible performance note.
- `bench/tool/README.md` and `bench/tool/scenarios/file_search.cpp` - clarify
  that the existing regex benchmark now mostly measures the steady-state cached
  path after warm-up.

### Validation

- Commands run:
  - `xmake build test-tool`
  - `xmake run test-tool "[file_search]"`
  - `xmake build orangutan`
  - `xmake run orangutan --help`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - `tests/tool/test_registry.cpp` adds one `[file_search][regex]` regression
    covering the same regex pattern reused across two dispatches.
  - Focused `test-tool` reports 132 cases / 1104 assertions.
- Bench impact: not measured in this slice. The existing regex A/B now warms
  the process-local cache; a future cold-compile scenario should vary the
  pattern each iteration if exact compile-cost tracking becomes useful.

### Follow-Ups

- Continue spec 0011 v1.1 with file-view cache, singleflight reads, and
  external-edit awareness.
