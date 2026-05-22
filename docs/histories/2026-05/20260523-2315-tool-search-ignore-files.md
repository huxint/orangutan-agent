## [2026-05-23 23:15] | Task: source-controlled ignore files for `file.search`

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none - scoped follow-up to spec 0011 v1.1's
  `file.search` ignore-predicate item.

### User Query

> Continue the current Orangutan v2 slice work and commit each slice.

### Changes Overview

- Areas: `oran-tool` `file.search`, tests, slice version, docs/status.
- Key actions:
  - `file.search` now loads `.gitignore` and `.ignore` files from the
    recursive search root downward when `respect_ignore=true` (default).
  - The supported subset covers blank lines, `#` comments, escaped leading
    `#` / `!` literals, `!` negation, trailing `/` directory rules,
    slash-relative patterns, basename patterns, and fnmatch-style globs.
  - Nested ignore files apply to descendants only; `respect_ignore=false`
    disables both ignore-file rules and the slice-48 built-in skip list.
  - `xmake run orangutan` now reports `2.0.0-slice49`.

### Design Intent

This finishes the practical `file.search` ignore-predicate work without
trying to implement full Git ignore parity. The implementation keeps the
parser private to `file_search.cpp`, because spec 0013 still owns the future
structural move to a shared `Workspace::is_ignored(...)` predicate once
`directory.scan` exists. That keeps this slice small while giving agents the
common repository behavior they need today.

### Files Modified

- `src/oran-tool/file_search.cpp`
- `include/oran/tool/builtins.hpp`
- `tests/tool/test_registry.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/tool-runtime.md`
- `docs/product-specs/0011-file-view-and-caching.md`
- `docs/product-specs/0013-workspace-and-path-policy.md`
- `docs/exec-plans/tech-debt-tracker.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 49, new history pointer, refreshed
  `oran-tool` test counts (131 / 1093), and the next-intended-slice
  narrative now removes `.gitignore` / `.ignore` from the remaining 0011
  ignore-predicate work.
- `docs/ARCHITECTURE.md` - `file.search` inventory documents the shipped
  ignore-file subset.
- `docs/QUALITY_SCORE.md` - Test framework row refreshed for `oran-tool`.
- `docs/design-docs/tool-runtime.md` - built-in catalog describes
  `respect_ignore=true` source-controlled ignore behavior.
- `docs/product-specs/0011-file-view-and-caching.md` - slice-49 status block
  marks the current `file.search` ignore predicate complete and leaves cache
  consumers as the remaining v1.1 work.
- `docs/product-specs/0013-workspace-and-path-policy.md` - records the
  shipped tool-local predicate and the future workspace-owned sharing point.
- `docs/exec-plans/tech-debt-tracker.md` - removes `.gitignore` parsing from
  the remaining ripgrep-class optimization row.
- `docs/releases/feature-release-notes.md` - user-visible behavior note.

### Validation

- Commands run:
  - `xmake build oran-tool`
  - `xmake build test-tool`
  - `xmake run test-tool "[file_search]"`
  - `git diff --check`
  - `make ci`
  - `xmake build orangutan`
  - `xmake test`
  - `xmake run orangutan --help`
- Tests added/changed:
  - `tests/tool/test_registry.cpp` adds four `[respect_ignore]` cases for
    basename/path/directory/negation rules, escaped leading markers, nested
    `.ignore` scope, and `respect_ignore=false`.
  - Focused `test-tool "[file_search]"` reports 131 cases / 1093 assertions.
- Bench impact: not measured. The ignore-file work adds per-directory rule
  loading and `fnmatch` checks on recursive walks only; single-file searches
  do not consult ignore files. Repo-scale optimization remains tracked as
  future work.
- Compile-budget delta: not measured. No public heavy includes were added;
  the new POSIX `fnmatch` include is private to `file_search.cpp`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: existing `tool/search` row remains for mmap,
  extension-aware binary skip, and multi-threaded walk optimizations.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
