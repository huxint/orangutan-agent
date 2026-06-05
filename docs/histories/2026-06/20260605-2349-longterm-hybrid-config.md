## [2026-06-05 23:49] | Task: Long-Term Hybrid Search Config

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: local CLI, Asia/Shanghai
- Linked plan: none

### User Query

> Deeply understand the project architecture and current implementation progress, then start the next slice and commit it.

### Changes Overview

- Areas: `oran-config`, long-term memory docs, release/status tracking.
- Key actions: added `LongtermMemoryHybridSearchConfig`, parsed
  `memory.longterm.hybrid_search`, validated positive per-backend/result limits,
  non-negative finite weights, the non-zero combined-weight invariant, and nested
  strict/loose unknown-field behavior.

### Design Intent

Slice 173 established the FTS5/vector/hybrid bench baseline and slice 172 already
defined `HybridRuntime` validation. This slice makes the operator-facing config
contract explicit before bootstrap owns embeddings or an optional vector backend.
That keeps the next runtime slice from inventing config semantics while also
avoiding the larger dependency/build-option work of a gated sqlite-vec adapter.

### Files Modified

- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/config/test_config.cpp`
- `config.example.json`

### Docs Updated In This PR (Prime Directive)

- `docs/STATUS.md` — moved the snapshot to slice 174 and recorded the next slice options.
- `docs/ARCHITECTURE.md` — documented the config/memory inventory change.
- `docs/design-docs/memory-system.md` — documented the hybrid-search config policy and parser-only status.
- `docs/design-docs/secrets-and-state.md` — documented the new config fields and strictness surface.
- `docs/product-specs/0005-memory-system.md` — recorded the shipped config contract and updated validation counts.
- `docs/QUALITY_SCORE.md` — updated `oran-config` counts and memory/config status notes.
- `docs/releases/feature-release-notes.md` — added the user-visible config release note.

### Validation

- Commands run:
  - `xmake build test-config && xmake run test-config`
- Tests added/changed:
  - `test-config` now covers hybrid-search parsing, defaults, malformed values,
    all-zero weights, and strict/loose unknown fields.
- Bench impact:
  - None; parser-only config surface.
- Compile-budget delta:
  - Not measured; one config TU and one test TU changed.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  [`docs/releases/feature-release-notes.md`](../../releases/feature-release-notes.md)
