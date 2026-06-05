## [2026-06-05 20:27] | Task: long-term hybrid runtime

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI
- Linked plan: none; small follow-up slice after the FTS5 baseline

### User Query

> Deeply understand the project architecture and current progress, then start the
> next slice and commit it.

### Changes Overview

- Areas: `oran-memory`, memory tests, bootstrap version banner, memory docs.
- Key actions: added `memory::longterm::HybridSearchRequest` and
  `HybridRuntime`, validated hybrid search inputs, merged lexical and vector
  hits with deterministic weighted scores, hydrated vector-only hits through
  `Backend::get`, skipped stale vector rows, reused recall framing, and bumped
  the slice banner to 172.

### Design Intent

The previous slice measured the FTS5 baseline; this slice pins the adapter-neutral
composition contract before adding sqlite-vec. Keeping `HybridRuntime` above the
lexical and vector traits preserves the existing dependency boundary:
`Fts5Backend` remains lexical-only, optional vector adapters stay behind
`VectorBackend`, and default builds still pay no sqlite-vec compile cost.

### Files Modified

- `include/oran/memory.hpp`
- `include/oran/memory/longterm.hpp`
- `src/oran-memory/longterm.cpp`
- `src/oran-memory/longterm_runtime.cpp`
- `tests/memory/test_longterm.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 172, pointed at this history, recorded the
  new hybrid contract, and refreshed the next intended memory slice.
- `docs/ARCHITECTURE.md` — `oran-memory` inventory now lists
  `HybridSearchRequest` / `HybridRuntime`.
- `docs/design-docs/memory-system.md` — long-term status and API snippets now
  describe hybrid composition semantics.
- `docs/product-specs/0005-memory-system.md` — scope, acceptance criteria, and
  test counts now include the hybrid runtime contract.
- `docs/QUALITY_SCORE.md` — memory/test rows now include the new `test-memory`
  count and remaining vector work.
- `docs/exec-plans/tech-debt-tracker.md` — the deep-review memory row now marks
  the first hybrid/vector composition contract closed.

### Validation

- Commands run:
  - `xmake build test-memory`
  - `xmake run test-memory`
- Tests added/changed: `test-memory` now covers hybrid input validation,
  lexical/vector score merge ordering, vector-only record hydration, stale
  vector-row skipping, and hybrid recall framing.
- Bench impact: no new benchmark; this is the library-local composition
  contract the future sqlite-vec/vector corpus bench will call.
- Compile-budget delta: not measured separately; one public memory header and one
  existing memory implementation TU changed, with no new default dependency.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: existing P3 memory follow-up now narrows to the gated
  sqlite-vec adapter, hybrid ranking policy/wiring, and vector-vs-FTS5 corpus
  comparison.
- Linked release note: none — internal memory runtime API.
