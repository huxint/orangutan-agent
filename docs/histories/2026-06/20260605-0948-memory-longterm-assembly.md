## [2026-06-05 09:48] | Task: Bootstrap long-term memory assembly

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local shell + xmake/GCC 16.1
- Linked plan: none; small follow-up after the slice-162 long-term runtime seam.

### User Query

> Continue iterating after the long-term runtime recall slice, keep docs current,
> verify thoroughly, commit, and keep the next slice small.

### Changes Overview

- Areas: `oran-bootstrap`, long-term memory docs/status/history.
- Key actions:
  - Added optional long-term memory ownership to `RuntimeAssemblyOptions`.
  - Added `RuntimeAssembly` accessors for the assembly-owned long-term backend,
    runtime, enablement state, and DB path.
  - Provisioned `<workspace>/.orangutan/memory.db` by running the embedded
    `memory::longterm::Fts5Backend` migration before opening the long-lived pool.
  - Kept configured-route bootstrap startup enabled for long-term memory while
    built-in no-provider startup disables it, matching the existing session-state
    policy for fresh deterministic CLI runs.
  - Updated the bootstrap startup banner to report `longterm-memory=<state>`.
  - Added bootstrap tests for default provisioning, explicit DB path, disabled
    mode, idempotent re-run, no-route non-creation, and configured-route creation.

### Design Intent

Slice 162 proved the library-local recall runtime, but no process-owned service
could yet hold the default backend. This slice adds only the bootstrap assembly
ownership layer: lifetime, migration, DB path policy, and accessors. It does not
choose recall query policy, add config fields, render recall into section 5, or
ship memory tools. That keeps the next prompt-boundary slice able to consume an
owned `longterm::Runtime` without also debugging SQLite provisioning.

### Files Modified

- `include/oran/bootstrap/runtime_assembly.hpp`
- `src/oran-bootstrap/runtime_assembly.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`
- `tests/bootstrap/test_bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/bootstrap-runtime.md` — documents the new runtime assembly
  options/accessors, memory DB ownership, and banner state.
- `docs/design-docs/memory-system.md` — records slice-163 bootstrap ownership
  over `memory.db`, `Fts5Backend`, and `longterm::Runtime`.
- `docs/product-specs/0005-memory-system.md` — updates v1 scope and acceptance
  status to distinguish assembly ownership from remaining config/prompt recall.
- `docs/ARCHITECTURE.md` — updates the `oran-memory` and `oran-bootstrap`
  inventory rows with the new boundary.
- `docs/QUALITY_SCORE.md` and `docs/STATUS.md` — update slice number, latest
  history, test counts, and remaining follow-ups.
- `docs/exec-plans/tech-debt-tracker.md` — closes bootstrap assembly ownership
  and leaves config/query policy, prompt recall rendering, vector/hybrid work,
  and memory tools open.

### Validation

- Commands run:
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
  - `git diff --check`
  - `make ci`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `xmake test`
- Tests added/changed:
  - `tests/bootstrap/test_runtime_assembly.cpp` covers default `memory.db`
    provisioning, explicit long-term DB path, disabled long-term memory, idempotent
    migration re-run, and real backend/runtime recall through the assembly.
  - `tests/bootstrap/test_bootstrap.cpp` covers no-provider startup not creating
    `memory.db` and configured provider startup creating/migrating it.
- Bench impact:
  - No new bench; this slice is startup composition/lifetime ownership. The
    10k-record runtime search benchmark remains open.
- Compile-budget delta:
  - Not measured in this slice. The new code reuses existing `oran-bootstrap`
    and `oran-memory` translation units and adds no third-party dependencies.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: existing deep-review tracker row now leaves config/query
  recall policy, prompt-boundary recall rendering, memory tools, gated sqlite-vec,
  and hybrid ranking as remaining memory work.
- Linked release note: none; internal runtime infrastructure.
