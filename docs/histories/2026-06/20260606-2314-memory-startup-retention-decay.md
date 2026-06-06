## [2026-06-06 23:14] | Task: memory-startup-retention-decay

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI
- Linked plan: none

### User Query

Continue the most valuable implementation slice in the current repository using the
docs-first workflow, avoid bench-only churn, keep docs/status/history in sync, and
finish with a Conventional Commit.

### Changes Overview

- Areas: bootstrap runtime assembly, configured-route startup, long-term memory
  retention.
- Key actions:
  - Added `LongtermMemoryStartupDecayOptions` to `RuntimeAssemblyOptions`.
  - Mapped `memory.longterm.retention` into one startup decay pass for the stable
    configured-route `cli` scope.
  - Ran that pass after long-term memory migration and before exposing the
    long-lived memory pool/backend/runtime.
  - Added assembly-level and provider-route regressions proving stale low-importance
    records are shadowed before prompt-boundary recall.

### Design Intent

Slice 183 made retention policy authorable but did not consume it. This slice closes
the smallest useful runtime gap without pretending periodic automation exists yet:
configured-route startup runs one bounded lexical decay pass before prompt/tool reads.
`decay_check_interval_hours` is intentionally left for the future `oran-automation`
cadence owner, and decay lifecycle hooks remain deferred until a producer can publish
where configured subscribers are attached.

### Files Modified

- `include/oran/bootstrap/runtime_assembly.hpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-bootstrap/runtime_assembly.cpp`
- `tests/bootstrap/test_bootstrap.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`

### Docs Updated In This PR

- `docs/STATUS.md` — slice 184 snapshot, history pointer, focused validation, and
  next-slice candidates.
- `docs/design-docs/bootstrap-runtime.md` — new startup decay assembly option and
  configured-route startup ordering.
- `docs/design-docs/memory-system.md` — retention policy ownership now includes the
  startup pass while keeping periodic scheduling downstream.
- `docs/design-docs/secrets-and-state.md` — config status updated for retention
  startup consumption.
- `docs/product-specs/0005-memory-system.md` — acceptance/status updated for startup
  retention consumption and remaining automation/hook gaps.
- `docs/ARCHITECTURE.md` — config/memory library inventory updated for slice 184.
- `docs/QUALITY_SCORE.md` — bootstrap/memory status and test counts updated.
- `docs/releases/feature-release-notes.md` — user-visible release note added.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap` — 119 cases / 1002 assertions
- Tests added/changed:
  - `RuntimeAssembly::build applies long-term startup decay before exposing memory`
  - `RuntimeAssembly::build rejects long-term startup decay when long-term memory is disabled`
  - `run applies configured memory retention before configured provider prompts`
- Bench impact: no benchmark change; this is one-shot startup correctness, not a
  performance trade-off.
- Compile-budget delta: one public bootstrap option with `core::Time`; no new
  third-party dependency or heavy public include beyond existing in-repo core time.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  [`docs/releases/feature-release-notes.md`](../../releases/feature-release-notes.md)
