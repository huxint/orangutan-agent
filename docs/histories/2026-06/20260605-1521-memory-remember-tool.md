## [2026-06-05 15:21] | Task: memory-remember-tool

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local xmake release build
- Linked plan: none - small tracker slice after `memory.recall`

### User Query

> Deeply understand the current architecture/progress, start the next slice,
> iterate continuously, keep docs synced with reality, and commit when finished.

### Changes Overview

- Areas: `oran-tool`, `oran-memory`, `oran-bootstrap`, focused tests, docs.
- Key actions: added deferred `memory.remember` with `write_memory` capability;
  added `MemoryRememberRequest` plus `DispatchContext::memory_remember`; added
  `memory::longterm::render_remember_data_json(...)`; and bound
  `AgentPromptRunner` to the assembly-owned long-term memory backend so provider
  tool calls can upsert one scoped record through the normal
  permission/audit/hook/output-cap path.

### Design Intent

This is the narrow write-side sibling to slice 168's read-only recall tool:
`oran-tool` owns JSON parsing, schema, permission declaration, hooks, audit, and
output caps, but still does not depend on `oran-memory`. Bootstrap supplies the
stable scope key, dispatch-time timestamps, and backend write. The memory layer
owns structured saved-record serialization because it owns the `Record` contract
and the private JSON dependency.

### Files Modified

- `include/oran/tool/registry.hpp`
- `include/oran/tool/builtins.hpp`
- `src/oran-tool/memory_remember.cpp`
- `src/oran-tool/builtins.cpp`
- `src/oran-tool/registry.cpp`
- `include/oran/memory/longterm.hpp`
- `src/oran-memory/longterm_runtime.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/tool/test_registry.cpp`
- `tests/memory/test_longterm.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumps slice/status and focused test counts.
- `docs/ARCHITECTURE.md` - records the new write-side memory tool, memory JSON
  renderer, and runner callback binding in the library inventory.
- `docs/design-docs/tool-runtime.md` - documents shipped `memory.remember`.
- `docs/design-docs/memory-system.md` - updates long-term memory status.
- `docs/design-docs/bootstrap-runtime.md` - documents runner-owned remember
  tool binding.
- `docs/product-specs/0002-tool-registry.md` - updates built-in scope/status.
- `docs/product-specs/0005-memory-system.md` - updates v1 status and acceptance
  counts.
- `docs/product-specs/0012-tool-scheduler-and-state.md` - marks
  `memory.remember` as a shipped mutating tool.
- `docs/product-specs/0014-structured-tool-output.md` - records
  `memory.remember` structured output shape.
- `docs/QUALITY_SCORE.md` - refreshes memory/bootstrap/tool test counts and
  next steps.
- `docs/exec-plans/tech-debt-tracker.md` - narrows remaining memory follow-ups.
- `docs/releases/feature-release-notes.md` - adds the user-visible memory tool
  note.

### Validation

- Commands run:
  - `xmake build test-tool`
  - `xmake build test-memory`
  - `xmake build test-bootstrap`
  - `xmake run test-tool` - 204 cases / 2140 assertions
  - `xmake run test-memory` - 27 cases / 723 assertions
  - `xmake run test-bootstrap` - 113 cases / 859 assertions
  - `make ci` - base docs/hygiene/dependency/prompt-preamble checks passed
  - `xmake test` - 16 test targets passed
- Tests added/changed: tool registration/delegation/missing-runtime coverage,
  memory remember malformed-input coverage, memory remember `data_json`
  coverage, and bootstrap provider/tool-loop write coverage against the real
  long-term backend.
- Bench impact: no new bench; this is a narrow tool binding over the existing
  FTS5 backend.
- Compile-budget delta: not measured separately; new code is one small
  `oran-tool` TU plus memory/bootstrap edits within existing targets.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: remaining memory work is now `memory.forget`, gated
  sqlite-vec/vector composition, and hybrid ranking.
- Linked release note: `docs/releases/feature-release-notes.md`
