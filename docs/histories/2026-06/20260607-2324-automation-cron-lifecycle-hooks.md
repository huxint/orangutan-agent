## [2026-06-07 23:24] | Task: Automation cron lifecycle hooks

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue implementing the most valuable current slice in `orangutan-refactor`
> with docs-first workflow, focused validation, and a Conventional Commit.

### Changes Overview

- Areas: `oran-automation`, cron due execution observability, hook metadata.
- Key actions:
  - Added `CronHookOptions` and `CronServiceOptions`.
  - Let `AutomationRuntime::cron_service(...)` and `cron_loop(...)` pass those
    options into runtime-owned cron services.
  - Published advisory `job_started`, `job_finished`, and `job_failed`
    metadata from `CronService::execute_due(...)`.
  - Added focused tests for handler success and handler failure lifecycle
    metadata.
  - Bumped the binary slice tag to `2.0.0-slice202`.

### Design Intent

Cron now has a real explicit execution boundary but still no process scheduler.
That handler boundary is the right place to expose lifecycle observability:
`job_started` fires before caller work, `job_finished` fires only after handler
success and durable cron state advancement, and `job_failed` fires after handler
failure while leaving the stored fire due for retry. The hook path is advisory
and optional, so observers cannot veto handler execution or turn cron into a
hidden background service. Cron config, queues, notifiers, timers, and agent
firing remain downstream.

### Files Modified

- `include/oran/automation.hpp`
- `include/oran/automation/service.hpp`
- `include/oran/automation/runtime.hpp`
- `src/oran-automation/service.cpp`
- `src/oran-automation/runtime.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_service.cpp`
- `docs/ARCHITECTURE.md`
- `docs/STATUS.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/automation-runtime.md`
- `docs/design-docs/permissions-and-hooks.md`
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md`
- `docs/product-specs/0006-automation.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — automation inventory and bootstrap ownership notes
  include cron lifecycle metadata.
- `docs/design-docs/automation-runtime.md` — public API and cron execution
  semantics now describe cron hook options and event timing.
- `docs/design-docs/permissions-and-hooks.md` — hook producer notes include the
  cron `JobLifecyclePayload` producer.
- `docs/product-specs/0006-automation.md` — shipped prework/current
  implementation/open items reflect slice 202.
- `docs/QUALITY_SCORE.md` — test counts and automation coverage rows updated.
- `docs/STATUS.md` — snapshot, slice tag, history pointer, focused result, and
  next boundary refreshed.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — progress
  and decision log updated for this milestone.
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
  - Added cron lifecycle metadata coverage for handler success.
  - Added cron lifecycle metadata coverage for handler failure without state
    advancement.
- Bench impact: not benchmark-relevant; no scheduler tick loop or hot path
  policy changed.
- Compile-budget delta: no new targets, dependencies, or translation units.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
