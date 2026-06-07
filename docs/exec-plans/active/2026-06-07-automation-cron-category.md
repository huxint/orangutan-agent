# Automation Cron Category

## Goal

Land the first cron-category planning and state boundaries for
`oran-automation` without starting a scheduler service. The plan starts with a
deterministic POSIX 5-field cron parser/evaluator that returns the next due
fire for a caller-owned clock and stored state, then adds durable repository
state for cron schedules and last-fired timestamps. Cron-authored config,
service-loop startup, queues, notifiers, and agent firing stay in later
scheduler slices.

## Scope

- In scope:
- Add a public cron schedule shape and evaluator in `oran-automation`.
- Support POSIX-style 5-field expressions (`minute hour day-of-month month
  day-of-week`) with `*`, lists, ranges, and steps.
- Persist cron jobs through `AutomationRepository` using an automation-owned
  `automation_cron_jobs` migration, with upsert/load/list/mark-fired APIs.
- Validate stored cron expressions through the evaluator, while keeping
  evaluation deterministic and caller-clocked.
- Keep the slice free of config, hook, agent, timer, or background task
  ownership.
- Update automation docs/status/history/release notes in the same slice.
- Out of scope:
- Cron-authored config fields or a scheduler service that reads config.
- Process timers, triggered jobs, queueing/backpressure, notifier routing, and
  agent firing.
- Bootstrap opening `automation.db`, starting timers, or spawning detached
  automation work.
- Scheduler tick performance work beyond focused correctness coverage.

## Context

- Relevant docs:
- `docs/STATUS.md`
- `docs/product-specs/0006-automation.md`
- `docs/design-docs/automation-runtime.md`
- `docs/rules/docs-in-sync.md`
- `docs/rules/testing-and-bench.md`
- Relevant code paths:
- `include/oran/automation/periodic.hpp`
- `src/oran-automation/periodic.cpp`
- `include/oran/automation/repository.hpp`
- `src/oran-automation/repository.cpp`
- `src/oran-automation/migrations/automation/0003-automation-cron-jobs.sql`
- `tests/automation/test_periodic.cpp`
- `tests/automation/test_repository.cpp`
- `tests/automation/test_runtime.cpp`
- `include/oran/automation.hpp`
- Constraints:
- Keep `oran-automation` independent of `oran-config`, `oran-agent`, and
  bootstrap scheduling ownership.
- Public headers must stay lightweight and third-party-free.
- Cron evaluation must not skip/coalesce missed firings; later service policy
  decides catch-up/drop behavior.
- Compile-budget impact (if any):
- Implementation stays in the existing `periodic.cpp` translation unit and
  repository state stays in the existing `repository.cpp` translation unit plus
  one embedded SQL migration; no new third-party dependency is added.

## Risks

- Risk: cron parsing becomes a hidden scheduler. Mitigation: expose only
  deterministic evaluation over caller-supplied `now` and state.
- Risk: day-of-month/day-of-week semantics are ambiguous. Mitigation: document
  the POSIX/Vixie-style OR behavior when both fields are restricted.
- Risk: impossible schedules cause unbounded scans. Mitigation: bound the
  search window and return a validation error when no matching fire is found.
- Risk: persistence becomes scheduler ownership by accident. Mitigation: store
  only schedule/state rows and leave config reads, timers, hooks, queues, and
  agent firing out of the repository boundary.

## Milestones

1. **Cron evaluator.**
   Add `CronSchedule` and `evaluate_cron_schedule(...)` with parser and focused
   tests for exact fires, future fires, stored state, steps/lists/ranges, and
   validation. Shipped in slice 197.
2. **Cron repository state.**
   Add durable cron job rows and repository APIs for schedule/last-fired state
   without reading config or starting timers. Shipped in slice 198.
3. **Scheduler/category owner.**
   In progress: slice 199 layers stored cron jobs into the explicit automation
   runtime with a caller-driven scan/wait surface, and slice 200 adds explicit
   due execution that advances state only after a caller-supplied handler
   succeeds. Later: own cron config and service/timer startup policy without
   bootstrap-owned background work.
4. **Triggered/notifier/queue policy.**
   Later: add triggered categories, queueing/backpressure, notifier routing,
   and agent execution leases.
5. **Scheduler performance.**
   Later: measure the 1 000-job scheduler tick criterion once the scheduler
   exists.

## Validation

- Commands:
- `xmake build test-automation`
- `build/linux/x86_64/release/test-automation "[repository]"`
- `xmake run test-automation`
- `xmake build oran-automation`
- `xmake build orangutan`
- `xmake run orangutan -- --help`
- `git diff --check`
- `make ci`
- Manual checks:
- Confirm cron evaluator is pure and caller-clocked.
- Confirm cron repository writes validate through the evaluator and store only
  durable schedule/state.
- Confirm bootstrap still does not open or run `automation.db`.
- Confirm no new dependency direction crosses from automation into bootstrap,
  config, or agent.
- Observability checks:
- Not applicable in the first cron evaluator slice; no hook events are emitted.
- Bench comparison (if perf-relevant):
- Not perf-relevant until a scheduler tick loop exists.

## Progress Log

- [x] 2026-06-07 17:56 +0800: Selected cron-category planning as the next
  spec-0006 boundary after the retention loop reached the explicit runtime
  policy layer. This slice stays pure and does not introduce scheduler startup.
- [x] 2026-06-07 18:00 +0800: Implemented `CronSchedule` plus
  `evaluate_cron_schedule(...)` as a caller-clocked POSIX 5-field UTC
  evaluator with `*`, lists, ranges, steps, Sunday `0`/`7`, DOM/DOW OR
  semantics, bounded impossible-schedule scans, and no persistence or runtime
  service ownership.
- [x] 2026-06-07 18:31 +0800: Implemented `automation_cron_jobs` plus
  `AutomationRepository` cron upsert/load/list/mark-fired APIs. Stored cron
  schedules validate through `evaluate_cron_schedule(...)`; missing reads
  return `std::nullopt`; missing mark-fired mutations return `not_found`; no
  cron config ownership, timers, hook production, queues, notifiers, or agent
  firing were added.
- [x] 2026-06-07 21:46 +0800: Implemented `CronService::tick(...)`,
  `CronLoop::run_once(...)`, and `AutomationRuntime` cron factories. The cron
  service scans stored cron jobs and reports due jobs plus earliest next fire;
  the loop can wait once within a caller budget and re-tick; neither path reads
  config, marks jobs fired, publishes hooks, queues work, notifies channels, or
  calls agents.
- [x] 2026-06-07 22:38 +0800: Implemented
  `CronService::execute_due(...)` plus the public execute request/result shapes.
  The service invokes a supplied handler for each due cron job and advances
  stored cron state only after handler success. Handler failures remain
  per-attempt results and leave `last_fired_at` unchanged for retry; no cron
  config ownership, hooks, queues, notifiers, process timers, or agent firing
  were added.
- [x] **Update the docs that this slice invalidates in the same PR**
  (`docs/rules/docs-in-sync.md`).
- [x] Run validation and record results.
- [x] Update `docs/QUALITY_SCORE.md` row.
- [x] Write history entry.
- [x] Add release note.

## Decision Log

- 2026-06-07: Start cron work with parsing/evaluation, not service startup.
  The cron acceptance criterion needs a deterministic schedule primitive before
  a process service loop can route jobs, and a pure evaluator keeps bootstrap
  free of hidden automation ownership.
- 2026-06-07: Persist cron jobs at the repository boundary before adding config
  or timer ownership. This gives later runtime/service slices durable state
  while keeping bootstrap free of automatic `automation.db` opening.
- 2026-06-07: Add a read-only cron scan/wait runtime boundary before cron job
  execution. Repeated loops would re-see the same due fire until an execution
  owner advances `last_fired_at`, so this slice intentionally ships only
  `run_once(...)` and leaves mark-fired policy downstream.
- 2026-06-07: Add cron due execution as a caller-supplied handler boundary, not
  as a scheduler. The handler result is the only success signal that permits
  `last_fired_at` advancement; handler failures leave the fire due for explicit
  retry, and process retry/backpressure policy remains downstream.

## Linked Artifacts

- Related design doc: `docs/design-docs/automation-runtime.md`
- Related product spec: `docs/product-specs/0006-automation.md`
- PRs:
- History entry:
- `docs/histories/2026-06/20260607-1800-automation-cron-schedule.md`
- `docs/histories/2026-06/20260607-1831-automation-cron-persistence.md`
- `docs/histories/2026-06/20260607-2146-automation-cron-runtime-tick.md`
- `docs/histories/2026-06/20260607-2238-automation-cron-execute-due.md`
- Release note:
- `docs/releases/feature-release-notes.md#2026-06`
