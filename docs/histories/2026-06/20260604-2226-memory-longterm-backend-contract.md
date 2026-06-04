## [2026-06-04 22:26] | Task: Memory Long-Term Backend Contract

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none

### User Query

> Re-orient on the repository, start the next slice, keep docs in sync, use
> Codegraph, and commit the finished work.

### Changes Overview

- Areas: `oran-memory`, memory design/spec docs, deep-review tracker.
- Key actions: added `<oran/memory/longterm.hpp>` with typed long-term record
  shapes, reflection-backed `RecordKind`, lexical `Backend`, vector
  `VectorBackend`, vector request/result shapes, and validation helpers; added
  focused `test-memory` coverage with fake async backend implementations; bumped
  the binary slice stamp to 160.

### Design Intent

This closes the vector-backend trait half of the deep-review P3 memory follow-up
without pulling sqlite-vec into the default build or inventing a storage schema
before the FTS5 repository slice. The contract keeps scope keys on every record
lookup/removal to avoid future cross-agent leaks, uses `core::enum_name` for
record-kind wire spelling, and validates search limits, record metadata, and
embedding finiteness before adapter code lands.

### Files Modified

- `include/oran/memory.hpp`
- `include/oran/memory/longterm.hpp`
- `src/oran-memory/longterm.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/memory/test_longterm.cpp`
- `docs/design-docs/memory-system.md`
- `docs/product-specs/0005-memory-system.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/STATUS.md`
- `docs/exec-plans/tech-debt-tracker.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/memory-system.md` — documents the shipped long-term
  contracts and what remains downstream.
- `docs/product-specs/0005-memory-system.md` — marks the contract prework as
  shipped while leaving runtime/search acceptance criteria open.
- `docs/ARCHITECTURE.md` — updates the `oran-memory` inventory row.
- `docs/QUALITY_SCORE.md` — updates the memory-tier row and test count.
- `docs/STATUS.md` — bumps slice/current history/test count.
- `docs/exec-plans/tech-debt-tracker.md` — narrows the remaining P3 memory item
  to FTS5/sqlite-vec implementation work.

### Validation

- Commands run:
  - `xmake build test-memory`
  - `xmake run test-memory`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `xmake run test-bootstrap`
  - `make ci`
  - `xmake test`
  - `xmake -j$(nproc)`
- Tests added/changed:
  - Added `tests/memory/test_longterm.cpp`; focused result is 16 cases / 623
    assertions for `test-memory`.
- Bench impact:
  - None; no runtime backend or search algorithm landed.
- Compile-budget delta:
  - Small `oran-memory` TU/header addition only; no new dependency and no
    optional sqlite-vec package wiring.

### Follow-ups

- Gated sqlite-vec adapter and the FTS5 repository/runtime remain tracked in
  `docs/exec-plans/tech-debt-tracker.md`.
