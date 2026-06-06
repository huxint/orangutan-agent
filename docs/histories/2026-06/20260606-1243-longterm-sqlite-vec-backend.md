## [2026-06-06 12:43] | Task: Long-Term sqlite-vec Vector Backend

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI
- Linked plan: none

### User Query

Continue the next small slice after reading the project docs/status, use codegraph
for architecture context, keep the implementation aligned with docs, and commit the
finished work.

### Changes Overview

- Areas: `oran-memory`, `oran-storage`, xmake feature gating, docs/status.
- Key actions: added `memory::longterm::SqliteVecBackend` behind
  `--vector_memory=y`, taught `storage::Connection` / `Pool` to open SQLite
  connections with a temporary auto-extension list, pinned optional
  `sqlite-vec 0.1.9`, bumped the binary slice tag to `2.0.0-slice176`, and added
  default-build plus gated-build coverage.

### Design Intent

Slice 175 deliberately rejected configured-route hybrid search because bootstrap
did not own embeddings or a real vector backend. This slice closes the backend half
without widening bootstrap behavior: `SqliteVecBackend` satisfies the existing
`VectorBackend` contract and stores one scoped vector row per `RecordKey` in a
sqlite-vec `vec0` table, but configured-route hybrid recall remains guarded until a
prompt/query embedding owner can feed it. The storage auto-extension hook is a
caller-supplied span rather than a new option field so existing aggregate
initializers stay quiet under the repo's warning policy and default opens remain
unchanged.

### Files Modified

- `include/oran/memory/longterm.hpp`
- `src/oran-memory/longterm_sqlite_vec.cpp`
- `include/oran/storage/sqlite.hpp`
- `include/oran/storage/pool.hpp`
- `src/oran-storage/sqlite.cpp`
- `src/oran-storage/pool.cpp`
- `tests/memory/test_longterm.cpp`
- `tests/storage/test_sqlite.cpp`
- `xmake/options.lua`
- `xmake/packages.lua`
- `xmake/targets.lua`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR

- `docs/STATUS.md` — records slice 176, focused test counts, and remaining memory follow-ups.
- `docs/ARCHITECTURE.md` — records storage auto-extension support and the optional sqlite-vec backend.
- `docs/design-docs/memory-system.md` — documents the shipped backend and still-guarded bootstrap boundary.
- `docs/design-docs/storage-runtime.md` — documents `SqliteExtensionInit` and per-open auto-extension semantics.
- `docs/design-docs/bootstrap-runtime.md` — clarifies why the hybrid guard remains after the library backend lands.
- `docs/design-docs/secrets-and-state.md` — records the config/state boundary for the still-disabled hybrid path.
- `docs/product-specs/0005-memory-system.md` — updates scope, acceptance status, and test counts.
- `docs/BUILD_SYSTEM.md` — documents the optional package and build flag.
- `docs/rules/libraries.md` — updates the sqlite-vec version/license approval row.
- `docs/SUPPLY_CHAIN_SECURITY.md` — records the optional native-extension supply-chain boundary.
- `docs/QUALITY_SCORE.md` — updates storage/memory/test status and counts.
- `docs/releases/feature-release-notes.md` — adds the slice 176 release-note row.
- `docs/exec-plans/tech-debt-tracker.md` — removes the real gated vector backend from remaining P3 debt.

### Validation

- Commands run:
  - `xmake f -m release -c --vector_memory=n`
  - `xmake build test-memory && xmake run test-memory`
  - `make ci`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `xmake build test-storage && xmake run test-storage`
  - `xmake f -m release -c --vector_memory=y`
  - `xmake build test-memory && xmake run test-memory`
  - `xmake f -m release -c --vector_memory=n`
  - `xmake build test-memory && xmake run test-memory`
- Tests added/changed:
  - default-build `SqliteVecBackend` config-error coverage.
  - gated sqlite-vec migrate/upsert/search/remove and dimension-drift coverage.
  - null SQLite auto-extension rejection coverage.
- Bench impact: no new bench; sqlite-vec corpus comparison remains tracked as a follow-up.
- Compile-budget delta: small `oran-memory` TU plus optional package edge; default builds keep sqlite-vec unresolved.

### Follow-ups

- Tech-debt entries: sqlite-vec corpus comparison numbers, unified bench JSON emission,
  embedding/vector ownership, and real bootstrap hybrid consumption remain in
  `docs/exec-plans/tech-debt-tracker.md`.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
