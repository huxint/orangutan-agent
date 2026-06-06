## [2026-06-06 13:29] | Task: Long-term sqlite-vec corpus bench

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none

### User Query

> Continue the next implementation slice, keep docs aligned, use codegraph for
> orientation, and commit when finished.

### Changes Overview

- Areas: `bench-memory`, long-term memory docs, release/status tracking.
- Key actions: extended `bench/memory/scenarios/search_fts5_vs_vector.cpp` with
  optional sqlite-vec vector-only and FTS5+sqlite-vec hybrid rows under
  `--vector_memory=y`, while preserving the default dependency-free comparison
  shape. Bumped the binary slice tag to `2.0.0-slice177`.

### Design Intent

Slice 176 shipped the real sqlite-vec `VectorBackend`, but the shared 10k corpus
comparison still only exercised the in-bench brute-force reference vector backend.
This slice keeps the benchmark at the existing runtime seam instead of wiring
bootstrap hybrid recall prematurely: the same deterministic record keys and
embeddings feed `SqliteVecBackend`, and the default FTS5 pool remains free of
sqlite-vec extension registration by using a separate temporary vector DB for the
gated rows. That gives maintainers evidence before the larger embedding/vector
owner lands.

### Files Modified

- `bench/memory/scenarios/search_fts5_vs_vector.cpp`
- `bench/memory/README.md`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — moved the snapshot to slice 177 and recorded local gated
  sqlite-vec corpus timings.
- `docs/design-docs/memory-system.md` — documented the gated sqlite-vec
  benchmark rows and kept bootstrap hybrid wiring downstream.
- `docs/product-specs/0005-memory-system.md` — closed the corpus search
  performance criterion for sqlite-vec and narrowed AC7 to unified JSON output.
- `docs/ARCHITECTURE.md` — recorded the new `oran-memory` benchmark evidence.
- `docs/QUALITY_SCORE.md` — added the sqlite-vec vector/hybrid bench numbers and
  removed the sqlite-vec corpus bench from the memory-tier next step.
- `docs/exec-plans/tech-debt-tracker.md` — marked sqlite-vec corpus comparison
  numbers as closed.
- `docs/releases/feature-release-notes.md` — added the slice 177 maintainer-facing
  release note.
- `bench/memory/README.md` — described the gated sqlite-vec rows and default
  benchmark shape.

### Validation

- Commands run before final gate:
  - `xmake f -m release -c --vector_memory=n`
  - `xmake build bench-memory`
  - `xmake run bench-memory`
  - `xmake f -m release -c --vector_memory=y`
  - `xmake build bench-memory`
  - `xmake run bench-memory`
- Bench impact: default release build still reports the original three search
  comparison rows. Gated release run adds
  `memory.longterm_search_vector_sqlite_vec_10k_limit10` at **~3.03 ms / batch**
  and `memory.longterm_search_hybrid_fts5_sqlite_vec_10k_limit10` at
  **~18.96 ms / batch**.
- Compile-budget delta: not measured; this slice changes one benchmark TU plus
  docs/status metadata.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: `docs/exec-plans/tech-debt-tracker.md` now leaves embedding
  ownership, hybrid ranking/bootstrap wiring, and unified JSON bench emission as
  the remaining memory P3 work.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`.
