## [2026-06-04 23:08] | Task: Long-term memory FTS5 backend

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local shell + xmake/GCC 16.1
- Linked plan: none; this is a small follow-up from the deep-review tracker.

### User Query

> Continue the memory roadmap after deeply re-orienting on the repository docs,
> use CodeGraph while working, commit the completed slice, and keep iterating.

### Changes Overview

- Areas: `oran-memory`, xmake package config, memory/product/build/status docs.
- Key actions:
  - Added `memory::longterm::Fts5Backend`, the default SQLite FTS5 lexical
    backend implementing the public `Backend` contract.
  - Added the long-term memory FTS5 migration under
    `src/oran-memory/migrations/longterm/` and embedded it into
    `oran-memory` with C++26 `#embed`.
  - Kept storage layering narrow: `oran-storage` supplies `Pool`,
    `StatementCache`, SQLite statements, and migration execution; `oran-memory`
    owns memory semantics, schema, validation, and backend behavior.
  - Configured the xmake SQLite package with `SQLITE_ENABLE_FTS5` after
    focused tests proved the previous package build lacked FTS5 support.
  - Added public-backend tests for migration idempotence, scope isolation,
    kind/shadow filtering, lexical scoring, update reindexing, missing-row
    `ErrorKind::not_found`, and idempotent deletes.

### Design Intent

The slice deliberately ships lexical search only. `Fts5Backend` satisfies the
existing `Backend` seam and leaves `VectorBackend`, sqlite-vec, hybrid ranking,
and prompt recall composition for later runtime slices. This keeps the storage
contract reviewable while preserving the memory design doc's split between the
record/lexical backend and future vector composition.

### Files Modified

- `include/oran/memory/longterm.hpp`
- `src/oran-memory/longterm_fts5.cpp`
- `src/oran-memory/migrations/longterm/0001-longterm-fts5-initial.sql`
- `tests/memory/test_longterm.cpp`
- `xmake/packages.lua`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/memory-system.md` — documents shipped `Fts5Backend`, schema
  ownership, remaining runtime/vector work, and migration ownership.
- `docs/product-specs/0005-memory-system.md` — updates v1 memory scope and
  acceptance status for the shipped lexical backend.
- `docs/ARCHITECTURE.md` — records the `oran-memory` backend and clarifies that
  `oran-storage` remains a generic SQLite/pool/migration layer.
- `docs/BUILD_SYSTEM.md` — records the SQLite FTS5 package flag.
- `docs/references/third-party-libs.md` and `docs/rules/libraries.md` — update
  SQLite guidance/approval metadata for FTS5 and memory-owned migrations.
- `docs/QUALITY_SCORE.md` and `docs/STATUS.md` — update the current-state
  snapshot, test count, slice number, and remaining memory follow-ups.
- `docs/exec-plans/tech-debt-tracker.md` — closes the FTS5 repository half of
  the P3 memory follow-up and leaves sqlite-vec/runtime recall as remaining work.

### Validation

- Commands run:
  - `xmake build test-memory`
  - `xmake run test-memory`
  - `make ci`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `xmake test`
- Tests added/changed:
  - `tests/memory/test_longterm.cpp` adds public `Fts5Backend` coverage for
    migration, scoped search, filtering, update/reindex, deletion, and
    missing-row behavior.
- Bench impact:
  - No new bench; this is the correctness slice for the lexical backend. The
    existing memory search A/B bench remains planned for the runtime/vector
    composition step.
- Compile-budget delta:
  - Not measured in this slice; the new implementation is isolated to
    `oran-memory`, and public header growth is limited to the backend type and
    migration report return type.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: existing deep-review tracker row now leaves gated
  sqlite-vec, hybrid/vector composition, and recall/runtime wiring as remaining
  memory work.
- Linked release note: none; this is internal runtime infrastructure.
