## [2026-06-07 15:00] | Task: Automation Retention Leases

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI
- Linked plan:
  `docs/exec-plans/active/2026-06-07-automation-retention-cadence.md`

### User Query

> Continue the most valuable current implementation slice, follow the repository
> process, read docs/current progress, avoid bench-only churn, keep
> docs/status/history synchronized, validate, and commit with a Conventional
> Commit subject.

### Changes Overview

- Areas: `oran-automation`, automation persistence, retention loop ownership,
  docs/status/release/history.
- Key actions:
  - Added automation migration version 2 for
    `automation_memory_retention_leases`.
  - Added `AutomationRepository` acquire/release APIs for memory-retention job
    leases, including active-lease conflicts, expired-lease replacement,
    missing-job `not_found`, matching-owner release, and input validation.
  - Changed `MemoryRetentionLoop::run_once(...)` to plan and wait without a
    lease, then acquire the stored lease only around due
    `MemoryRetentionService::tick(...)` execution and release it afterward.
  - Kept bootstrap unopened for `automation.db` and did not add a detached
    scheduler or long-running service loop.

### Design Intent

The prior slice made due retention ticks observable through job lifecycle
events. The next useful product boundary was preventing overlapping execution
for the same stored job without starting a scheduler. The loop deliberately does
not hold a lease while waiting: a caller cancellation during that wait returns
`ErrorKind::cancelled` without leaving retained lease state. Acquiring
immediately before due execution still prevents concurrent owners from running
the same due retention job.

### Files Modified

- `include/oran/automation.hpp`
- `include/oran/automation/loop.hpp`
- `include/oran/automation/repository.hpp`
- `include/oran/automation/service.hpp`
- `src/oran-automation/loop.cpp`
- `src/oran-automation/repository.cpp`
- `src/oran-automation/service.cpp`
- `src/oran-automation/migrations/automation/0002-automation-retention-leases.sql`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_repository.cpp`
- `tests/automation/test_runtime.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumped to slice 195 and refreshed latest/next slice text.
- `docs/ARCHITECTURE.md` - recorded automation-owned retention lease rows and
  loop-side due-run lease ownership.
- `docs/BUILD_SYSTEM.md` - updated the automation summary.
- `docs/QUALITY_SCORE.md` - refreshed automation coverage and next-step wording.
- `docs/design-docs/automation-runtime.md` - documented lease API, migration,
  loop semantics, validation, and future ownership.
- `docs/design-docs/memory-system.md` - recorded that leases stay in automation,
  not memory.
- `docs/design-docs/storage-runtime.md` - noted lease rows live above generic
  storage.
- `docs/design-docs/secrets-and-state.md` - recorded lease owner keys as
  non-secret scheduler metadata.
- `docs/product-specs/0005-memory-system.md` - reflected retained job lease
  ownership without moving periodic execution into memory.
- `docs/product-specs/0006-automation.md` - recorded the slice 195 lease
  boundary and remaining scheduler gaps.
- `docs/exec-plans/active/2026-06-07-automation-retention-cadence.md` - added
  milestone/progress/decision-log entries.
- `docs/releases/feature-release-notes.md` - added the slice 195 release note.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build test-automation`
  - `xmake run test-automation`
  - `xmake build oran-automation`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed:
  - Added repository coverage for lease acquire/conflict/expiry/release/reacquire
    semantics and invalid lease inputs.
  - Added loop coverage for due-run lease release, active-lease conflict, and
    invalid lease TTL.
  - Added loop coverage proving cancellation while waiting does not retain a
    lease and backend failure still releases the due-run lease.
  - Focused result: `test-automation` passed with 30 cases / 390 assertions.
  - Binary sanity: `xmake run orangutan -- --help` reports
    `orangutan v2.0.0-slice195`.
  - Base gate: `make ci` passed, including STATUS freshness and dependency
    layering checks.
- Bench impact: no bench change; this is a correctness/ownership boundary with
  no competing implementation choice.
- Compile-budget delta: one embedded SQL migration and small repository/loop
  surface over existing automation dependencies; no new third-party dependency.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#automation-retention-leases`
