## [2026-06-07 22:57] | Task: Automation cron loop run policy

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue implementing the most valuable current slice in `orangutan-refactor`
> with docs-first workflow, focused validation, and a Conventional Commit.

### Changes Overview

- Areas: `oran-automation`, cron runtime loop policy, docs/status/release notes.
- Key actions:
  - Added `CronLoop::run(...)` plus finite run request/result/stop-reason
    shapes.
  - Reused `CronService::execute_due(...)` as the only due-execution owner, so
    stored cron state still advances only after handler success.
  - Added runtime tests for overdue catch-up, handler-failure stop behavior, and
    invalid run policy inputs.
  - Bumped the binary slice tag to `2.0.0-slice201`.

### Design Intent

The active cron-category plan needed one bounded policy layer above explicit due
execution before cron config or process service-loop startup. This slice keeps
the owner caller-driven: it can catch up overdue cron fires one stored fire at a
time and wait only inside a caller-provided total budget, but it still does not
read cron config, start timers, publish lifecycle hooks, enqueue work, route
notifications, or call agents. Handler failures stop the loop and remain visible
in the final execution result instead of being retried immediately; retry and
backpressure policy stay downstream.

### Files Modified

- `include/oran/automation.hpp`
- `include/oran/automation/loop.hpp`
- `src/oran-automation/loop.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_runtime.cpp`
- `docs/ARCHITECTURE.md`
- `docs/STATUS.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/automation-runtime.md`
- `docs/product-specs/0006-automation.md`
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — library inventory and bootstrap ownership note now
  include finite cron run policy.
- `docs/design-docs/automation-runtime.md` — public API and cron loop semantics
  now describe `CronLoop::run(...)`.
- `docs/product-specs/0006-automation.md` — shipped prework/current
  implementation now include slice 201.
- `docs/QUALITY_SCORE.md` — automation/test rows reflect the new focused test
  count and loop-policy coverage.
- `docs/STATUS.md` — snapshot, slice tag, history pointer, and next boundary
  refreshed.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — progress
  and decision log updated for this plan milestone.
- `docs/releases/feature-release-notes.md` — user-facing feature note added for
  the public runtime API.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `xmake run test-automation`
  - `xmake build oran-automation`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - Added runtime coverage for finite cron loop overdue catch-up.
  - Added runtime coverage for handler failure stopping without immediate retry.
  - Extended invalid cron scan policy coverage with run-policy validation.
- Bench impact: not benchmark-relevant; no scheduler tick loop exists yet.
- Compile-budget delta: no new targets, dependencies, or translation units.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
