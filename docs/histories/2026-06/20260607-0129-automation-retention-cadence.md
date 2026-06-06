## [2026-06-07 01:29] | Task: automation retention cadence

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local xmake / GCC 16.1 release build
- Linked plan:
  `docs/exec-plans/active/2026-06-07-automation-retention-cadence.md`

### User Query

Continue the most valuable implementation slice in the repo, follow the
docs-first workflow, avoid bench-only work, keep status/history/docs in sync,
validate the result, and commit with a Conventional Commit subject.

### Changes Overview

- Areas: automation, long-term memory retention planning, build/test/bench
  target registration, docs/status/history.
- Key actions: added the `oran-automation` library with deterministic
  periodic schedule evaluation and memory-retention request planning; registered
  `oran-automation`, `test-automation`, and `bench-automation`; kept
  automation retention validation aligned with config/memory by rejecting
  non-finite `importance_floor`; documented that cron parsing, `automation.db`,
  leases, background service ownership, and
  periodic `memory_decay` publishing remain downstream.

### Design Intent

The memory retention policy already had a shipped interval and the memory layer
already had `memory::longterm::DecayRequest`. This slice connects those facts at
the automation boundary without moving scheduling math into bootstrap or making
`oran-memory` depend upward on automation. The API is pure and caller-clocked so
future service code can add persistence, leases, missed-fire policy, and hook
publication without changing the planning contract.

### Files Modified

- `include/oran/automation.hpp`
- `include/oran/automation/periodic.hpp`
- `src/oran-automation/periodic.cpp`
- `tests/automation/main.cpp`
- `tests/automation/test_periodic.cpp`
- `bench/automation/main.cpp`
- `bench/automation/scenarios/periodic.cpp`
- `bench/automation/README.md`
- `xmake/targets.lua`
- `xmake/tests.lua`
- `xmake/bench.lua`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive)

- `docs/design-docs/automation-runtime.md` — documents the new planner API and
  out-of-scope scheduler/service ownership.
- `docs/product-specs/0006-automation.md` — records shipped prework and open
  acceptance criteria.
- `docs/design-docs/memory-system.md` and
  `docs/product-specs/0005-memory-system.md` — distinguish planner existence
  from periodic execution/publishing.
- `docs/design-docs/secrets-and-state.md` — clarifies that `automation.db` and
  config job seeds remain unimplemented.
- `docs/ARCHITECTURE.md`, `docs/BUILD_SYSTEM.md`, `AGENTS.md`, `include/README.md`,
  `src/README.md`, `tests/README.md`, and `bench/README.md` — register the new
  library, targets, routing, and bucket parity.
- `docs/STATUS.md`, `docs/QUALITY_SCORE.md`, and
  `docs/releases/feature-release-notes.md` — move the project snapshot and
  release notes to slice 187.

### Validation

- Commands run:
  - `xmake build oran-automation`
  - `xmake build test-automation`
  - `xmake run test-automation`
  - `xmake build bench-automation`
  - `xmake run bench-automation`
- Tests added/changed:
  - `test-automation` covers due/not-due schedule evaluation, last-fired
    advancement, invalid intervals, due-only retention requests, request field
    mapping, and retention input validation including non-finite importance.
- Bench impact:
  - `bench-automation` reports `automation.periodic_evaluate_1024` and
    `automation.memory_retention_plan_1024`.
- Compile-budget delta:
  - New one-TU `oran-automation` target uses stdlib plus public `oran-core` /
    `oran-memory` surfaces and adds no third-party dependency.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#automation-retention-cadence`
