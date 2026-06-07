## [2026-06-07 13:30] | Task: Automation Retention Job Lifecycle Hooks

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI
- Linked plan:
  `docs/exec-plans/completed/2026-06-07-automation-retention-cadence.md`

### User Query

> Continue the most valuable current slice, follow the repository process, keep
> docs and current status synchronized, avoid bench-only churn, and use a
> repo-style commit message.

### Changes Overview

- Areas: `oran-automation`, `oran-hook`, docs/status/release/history.
- Key actions:
  - Added typed `hook::JobLifecyclePayload`.
  - Published advisory `job_started`, `job_failed`, and `job_finished` from due
    `MemoryRetentionService::tick(...)` calls.
  - Added automation coverage for not-due, success, and backend-failure
    lifecycle behavior.
  - Kept lifecycle publishing advisory and left leases/service-loop ownership
    downstream.

### Design Intent

Lifecycle events now come from the explicit retention tick owner because that is
the first boundary where a real durable job run starts and finishes. Outcome
events are emitted only after durable run/state transitions, avoiding false
success/failure if repository persistence fails. Bootstrap remains free of
hidden background work; leases and long-running scheduling policy stay in the
active automation-retention plan.

### Files Modified

- `include/oran/hook/payload.hpp`
- `include/oran/automation.hpp`
- `include/oran/automation/service.hpp`
- `src/oran-automation/service.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_service.cpp`
- `tests/hook/test_bus.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumped to slice 194 and refreshed latest/next slice text.
- `docs/ARCHITECTURE.md` - recorded `JobLifecyclePayload` and the automation
  lifecycle producer boundary.
- `docs/QUALITY_SCORE.md` - refreshed automation/hook coverage and next-step
  wording.
- `docs/BUILD_SYSTEM.md` - updated the automation dependency summary.
- `docs/design-docs/automation-runtime.md` - documented lifecycle publish
  timing and remaining service-loop gaps.
- `docs/design-docs/permissions-and-hooks.md` - added the hook payload and
  automation producer.
- `docs/design-docs/memory-system.md` - narrowed retention lifecycle gaps to
  service-loop ownership.
- `docs/design-docs/storage-runtime.md` - noted lifecycle publishing remains
  above generic storage.
- `docs/design-docs/secrets-and-state.md` - recorded that lifecycle metadata
  adds no secret material.
- `docs/product-specs/0005-memory-system.md` - reflected lifecycle metadata from
  periodic retention ticks.
- `docs/product-specs/0006-automation.md` - recorded slice 194 automation
  lifecycle hook behavior.
- `docs/exec-plans/completed/2026-06-07-automation-retention-cadence.md` - added
  milestone/progress/decision-log entries.
- `docs/releases/feature-release-notes.md` - added the slice 194 release note.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `xmake run test-automation`
  - `xmake build test-hook`
  - `xmake run test-hook`
  - `git diff --check`
  - `scripts/check-deps.sh`
  - `xmake build oran-hook`
  - `xmake build oran-automation`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed: lifecycle tests in `tests/automation/test_service.cpp`
  plus hook payload-kind recognition in `tests/hook/test_bus.cpp`.
- Focused results: `test-automation` passed with 27 cases / 327 assertions, and
  `test-hook` passed with 37 cases / 299 assertions.
- Bench impact: no bench change; this slice adds observability/correctness
  metadata, not a new performance tradeoff.
- Compile-budget delta: no new target and no new third-party dependency;
  `oran-automation` already depends on `oran-hook`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#automation-retention-job-lifecycle-hooks`
