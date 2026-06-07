# 0006 — Automation Engine

## User Problem

Operators want recurring agent work: "every morning at 9 am, summarize overnight CI
runs", "every 30 minutes, scan for new GitHub issues", "when a webhook fires, route
the payload to the research agent". The engine schedules and runs these jobs.

## Scope (v1)

- `oran-automation::Service` + `Runtime`.
- Job categories:
  - `cron` — POSIX-style cron expressions.
  - `periodic` — duration interval.
  - `triggered` — fired by external events (webhook, signal, file watch).
- Per-agent execution lease (prevent concurrent runs of same agent).
- Persistence in `automation.db` (jobs, runs, last-fired, cooldown).
- Per-category runners — built-in `heartbeat`, custom via `Category` interface.
- Hook events: `job_scheduled`, `job_started`, `job_finished`, `job_failed`.
- Notifier callback routes job output to cli / channel / desktop.

## Shipped Prework

Slice 187 adds the first `oran-automation` library boundary, slice 188 lets
bootstrap seed it from configured retention policy without creating the full
service, slice 189 adds the automation-owned persistent state boundary, slice
190 adds the first explicit caller-driven retention tick owner, slice 191
adds optional periodic `memory_decay` publishing from that tick owner, slice
192 adds the caller-owned automation runtime state handle, and slice 193 adds a
caller-started retention loop step. Slice 194 adds advisory job lifecycle
metadata from due retention ticks, slice 195 adds repository-backed retention
job leases plus due-run lease ownership in the loop step, and slice 196 adds a
finite caller-owned loop policy above that leased step. Slice 197 adds the
first cron-category planning primitive with a POSIX 5-field UTC parser and
deterministic next-fire evaluator, and slice 198 adds durable cron job state
through the same repository boundary. Slice 199 adds the first caller-driven
cron runtime scan/wait boundary, and slice 200 adds explicit caller-supplied due
execution that advances stored cron state only after handler success. Slice 201
adds finite caller-owned cron loop policy over that execution surface, slice
202 adds advisory cron job lifecycle metadata from the same explicit execution
owner, slice 203 adds typed `automation.cron.jobs[]` config seeds plus
bootstrap mapping into cron repository upsert descriptors, and slice 204 adds
explicit `AutomationRuntime::apply_cron_job_seeds(...)` persistence for those
mapped descriptors. The current API
evaluates periodic and cron
schedules from caller-supplied state, maps a long-term memory retention policy
into a due-only `memory::longterm::DecayRequest`, persists the configured
retention job plus run history and lease state through `AutomationRepository`,
persists cron job schedule/last-fired state through `AutomationRepository`,
explicitly opens/migrates automation state through `AutomationRuntime::open(...)`,
lets a runtime owner scan stored cron jobs for due work without mutating them,
lets a runtime owner execute due cron jobs through a supplied handler and mark
only handler-successful fires complete,
lets a runtime owner run a finite explicit cron loop that can catch up due
fires or wait within a caller budget,
publishes advisory cron job lifecycle metadata when the caller supplies a hook
bus,
parses and maps config-authored cron schedule seeds without starting a
scheduler,
lets a caller-owned automation runtime explicitly apply those cron seeds into
`automation.db`,
lets a runtime owner tick one stored retention job against a supplied long-term
memory backend, publishes advisory retention metadata when the caller supplies a
hook bus, can wait once within a caller budget for the earliest stored cron fire,
and can wait once within a caller budget for a stored retention job to become
due while leasing due execution or run a finite caller-owned loop over that
step. Bootstrap maps configured-route `memory.longterm.retention` into a stored
`MemoryRetentionJob` descriptor whose first fire is after the one-shot startup
decay pass, and maps `automation.cron.jobs[]` into
`UpsertCronJobRequest` descriptors, but bootstrap still does not open
`automation.db`, apply those rows, or run a background service.

Current implementation:

- `evaluate_periodic_schedule(PeriodicSchedule, PeriodicJobState, now)` returns
  due/not-due, the scheduled fire time, and overdue duration.
- `plan_memory_retention(MemoryRetentionJob, PeriodicJobState, now)` validates
  scope/policy fields, including finite `importance_floor`, and returns no
  request before the cadence is due.
- `bootstrap::longterm_memory_retention_job_from(...)` maps config retention
  values into automation-owned units while keeping `oran-automation`
  independent of `oran-config`.
- `RuntimeAssembly::longterm_memory_retention_job()` exposes the stored
  descriptor for diagnostics and future scheduler ownership; it is not run by
  `RuntimeAssembly::build`.
- `config::Config::automation().cron.jobs` exposes typed cron schedule seeds
  from `automation.cron.jobs[]`, while `bootstrap::cron_jobs_from(...)`
  validates the cron expressions through `evaluate_cron_schedule(...)` and maps
  them into repository upsert requests. Bootstrap performs that mapping for
  loaded config even when no provider route is configured.
- `RuntimeAssembly::cron_jobs()` exposes those mapped cron seeds for diagnostics
  and future persistence ownership; it is not persisted or run by
  `RuntimeAssembly::build`.
- `AutomationRuntime::apply_cron_job_seeds(...)` is the explicit persistence
  handoff. It upserts mapped cron seed rows through the caller-owned runtime
  repository, returns requested/upserted counts plus stored rows, and annotates
  failures with `seed_index` / `job_key` context.
- `AutomationRepository` runs migrations over a caller-supplied `storage::Pool`,
  upserts and loads retention jobs by durable `job_key`, persists
  `last_fired_at`, records success/failure run rows, lists recent runs, acquires
  retention job leases when no active lease exists or an existing lease has
  expired, releases leases only for the matching owner, and upserts/loads/lists
  cron jobs with durable schedule plus last-fired state.
- `AutomationRuntime::open(...)` validates an explicit database path, creates
  parent directories, opens `automation.db` through an owned `storage::Pool`,
  runs automation migrations, exposes the migration report and repository, can
  explicitly apply cron seed descriptors, and can construct `CronService`,
  `CronLoop`, `MemoryRetentionService`, or `MemoryRetentionLoop` over that
  stable state.
- `MemoryRetentionService::tick(...)` loads one stored job, skips not-due work
  without mutation, invokes `memory::longterm::Backend::decay(...)` only when
  due, records success/failure run rows, and advances `last_fired_at` only
  after success. Backend failures record a failed run and keep state unadvanced
  so retry policy remains explicit.
- `MemoryRetentionServiceOptions::hooks` lets callers provide a `hook::Bus`,
  source label, agent key, and identity. Successful due ticks publish advisory
  `memory_decay` metadata after durable state advances; not-due ticks and
  backend failures publish nothing, and advisory sink failures remain non-fatal.
- Due `MemoryRetentionService::tick(...)` calls also publish advisory
  `job_started`, `job_finished`, and `job_failed` metadata through the same hook
  settings. `job_started` fires before backend decay, `job_failed` fires after a
  backend failure is recorded as a failed run, and `job_finished` fires after a
  successful run row plus `last_fired_at` advancement. Not-due ticks publish no
  job lifecycle events.
- `MemoryRetentionLoop::run_once(...)` ticks a stored job immediately, returns
  the not-due result when the next fire is beyond `max_wait`, waits with
  `async::sleep_for(...)` when the next fire is within budget, leases only the
  due `MemoryRetentionService::tick(...)` execution, releases the lease after
  the tick, returns `ErrorKind::conflict` for active lease holders, propagates
  cancellation while waiting without holding a lease, and rejects invalid wait
  or lease budgets.
- `MemoryRetentionLoop::run(...)` repeatedly calls the leased `run_once(...)`
  step for one stored job until `max_iterations` is reached or a step returns
  no due work within the remaining wait budget. It reports iteration count,
  due-run count, total wait time, stop reason, and the last step, and can catch
  up overdue stored retention fires without owning a detached process loop.
- `CronSchedule` plus `evaluate_cron_schedule(...)` parse POSIX 5-field UTC
  cron expressions with `*`, lists, ranges, and steps; use `first_fire_at` as
  the never-fired anchor; advance from `PeriodicJobState::last_fired_at`; and
  return one next `PeriodicEvaluation` without starting a scheduler.
- Cron job repository APIs persist `CronSchedule` plus
  `PeriodicJobState::last_fired_at`, return `std::nullopt` for missing reads,
  return `ErrorKind::not_found` for missing mark-fired mutations, validate cron
  expressions before SQLite writes, and list rows by newest update.
- `CronService::tick(...)` scans stored cron jobs up to a caller limit,
  evaluates each stored schedule, and returns checked count, due jobs, and the
  earliest next fire without advancing `last_fired_at`.
- `CronLoop::run_once(...)` ticks immediately, waits through
  `async::sleep_for(...)` only when the earliest next cron fire is within the
  caller's `max_wait`, reports cancellation while waiting, and re-ticks after
  the wait.
- `CronService::execute_due(...)` reuses that scan result, invokes a
  caller-supplied handler for each due cron job, advances `last_fired_at` only
  after the handler succeeds, reports handler errors per attempt, and leaves
  failed-handler jobs due for retry by the next explicit call.
- `CronLoop::run(...)` repeatedly calls `execute_due(...)` up to a caller
  iteration limit, waits only within `max_total_wait`, aggregates
  attempted/advanced/failed counters, stops on `no_due_work`,
  `iteration_limit`, or `handler_failure`, and does not immediately retry
  failed handlers inside the same run.
- `CronServiceOptions::hooks` lets callers provide a `hook::Bus`, source label,
  agent key, and identity. Due cron execution publishes advisory `job_started`
  before the handler, `job_failed` after a handler failure while keeping cron
  state unadvanced, and `job_finished` only after handler success plus durable
  `last_fired_at` advancement. Advisory sink failures remain non-fatal.
- `test-automation` reports 57 cases / 747 assertions.
- `bench-automation` compares periodic schedule evaluation with retention
  request planning over a 1024-job batch.

Still open: automatic cron seed application from a process service owner,
triggered jobs, bootstrap/service-loop startup policy over `AutomationRuntime`,
broader per-agent/category leases for agent-facing jobs, queueing/backpressure,
process service/timer cancellation policy, notifier callbacks, and the
scheduler tick performance criterion. Job lifecycle publication exists for
explicit retention ticks and explicit cron due execution; full
scheduler/category lifecycle ownership remains downstream.

## Scope (v1.1)

- Per-job priority (urgent / normal / background).
- Backpressure: queue dropping with `job_dropped` hook.
- Job dependencies (A must complete before B fires).
- Conditional jobs (run only if predicate holds).

## Scope (v2)

- Distributed scheduling across runtimes (federation).
- Calendar-aware schedules (skip holidays, time-zone aware).

## Out Of Scope

- A full workflow engine (no DAGs of jobs); each job is independent.
- UI-based job builder; jobs configured via JSON or by a tool call (`automation.schedule`).

## Acceptance Criteria

1. A cron job ("`* * * * *`") fires exactly once per minute under nominal load.
2. A periodic job (every 15 s) fires within ±100 ms of the scheduled time.
3. A triggered job fires within 50 ms of the trigger event.
4. Per-agent lease prevents two concurrent runs of the same agent_key; the queued
   firing is held or dropped per policy.
5. A failing job is recorded with the failure reason; the next firing happens on
   schedule.
6. Cancelling a job mid-run respects the executor's cancellation semantics; the run
   is recorded as `aborted`.
7. `tests/automation/` ≥ 80% coverage.
8. `bench/automation/scheduler-tick` reports < 5 ms for 1 000 jobs.

## Design Doc Cross-References

- [`../design-docs/automation-runtime.md`](../design-docs/automation-runtime.md)
- [`../design-docs/agent-platform.md`](../design-docs/agent-platform.md)
  (cross-cutting concerns)
- [`../design-docs/async-model.md`](../design-docs/async-model.md)
- [`../design-docs/permissions-and-hooks.md`](../design-docs/permissions-and-hooks.md)

## Risks

- Cron-parser ambiguity (5-field vs. 6-field with seconds) — pick 5-field POSIX, add
  `seconds` as a separate optional config knob.
- Drift over long uptimes — use `oran-async`'s steady_timer with absolute next-fire
  time, not relative sleeps.

## Validation

```sh
xmake build oran-automation
xmake build test-automation && xmake run test-automation
xmake build bench-automation && xmake run bench-automation
```
