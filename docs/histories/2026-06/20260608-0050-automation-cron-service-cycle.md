## [2026-06-08 00:50] | Task: Automation cron service cycle

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue implementing the most valuable current slice in `orangutan-refactor`
> with docs-first workflow, focused validation, and a Conventional Commit.

### Changes Overview

- Areas: `oran-automation`, bootstrap versioning, automation cron docs.
- Key actions:
  - Added `CronServiceCycleRequest`, `CronServiceCycleResult`, and
    `AutomationRuntime::run_cron_service_cycle(...)`.
  - The runtime validates explicit service-cycle policy before applying seeds,
    then applies caller-supplied cron seeds and delegates execution to the
    existing finite `CronLoop::run(...)` surface.
  - Added runtime coverage for seed-apply plus cron loop execution through the
    new cycle helper.
  - Added runtime coverage proving invalid cycle policy fails before seeds are
    written to `automation.db`.
  - Bumped the binary slice tag to `2.0.0-slice205`.

### Design Intent

This slice gives embedders a single explicit startup-cycle handoff without
turning bootstrap into a scheduler. A caller that already opened
`AutomationRuntime` can pass mapped config seeds, hook options, a handler, and
time/iteration/wait budgets; the runtime applies the seeds and awaits one
finite cron loop cycle.

Invalid service-cycle policy is rejected before repository mutation, so callers
do not get half-started state where cron rows were applied but the cycle cannot
run. The helper is still an awaited call; it does not spawn detached timers,
own a long-running process loop, enqueue work, notify channels, or call agents.

### Files Modified

- `include/oran/automation/runtime.hpp`
- `include/oran/automation.hpp`
- `src/oran-automation/runtime.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_runtime.cpp`
- `docs/STATUS.md`
- `docs/QUALITY_SCORE.md`
- `docs/ARCHITECTURE.md`
- `docs/design-docs/automation-runtime.md`
- `docs/product-specs/0006-automation.md`
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/automation-runtime.md` — documents the explicit service
  cycle API, validation-before-seed-apply behavior, and non-daemon boundary.
- `docs/product-specs/0006-automation.md` — records slice 205 and narrows the
  remaining scheduler-service gaps.
- `docs/ARCHITECTURE.md` — updates automation ownership notes and remaining
  scheduler gaps.
- `docs/QUALITY_SCORE.md` — refreshes automation test counts and next target.
- `docs/STATUS.md` — bumps the project snapshot to slice 205.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — records the
  progress and decision log entry.
- `docs/releases/feature-release-notes.md` — adds the user-facing release note.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `build/linux/x86_64/release/test-automation "AutomationRuntime runs a caller-awaited cron service cycle"`
  - `build/linux/x86_64/release/test-automation "AutomationRuntime validates cron service cycles before applying seeds"`
  - `xmake run test-automation` — 59 cases / 768 assertions
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap` — 129 cases / 1087 assertions
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - Added runtime coverage for explicit seed-apply plus finite cron loop
    execution through `run_cron_service_cycle(...)`.
  - Added runtime coverage for validation-before-seed-apply behavior.
- Bench impact: not benchmark-relevant; the helper delegates to the existing
  finite cron loop and adds no scheduler hot path.
- Compile-budget delta: small public structs plus one method body in existing
  `runtime.cpp`; no new target or dependency.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
