# Automation Cron Category

## Goal

Land the first cron-category planning and state boundaries for
`oran-automation` without starting a scheduler service. The plan starts with a
deterministic POSIX 5-field cron parser/evaluator that returns the next due
fire for a caller-owned clock and stored state, then adds durable repository
state for cron schedules and last-fired timestamps, then adds config-authored
cron schedule seeds mapped by bootstrap into repository descriptors.
Explicit seed persistence, one caller-awaited cron service cycle, durable cron
run history, cooperative finite-loop stop policy, typed cron run outcome
classification, and repository-backed cron execution leases for explicit loop
owners now exist; detached service-loop startup, queues, notifiers, and agent
firing stay in later scheduler slices.

## Scope

- In scope:
- Add a public cron schedule shape and evaluator in `oran-automation`.
- Support POSIX-style 5-field expressions (`minute hour day-of-month month
  day-of-week`) with `*`, lists, ranges, and steps.
- Persist cron jobs through `AutomationRepository` using an automation-owned
  `automation_cron_jobs` migration, with upsert/load/list/mark-fired APIs.
- Validate stored cron expressions through the evaluator, while keeping
  evaluation deterministic and caller-clocked.
- Parse typed `automation.cron.jobs[]` config seeds and map them in bootstrap
  without making `oran-automation` depend on `oran-config`.
- Let caller-owned automation runtimes explicitly apply mapped cron seeds into
  `automation.db` without making bootstrap own that state.
- Let caller-owned automation runtimes run one explicit cron service cycle that
  validates finite loop policy, applies seeds, and awaits the existing cron
  loop without making bootstrap own that state.
- Record success/failure cron run rows for explicit due-execution attempts,
  including failure reasons, without treating run history as queueing,
  notification, or agent execution ownership.
- Let caller-owned cron loops and runtime service cycles stop cooperatively
  between explicit execution iterations without cancelling an active handler or
  starting a detached service.
- Classify explicit cron handler run rows as `success`, `failure`, or
  `aborted`, including `ErrorKind::cancelled` handler results, without adding
  new hook events, queueing, or scheduler retry/drop policy.
- Lease explicit cron handler execution per stored cron job so caller-owned
  loops and service cycles do not overlap active due work for the same job,
  without adding queueing, notifier routing, or agent execution ownership.
- Keep the slice free of agent, detached timer, automatic bootstrap
  persistence, or background task ownership.
- Update automation docs/status/history/release notes in the same slice.
- Out of scope:
- Scheduler service startup that automatically reads config and applies/runs
  cron seeds.
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
- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `include/oran/bootstrap/automation_cron.hpp`
- `src/oran-bootstrap/automation_cron.cpp`
- `include/oran/bootstrap/runtime_assembly.hpp`
- `tests/automation/test_periodic.cpp`
- `tests/automation/test_repository.cpp`
- `tests/automation/test_runtime.cpp`
- `tests/config/test_config.cpp`
- `tests/bootstrap/test_memory_retention.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`
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
   succeeds. Slice 201 adds finite caller-owned loop policy over that execution
   surface, and slice 202 adds advisory cron lifecycle metadata around handler
   execution. Slice 203 adds typed cron config seeds and bootstrap mapping into
   repository upsert descriptors, slice 204 adds explicit caller-owned runtime
   application for those seeds, slice 205 adds one caller-awaited service cycle
   over seed apply plus finite cron loop execution, slice 206 records durable
   run history for due cron handler attempts, slice 207 adds cooperative stop
   policy for the finite cron loop and service-cycle handoff, and slice 208
  classifies explicit run history as `success`, `failure`, or `aborted` for
  cancelled handlers, and slice 209 adds repository-backed cron execution
  leases used by the finite cron loop and service-cycle handoff. Later: add
  detached service/timer ownership without bootstrap-owned background work.
4. **Triggered/notifier/queue policy.**
   Later: add triggered categories, queueing/backpressure, notifier routing,
   and agent execution leases.
5. **Scheduler performance.**
   Later: measure the 1 000-job scheduler tick criterion once the scheduler
   exists.

## Validation

- Commands:
- `xmake build test-config`
- `xmake run test-config`
- `xmake build test-bootstrap`
- `xmake run test-bootstrap`
- `xmake build test-automation`
- `build/linux/x86_64/release/test-automation "AutomationRuntime applies cron job seeds explicitly"`
- `build/linux/x86_64/release/test-automation "AutomationRuntime runs a caller-awaited cron service cycle"`
- `build/linux/x86_64/release/test-automation "AutomationRuntime validates cron service cycles before applying seeds"`
- `build/linux/x86_64/release/test-automation "AutomationRepository records and lists cron runs"`
- `build/linux/x86_64/release/test-automation "[unit][automation][repository][cron][lease]"`
- `build/linux/x86_64/release/test-automation "[unit][automation][service][cron][lease]"`
- `build/linux/x86_64/release/test-automation "[unit][automation][runtime][cron][loop][lease]"`
- `build/linux/x86_64/release/test-automation "CronService::execute_due records cancelled cron handlers as aborted"`
- `build/linux/x86_64/release/test-automation "CronService::execute_due advances only successful due cron jobs"`
- `build/linux/x86_64/release/test-automation "AutomationRuntime::open creates parent directories and migrates state"`
- `build/linux/x86_64/release/test-automation "AutomationRuntime::open reuses an already migrated automation database"`
- `build/linux/x86_64/release/test-automation "CronLoop::run honors stop requests before starting work"`
- `build/linux/x86_64/release/test-automation "CronLoop::run stops after a successful iteration when requested"`
- `build/linux/x86_64/release/test-automation "AutomationRuntime forwards cron service cycle stop requests"`
- `build/linux/x86_64/release/test-bootstrap "RuntimeAssembly cron seeds persist only through caller-owned automation runtime"`
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
- Confirm invalid cron service-cycle policy fails before seed rows are written.
- Confirm due cron handler attempts record success/failure run rows while
  not-due scans record no rows.
- Confirm failed cron handlers record the failure reason but keep stored cron
  state due for retry.
- Confirm cancelled cron handlers record an `aborted` run outcome but keep
  stored cron state due for retry.
- Confirm cooperative stop requests prevent new work before an iteration and
  prevent catch-up/sleep after a completed execution without cancelling an
  active handler.
- Confirm cron execution leases reject active conflicts before handler
  execution, release after durable success, and allow expired takeover.
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
- [x] 2026-06-07 22:57 +0800: Implemented `CronLoop::run(...)` plus the
  public finite cron loop request/result/stop-reason shapes. The loop repeatedly
  calls the explicit due-execution surface, catches up overdue fires one stored
  fire at a time, waits only within the caller's total budget, and stops on no
  due work, iteration limit, or handler failure without introducing process
  timers, queues, notifiers, hooks, or agent firing.
- [x] 2026-06-07 23:24 +0800: Implemented optional advisory cron lifecycle
  metadata through `CronServiceOptions::hooks`. Due execution now publishes
  `job_started` before the caller handler, `job_failed` after handler failure,
  and `job_finished` only after handler success plus durable cron state
  advancement, while keeping sink failures advisory and preserving caller-owned
  execution.
- [x] 2026-06-07 23:56 +0800: Implemented typed
  `automation.cron.jobs[]` config seeds plus `bootstrap::cron_jobs_from(...)`.
  Config owns JSON shape, UTC timestamps, and unique job keys; bootstrap
  validates expressions through `oran-automation` and stores
  `UpsertCronJobRequest` descriptors on `RuntimeAssembly`. Bootstrap still does
  not open `automation.db`, upsert cron rows, start timers, or execute jobs.
- [x] 2026-06-08 00:31 +0800: Implemented
  `AutomationRuntime::apply_cron_job_seeds(...)` as the explicit caller-owned
  persistence handoff for mapped cron descriptors. Runtime owners can upsert
  config-authored seeds into `automation.db` after they open automation state;
  bootstrap still only stores descriptors and does not apply them automatically.
- [x] 2026-06-08 00:50 +0800: Implemented
  `AutomationRuntime::run_cron_service_cycle(...)` as a caller-awaited startup
  cycle over seed application and the existing finite cron loop. The runtime
  validates service policy before seed writes, applies caller-supplied seeds,
  runs the supplied handler through `CronLoop::run(...)`, and still does not
  spawn timers, enqueue work, notify channels, call agents, or let bootstrap own
  automation state.
- [x] 2026-06-08 01:07 +0800: Implemented durable cron run history through
  `automation_cron_runs`, `AutomationRepository::record_cron_run(...)`, and
  `list_cron_runs(...)`. Explicit due execution now records success and failure
  rows, exposes the stored row on `CronExecuteAttempt::run`, leaves not-due
  scans without rows, and still advances cron state only after handler success.
- [x] 2026-06-08 01:22 +0800: Implemented cooperative cron loop stop policy
  through `CronLoopRunRequest::stop_requested` and
  `CronLoopRunStopReason::stop_requested`. `CronLoop::run(...)` checks the
  predicate before each iteration and after each execution, and
  `AutomationRuntime::run_cron_service_cycle(...)` forwards the same policy
  without starting timers or cancelling active handlers.
- [x] 2026-06-08 01:43 +0800: Implemented typed cron run outcomes through
  `CronRunOutcome` and `automation_cron_runs.outcome` migration v5.
  `CronService::execute_due(...)` records cancelled handler errors as
  `aborted`, ordinary errors as `failure`, and successful handlers as
  `success`, while failed and aborted jobs remain due for explicit retry.
- [x] 2026-06-08 02:06 +0800: Implemented repository-backed cron execution
  leases through `automation_cron_leases`,
  `AutomationRepository::acquire_cron_lease(...)`, and
  `release_cron_lease(...)`. `CronService::execute_due(...)` can explicitly
  lease due handlers, and `CronLoop::run(...)` /
  `AutomationRuntime::run_cron_service_cycle(...)` default to
  `automation-cron-loop` lease ownership. Active lease conflicts return
  `ErrorKind::conflict` before handler execution; expired leases can be taken
  over; no queueing, notifier routing, detached scheduler startup, or agent
  firing was added.
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
- 2026-06-07: Add finite cron loop policy above due execution before config or
  service-loop startup. A bounded caller-owned loop is enough to prove catch-up,
  wait-budget, and failure-stop semantics while preserving the plan's rule that
  bootstrap must not gain hidden automation ownership.
- 2026-06-07: Add cron lifecycle metadata before config/service startup. The
  explicit handler boundary is the first place with a real start/outcome signal,
  so hooks can observe cron work without introducing queues, timers, notifiers,
  or agent firing.
- 2026-06-07: Add config-authored cron schedule seeds before automatic
  persistence or service startup. The parser can own shape and timestamps while
  bootstrap validates expressions through automation; deferring upsert/timer
  ownership keeps configured cron jobs from becoming a hidden daemon side
  effect.
- 2026-06-08: Add explicit runtime seed application before service startup.
  Applying seeds through `AutomationRuntime` lets embedders persist authored
  cron rows after opening caller-owned state, while keeping bootstrap free of
  automatic `automation.db` creation and keeping timer ownership downstream.
- 2026-06-08: Add one explicit runtime service cycle before detached service
  startup. The cycle validates policy before writes, composes seed application
  with the existing finite cron loop, and gives embedders one startup handoff
  without making bootstrap or `oran-automation` own background timers.
- 2026-06-08: Record cron run history before queue/notifier/agent firing
  policy. The spec needs failure reasons to be durable, and this boundary can
  be shipped without introducing hidden scheduler ownership or broader retry
  semantics.
- 2026-06-08: Add cooperative stop policy before detached service startup.
  Runtime owners need a graceful way to end a finite cron service cycle between
  executions, while active handler interruption and retry/drop policy remain
  separate downstream work.
- 2026-06-08: Classify cron run outcomes before broader queue/notifier policy.
  Spec 0006 requires cancelled runs to be recorded as `aborted`, and the
  existing explicit handler boundary can do that without inventing process
  retry/drop semantics or adding scheduler ownership.
- 2026-06-08: Add cron execution leases before queue/notifier/agent firing
  policy. Explicit loops can now avoid overlapping due work for the same stored
  cron job, while process-wide detached scheduler ownership and agent-facing
  lease policy remain downstream.

## Linked Artifacts

- Related design doc: `docs/design-docs/automation-runtime.md`
- Related product spec: `docs/product-specs/0006-automation.md`
- PRs:
- History entry:
- `docs/histories/2026-06/20260607-1800-automation-cron-schedule.md`
- `docs/histories/2026-06/20260607-1831-automation-cron-persistence.md`
- `docs/histories/2026-06/20260607-2146-automation-cron-runtime-tick.md`
- `docs/histories/2026-06/20260607-2238-automation-cron-execute-due.md`
- `docs/histories/2026-06/20260607-2257-automation-cron-loop-run-policy.md`
- `docs/histories/2026-06/20260607-2324-automation-cron-lifecycle-hooks.md`
- `docs/histories/2026-06/20260607-2356-automation-cron-config.md`
- `docs/histories/2026-06/20260608-0031-automation-cron-seed-apply.md`
- `docs/histories/2026-06/20260608-0050-automation-cron-service-cycle.md`
- `docs/histories/2026-06/20260608-0107-automation-cron-run-history.md`
- `docs/histories/2026-06/20260608-0122-automation-cron-loop-stop-policy.md`
- `docs/histories/2026-06/20260608-0143-automation-cron-run-outcomes.md`
- `docs/histories/2026-06/20260608-0206-automation-cron-execution-leases.md`
- Release note:
- `docs/releases/feature-release-notes.md#2026-06`
