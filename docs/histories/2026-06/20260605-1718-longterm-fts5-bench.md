## [2026-06-05 17:18] | Task: long-term FTS5 search bench

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI
- Linked plan: none; small measure-first slice after the memory tool arc

### User Query

> Deeply understand the project architecture and current progress, then start the
> next slice and commit it.

### Changes Overview

- Areas: `bench-memory`, `oran-bootstrap` version banner, memory docs.
- Key actions: added a seeded 10k-record long-term memory FTS5 benchmark,
  routed the measurement through `memory::longterm::Runtime::search`, tuned the
  expensive scenario's nanobench epochs, and bumped the slice banner to 171.

### Design Intent

The tracker names the vector-memory path as "measure first." This slice avoids
starting sqlite-vec or hybrid ranking without a baseline: it seeds the default
`Fts5Backend` once, then measures the runtime search boundary future vector and
hybrid implementations must beat or justify. Setup is outside the timed region,
and the scenario has local epoch settings so `bench-memory` stays practical.

### Files Modified

- `bench/memory/main.cpp`
- `bench/memory/scenarios/longterm_fts5.cpp`
- `bench/memory/README.md`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 171, pointed at this history, recorded
  local bench results, and refreshed latest library surface counts.
- `docs/design-docs/memory-system.md` — long-term status now records the FTS5
  10k search baseline and the remaining vector/hybrid work.
- `docs/product-specs/0005-memory-system.md` — AC2/AC7 now record the FTS5
  baseline and keep vector/hybrid results open.
- `docs/exec-plans/tech-debt-tracker.md` — P3 memory row now notes the shipped
  measured FTS5 baseline.
- `docs/QUALITY_SCORE.md` — bench/memory status now includes the long-term FTS5
  baseline.

### Validation

- Commands run:
  - `xmake build bench-memory`
  - `xmake build bench-memory && xmake run bench-memory`
- Tests added/changed: no Catch2 tests; this is a benchmark-only slice.
- Bench impact: local `bench-memory` run reported
  `memory.longterm_fts5_search_10k_limit10` at **~15.08 ms / batch** over a
  10k-record corpus; same run reported session store comparison at **~887.2 us**
  raw repository append/load vs. **~1041.3 us** typed store append/load.
- Compile-budget delta: not measured separately; no public headers or production
  dependencies changed.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none; existing P3 vector-memory follow-up remains open.
- Linked release note: none — this is internal benchmark/measurement work.
