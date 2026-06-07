## [2026-06-08 01:07] | Task: automation cron run history

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue the most valuable automation cron/category slice, keep docs in sync,
> validate it, and commit with a Conventional Commit message.

### Changes Overview

- Areas: `oran-automation`, automation migrations, cron runtime docs.
- Key actions: added durable cron run rows through migration version 4, public
  repository record/list APIs, `CronExecuteAttempt::run`, and service-side
  success/failure recording for explicit due cron handler attempts.

### Design Intent

The automation spec requires failing jobs to be recorded with a failure reason,
but jumping straight to queues, notifiers, or agent firing would blur scheduler
ownership. This slice keeps the current explicit caller-owned cron execution
model and records only the outcome of handler attempts that already ran through
`CronService::execute_due(...)`. Not-due scans remain read-only, failed handlers
leave `last_fired_at` unchanged for retry, and bootstrap still does not open or
run `automation.db`.

### Files Modified

- `include/oran/automation/repository.hpp`
- `include/oran/automation/service.hpp`
- `src/oran-automation/migrations/automation/0004-automation-cron-runs.sql`
- `src/oran-automation/repository.cpp`
- `src/oran-automation/service.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_repository.cpp`
- `tests/automation/test_runtime.cpp`
- `tests/automation/test_service.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/automation-runtime.md` — records cron run persistence,
  API, execute-due semantics, and migration/test counts.
- `docs/product-specs/0006-automation.md` — marks explicit cron failure history
  as shipped while leaving broader scheduler policy downstream.
- `docs/ARCHITECTURE.md` — updates automation ownership notes for cron run
  history.
- `docs/STATUS.md` — bumps the project snapshot to slice 206.
- `docs/QUALITY_SCORE.md` — updates automation/test counts and shipped slice
  coverage.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — records the
  slice decision, validation, and progress.
- `docs/releases/feature-release-notes.md` — adds the slice 206 user-facing
  release note.

### Validation

- Commands run:
  `xmake build test-automation`;
  `build/linux/x86_64/release/test-automation "AutomationRepository records and lists cron runs"`;
  `build/linux/x86_64/release/test-automation "CronService::execute_due advances only successful due cron jobs"`;
  `build/linux/x86_64/release/test-automation "AutomationRuntime::open creates parent directories and migrates state"`;
  `xmake run test-automation`;
  `xmake build orangutan`;
  `xmake run orangutan -- --help`;
  `xmake build test-bootstrap`;
  `xmake run test-bootstrap`;
  `git diff --check`;
  `make ci`.
- Tests added/changed: repository coverage for cron run record/list and
  validation, service coverage for success/failure run rows and not-due
  suppression, runtime migration version 4 assertions.
- Bench impact: none; this is correctness/state history, not scheduler
  throughput.
- Compile-budget delta: one embedded SQL migration and repository/service code
  in existing translation units; no new dependency or TU.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
