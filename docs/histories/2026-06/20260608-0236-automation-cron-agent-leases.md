## [2026-06-08 02:36] | Task: Automation cron agent leases

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

- Areas: `oran-config`, `oran-bootstrap`, `oran-automation`, automation runtime
  docs/status/history/release.
- Key actions:
  - Added optional `automation.cron.jobs[].agent_key` parsing with default
    `automation` and non-empty validation.
  - Added `agent_key` to cron seed descriptors and stored cron job records.
  - Added migration v7 for `automation_cron_jobs.agent_key` and
    `automation_cron_agent_leases`.
  - Added `AutomationRepository::acquire_cron_agent_lease(...)` /
    `release_cron_agent_lease(...)` plus public request/record types.
  - Updated `CronService::execute_due(...)` so enabled lease ownership acquires
    both the per-job cron lease and the stored job's per-agent lease before
    running the handler, then releases both after durable outcome work.
  - Updated cron lifecycle hook payloads to use the stored cron job `agent_key`.

### Design Intent

Slice 209 prevented two explicit runtime owners from overlapping the same stored
cron job. Spec 0006 also requires the first per-agent execution lease boundary:
two different cron jobs targeting the same agent must not run concurrently once
the explicit loop path is acting like the scheduler owner. This slice adds that
smallest useful boundary by storing an `agent_key` on cron jobs and leasing by
that key during explicit due execution. It does not add queue hold/drop policy,
notifier routing, triggered jobs, detached scheduler startup, or actual agent
invocation.

### Files Modified

- `config.example.json`
- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `include/oran/automation/repository.hpp`
- `src/oran-automation/repository.cpp`
- `src/oran-automation/service.cpp`
- `src/oran-automation/migrations/automation/0007-automation-cron-agent-leases.sql`
- `src/oran-bootstrap/automation_cron.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/config/test_config.cpp`
- `tests/bootstrap/test_memory_retention.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`
- `tests/automation/test_repository.cpp`
- `tests/automation/test_service.cpp`
- `tests/automation/test_runtime.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — moved the project snapshot to slice 210.
- `docs/design-docs/automation-runtime.md` — documented cron agent key storage,
  migration v7, repository agent-lease APIs, service/loop semantics, and
  validation count.
- `docs/product-specs/0006-automation.md` — recorded the current status for the
  per-agent lease acceptance boundary and remaining queue/notifier/agent work.
- `docs/ARCHITECTURE.md` — updated config, storage, automation, and bootstrap
  library inventory for cron agent keys and agent leases.
- `docs/QUALITY_SCORE.md` — updated automation/config/bootstrap coverage and
  test counts.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — recorded
  progress and validation for the active cron plan.
- `docs/releases/feature-release-notes.md` — added the slice 210 release note.
- `docs/histories/2026-06/20260608-0236-automation-cron-agent-leases.md` —
  this history entry.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `xmake run test-automation`
  - `xmake build test-config`
  - `xmake run test-config`
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - Config coverage for explicit/default cron `agent_key` parsing and empty
    `agent_key` rejection.
  - Bootstrap coverage proving mapped cron seeds preserve explicit/default
    agent keys.
  - Repository coverage for cron job `agent_key` round-trips, migration v7,
    agent lease acquire, active conflict, expired takeover, release, and
    validation.
  - Service coverage proving active same-agent leases suppress handlers and
    release the already-acquired job lease.
  - Runtime loop coverage proving default cron loop ownership uses agent leases
    before handler execution.
- Bench impact: not perf-relevant; scheduler tick benchmark remains downstream.
- Compile-budget delta: one embedded SQL migration in the existing
  `oran-automation` repository translation unit; no new translation units or
  dependencies.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
