## [2026-06-05 16:04] | Task: memory forget tool

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI
- Linked plan: none; small follow-up slice after slice 169

### User Query

> next slice.

### Changes Overview

- Areas: `oran-tool`, `oran-memory`, `oran-bootstrap`, memory/tool docs.
- Key actions: registered deferred `memory.forget`, added the
  `DispatchContext::memory_forget` callback boundary, rendered structured
  `memory_forget` output in `oran-memory`, and bound the bootstrap runner to the
  assembly-owned long-term backend delete path.

### Design Intent

This completes the delete side of the first permissioned long-term memory tool
surface without changing the storage contract. `oran-tool` owns only JSON parsing,
capability metadata, and delegation so it stays independent of `oran-memory`;
bootstrap owns scope derivation and backend access; `oran-memory` owns the
structured output shape. The backend delete remains idempotent via the existing
`memory::longterm::Backend::remove(RecordKey)` contract.

### Files Modified

- `include/oran/tool/builtins.hpp`
- `include/oran/tool/registry.hpp`
- `include/oran/memory/longterm.hpp`
- `src/oran-tool/builtins.cpp`
- `src/oran-tool/registry.cpp`
- `src/oran-tool/memory_forget.cpp`
- `src/oran-memory/longterm_runtime.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `tests/tool/test_registry.cpp`
- `tests/memory/test_longterm.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — library inventory now lists `memory.forget`,
  callback binding, and structured removed-key output.
- `docs/STATUS.md` — slice pointer, latest-history pointer, focused test counts,
  and long-term memory tracker status updated for slice 170.
- `docs/exec-plans/tech-debt-tracker.md` — removed `memory.forget` from the P3
  long-term memory follow-up list.
- `docs/QUALITY_SCORE.md` — focused test counts and memory/tool/bootstrap status
  updated.
- `docs/design-docs/memory-system.md` — long-term tool state now includes
  delete-side `memory.forget`.
- `docs/design-docs/tool-runtime.md` — built-in catalog and memory callback
  boundary updated.
- `docs/design-docs/bootstrap-runtime.md` — runner binding over the long-term
  backend delete path recorded.
- `docs/product-specs/0002-tool-registry.md` — shipped built-in catalog updated.
- `docs/product-specs/0005-memory-system.md` — delete-side long-term memory tool
  acceptance state recorded.
- `docs/product-specs/0012-tool-scheduler-and-state.md` — mutating memory tool
  classification includes `memory.forget`.
- `docs/product-specs/0014-structured-tool-output.md` — structured output list
  includes `memory.forget`.
- `docs/releases/feature-release-notes.md` — user-visible release row added.

### Validation

- Commands run:
  - `xmake build test-tool && xmake run test-tool`
  - `xmake build test-memory && xmake run test-memory`
  - `xmake build test-bootstrap && xmake run test-bootstrap`
- Tests added/changed: tool registration/delegation/error coverage, memory
  `render_forget_data_json(...)` coverage, and bootstrap runner end-to-end
  delete coverage against the scoped long-term backend.
- Bench impact: no new bench; this slice uses the existing backend delete path
  and adds no competing implementation choice.
- Compile-budget delta: not measured separately; no public heavy includes or new
  third-party dependencies were added.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#memory-forget-tool`.
