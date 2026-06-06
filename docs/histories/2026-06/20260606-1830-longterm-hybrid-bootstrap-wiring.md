## [2026-06-06 18:30] | Task: long-term hybrid bootstrap wiring

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none

### User Query

Continue the most valuable implementation slice for Orangutan without defaulting
to another benchmark-only step; follow the repository docs-first workflow and
finish with a Conventional Commit.

### Changes Overview

- Areas: `oran-memory`, `oran-bootstrap`, configured-route memory policy.
- Key actions: added deterministic local text/record embedding helpers, gave
  `RuntimeAssembly` gated ownership of a separate sqlite-vec vector-memory DB,
  mapped `memory.longterm.hybrid_search` into `AgentPromptRunner`, routed
  prompt-boundary recall and `memory.recall` through `HybridRuntime`, and
  mirrored `memory.remember` / `memory.forget` into the vector index.

### Design Intent

Slice 176 made sqlite-vec available as an optional library backend and slice 177
measured it, but configured-route startup still could not consume the hybrid
policy. This slice closes that runtime wiring while preserving the dependency
boundary: default builds still reject enabled hybrid search before assembly or
provider side effects, while `--vector_memory=y` builds own the vector DB and
consume the validated policy. The local embedding owner is deterministic
plumbing, not a semantic embedding model; external/provider-backed embeddings
remain a separate runtime capability.

### Files Modified

- `include/oran/memory/longterm.hpp`
- `src/oran-memory/longterm.cpp`
- `include/oran/bootstrap/runtime_assembly.hpp`
- `src/oran-bootstrap/runtime_assembly.cpp`
- `include/oran/bootstrap/prompt_runner.hpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/memory/test_longterm.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `tests/bootstrap/test_bootstrap.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumped slice/status and recorded focused validation counts.
- `docs/ARCHITECTURE.md` - refreshed `oran-config`, `oran-memory`, and
  `oran-bootstrap` inventory text for gated hybrid wiring.
- `docs/design-docs/memory-system.md` - documented the deterministic embedding
  owner, vector DB, hybrid runtime consumption, and remaining semantic gap.
- `docs/design-docs/bootstrap-runtime.md` - documented vector-memory assembly
  options/accessors, startup banner text, and default-build guard behavior.
- `docs/design-docs/secrets-and-state.md` - updated typed config consumption
  notes for hybrid search.
- `docs/product-specs/0005-memory-system.md` - moved bootstrap hybrid wiring out
  of future scope and refreshed acceptance/coverage status.
- `docs/QUALITY_SCORE.md` - refreshed bootstrap and memory-tier status.
- `docs/exec-plans/tech-debt-tracker.md` - removed bootstrap embedding/vector
  ownership from the open P3 memory debt.
- `docs/releases/feature-release-notes.md` - added the user-visible slice note.

### Validation

- Commands run:
  - `xmake f -m release -c --vector_memory=n`
  - `xmake build test-memory`
  - `xmake run test-memory`
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
  - `xmake f -m release -c --vector_memory=y`
  - `xmake build test-memory`
  - `xmake run test-memory`
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
- Tests added/changed: text/record embedding coverage, gated vector assembly
  ownership, configured-route hybrid startup, hybrid prompt-boundary recall,
  hybrid `memory.recall`, and `memory.remember` vector mirroring.
- Bench impact: no new benchmark; slice 177 already measured sqlite-vec on the
  shared 10k-record corpus.
- Compile-budget delta: no new dependency in default builds; `--vector_memory=y`
  continues to own sqlite-vec as an explicit optional build.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: semantic/external embedding provider, memory lifecycle
  hooks, decay, optional `MEMORY.md` mirror, and spec-0010 unified benchmark JSON
  remain separate future slices.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
