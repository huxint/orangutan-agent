## [2026-06-07 04:24] | Task: bootstrap retention job

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local xmake / GCC 16.1 release build
- Linked plan:
  `docs/exec-plans/active/2026-06-07-automation-retention-cadence.md`

### User Query

> Continue implementing the most valuable product-capability slice in the
> active plan, avoid bench-only churn, keep docs/status/history synchronized,
> validate the result, and commit with a Conventional Commit subject.

### Changes Overview

- Areas: bootstrap, automation retention mapping, long-term memory retention
  ownership docs, build dependency docs.
- Key actions: added the bootstrap-owned config-to-automation retention mapping
  helper, stored the resulting `MemoryRetentionJob` descriptor on
  `RuntimeAssembly`, derived startup decay options from the same job, and linked
  `oran-bootstrap` / `orangutan` to `oran-automation` for this descriptor
  boundary only.

### Design Intent

Slice 187 gave automation a pure planner but left configured-route retention
policy represented only as startup-decay options. This slice makes bootstrap the
composition root for `config::Config` -> `automation::MemoryRetentionJob`
mapping so `oran-automation` stays independent of `oran-config` and
`oran-memory` stays independent of scheduling. The descriptor's `first_fire_at`
is startup time plus `decay_check_interval_hours`, which lets a future periodic
owner start after the one-shot startup pass instead of immediately repeating it.
No background loop, `automation.db`, lease, backend execution, or periodic hook
producer is introduced.

### Files Modified

- `include/oran/bootstrap/memory_retention.hpp`
- `include/oran/bootstrap/runtime_assembly.hpp`
- `include/oran/bootstrap.hpp`
- `src/oran-bootstrap/memory_retention.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-bootstrap/runtime_assembly.cpp`
- `tests/bootstrap/test_memory_retention.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`
- `xmake/targets.lua`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/automation-runtime.md` — records the shipped bootstrap
  descriptor mapping and keeps service/persistence downstream.
- `docs/product-specs/0006-automation.md` — adds the slice-188 shipped prework.
- `docs/design-docs/bootstrap-runtime.md` — documents the new options/accessor
  and startup-vs-periodic ownership split.
- `docs/design-docs/memory-system.md` and
  `docs/product-specs/0005-memory-system.md` — distinguish descriptor mapping
  from periodic execution/publishing.
- `docs/design-docs/secrets-and-state.md` — updates config/state status and the
  reserved `automation.db` note.
- `docs/design-docs/module-boundaries.md`, `docs/ARCHITECTURE.md`, and
  `docs/BUILD_SYSTEM.md` — document bootstrap's new downward dependency on
  automation for descriptor mapping.
- `docs/STATUS.md`, `docs/QUALITY_SCORE.md`, and
  `docs/releases/feature-release-notes.md` — move the project snapshot and
  release notes to slice 188.
- `docs/exec-plans/active/2026-06-07-automation-retention-cadence.md` — marks
  the bootstrap/runner ownership milestone complete.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build oran-bootstrap`
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `xmake build test-automation && xmake run test-automation`
  - `make ci`
- Tests added/changed:
  - `tests/bootstrap/test_memory_retention.cpp` covers config-to-job mapping and
    startup-decay derivation from the job.
  - `tests/bootstrap/test_runtime_assembly.cpp` covers descriptor storage and
    rejection when long-term memory is disabled.
  - Focused results: `test-bootstrap` passed with 124 cases / 1054 assertions,
    and `test-automation` passed with 7 cases / 40 assertions.
- Bench impact: none; this is config/bootstrap composition and not a hot path.
- Compile-budget delta: one small bootstrap TU plus a public bootstrap helper
  header; no new third-party dependency.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#bootstrap-retention-job`
