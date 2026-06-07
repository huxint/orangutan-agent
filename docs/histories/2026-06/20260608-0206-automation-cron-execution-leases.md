## [2026-06-08 02:06] | Task: Automation cron execution leases

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue implementing the most valuable `orangutan-refactor` slice end to
> end, using the repository workflow rather than blindly following the next
> `STATUS.md` line. Some active docs may be stale or unarchived.

### Changes Overview

- Areas: `oran-automation`, automation runtime docs/status/history/release.
- Key actions:
  - Added migration v6 `automation_cron_leases`.
  - Added `AutomationRepository::acquire_cron_lease(...)` /
    `release_cron_lease(...)` plus public cron lease request/record types.
  - Added request-level lease owner/TTL fields to `CronService::execute_due(...)`.
  - Made `CronLoop::run(...)` and `AutomationRuntime::run_cron_service_cycle(...)`
    default to `automation-cron-loop` lease ownership.
  - Added repository, service, and loop/runtime tests for active conflict,
    expired takeover, release, and handler-suppression behavior.

### Design Intent

Spec 0006 calls out execution leases before broader scheduler ownership. The
retention path already had repository-backed due-run leases; cron execution did
not, so two runtime owners could observe the same due job and run a handler
before either marked the job fired. This slice adds the smallest useful
product boundary: per-cron-job stored leases for explicit execution. Direct
`CronService::execute_due(...)` remains opt-in so manual service callers do not
gain hidden locking semantics, while finite loops and runtime service cycles
default to `automation-cron-loop` ownership because they are the scheduler-like
explicit path.

### Files Modified

- `include/oran/automation/repository.hpp`
- `include/oran/automation/service.hpp`
- `include/oran/automation/loop.hpp`
- `include/oran/automation/runtime.hpp`
- `src/oran-automation/repository.cpp`
- `src/oran-automation/service.cpp`
- `src/oran-automation/loop.cpp`
- `src/oran-automation/runtime.cpp`
- `src/oran-automation/migrations/automation/0006-automation-cron-leases.sql`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_repository.cpp`
- `tests/automation/test_service.cpp`
- `tests/automation/test_runtime.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — moved the project snapshot to slice 209.
- `docs/design-docs/automation-runtime.md` — documented cron lease API,
  migration v6, service/loop semantics, and validation count.
- `docs/product-specs/0006-automation.md` — recorded the current status for
  execution-lease acceptance coverage and remaining agent-facing lease work.
- `docs/ARCHITECTURE.md` — updated storage and automation library inventory.
- `docs/QUALITY_SCORE.md` — updated automation coverage and test counts.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — recorded
  progress and validation for the active cron plan.
- `docs/releases/feature-release-notes.md` — added the slice 209 release note.
- `docs/histories/2026-06/20260608-0206-automation-cron-execution-leases.md`
  — this history entry.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `build/linux/x86_64/release/test-automation "AutomationRepository::migrate applies the automation schema once"`
  - `build/linux/x86_64/release/test-automation "[unit][automation][repository][cron][lease]"`
  - `build/linux/x86_64/release/test-automation "[unit][automation][service][cron][lease]"`
  - `build/linux/x86_64/release/test-automation "[unit][automation][runtime][cron][loop][lease]"`
  - `xmake run test-automation`
  - `xmake build orangutan`
  - `xmake build test-bootstrap`
  - `xmake run orangutan -- --help`
  - `xmake run test-bootstrap`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - Repository coverage for cron lease acquire, active conflict, expired
    takeover, release, and input validation.
  - Service coverage for opt-in execution leases, release after durable success,
    active conflict before handler execution, and invalid lease TTL.
  - Runtime loop coverage proving the default loop owner blocks handler work
    when another active lease exists.
- Bench impact: not perf-relevant; scheduler tick benchmark remains downstream.
- Compile-budget delta: no new translation units; existing
  `oran-automation` files only.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
