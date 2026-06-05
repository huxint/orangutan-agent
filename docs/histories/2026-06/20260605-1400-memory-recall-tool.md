## [2026-06-05 14:00] | Task: memory-recall-tool

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local xmake release build
- Linked plan: none — small tracker slice after the memory runtime v1 arc

### User Query

> Deeply understand the current architecture/progress, start the next slice, iterate
> continuously, use codegraph MCP, and commit when finished.

### Changes Overview

- Areas: `oran-tool`, `oran-memory`, `oran-bootstrap`, focused tests, docs.
- Key actions: added deferred `memory.recall` with `read_memory` capability;
  added `DispatchContext::memory_recall` plus a plain `MemoryRecallRequest`;
  added `memory::longterm::render_recall_data_json(...)`; and bound
  `AgentPromptRunner` to the assembly-owned `memory::longterm::Runtime` so
  provider tool calls return deterministic recall text plus structured record
  metadata through the normal permission/audit/hook path.

### Design Intent

This is the smallest useful memory-tool slice: read-only recall first, no write/forget
mutation semantics yet, and no new sibling dependency from `oran-tool` to
`oran-memory`. The tool layer owns schema, parsing, permission declaration, audit,
hooks, and output caps; bootstrap adapts the runtime service exactly like the existing
skill callback pattern. The memory layer owns serialization of memory record metadata
because it already owns `SearchHit` and the private nlohmann dependency.

### Files Modified

- `include/oran/tool/registry.hpp`
- `include/oran/tool/builtins.hpp`
- `src/oran-tool/memory_recall.cpp`
- `src/oran-tool/builtins.cpp`
- `src/oran-tool/registry.cpp`
- `include/oran/memory/longterm.hpp`
- `src/oran-memory/longterm_runtime.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/tool/test_registry.cpp`
- `tests/memory/test_longterm.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumps slice/status and focused test counts.
- `docs/ARCHITECTURE.md` — records the new memory tool, memory JSON renderer, and
  runner callback binding in the library inventory.
- `docs/design-docs/tool-runtime.md` — documents shipped `memory.recall`.
- `docs/design-docs/memory-system.md` — updates long-term memory status.
- `docs/design-docs/bootstrap-runtime.md` — documents runner-owned memory recall
  tool binding.
- `docs/product-specs/0002-tool-registry.md` — updates built-in scope/status.
- `docs/product-specs/0005-memory-system.md` — updates v1 status and acceptance
  counts.
- `docs/product-specs/0014-structured-tool-output.md` — records `memory.recall`
  structured output shape.
- `docs/QUALITY_SCORE.md` — refreshes memory/bootstrap/tool test counts and next
  steps.
- `docs/exec-plans/tech-debt-tracker.md` — narrows the remaining memory follow-up.
- `docs/releases/feature-release-notes.md` — adds the user-visible memory tool note.

### Validation

- Commands run:
  - `xmake build test-tool`
  - `xmake build test-memory`
  - `xmake build test-bootstrap`
  - `xmake run test-tool` — 200 cases / 2080 assertions
  - `xmake run test-memory` — 26 cases / 714 assertions
  - `xmake run test-bootstrap` — 112 cases / 838 assertions
  - `make ci` — base docs/hygiene/dependency/prompt-preamble checks passed
  - `xmake test` — 16 test targets passed
- Tests added/changed: tool registration/delegation/missing-runtime coverage,
  memory recall `data_json` coverage, and bootstrap provider/tool-loop coverage.
- Bench impact: no new bench; this is a narrow tool binding over the existing FTS5
  runtime. The 10 k-record search bench remains open.
- Compile-budget delta: not measured separately; new code is one small `oran-tool`
  TU plus memory/bootstrap edits within existing targets.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: remaining memory work is now `memory.remember` /
  `memory.forget`, gated sqlite-vec/vector composition, and hybrid ranking.
- Linked release note: `docs/releases/feature-release-notes.md`
