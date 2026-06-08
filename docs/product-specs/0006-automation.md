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
mapped descriptors. Slice 205 adds an explicit
`AutomationRuntime::run_cron_service_cycle(...)` helper that validates a finite
service-cycle policy, applies supplied cron seeds, and awaits the existing
finite cron loop in one caller-owned step. Slice 206 adds durable cron run
history for explicit due-execution attempts, and slice 207 adds cooperative
stop policy for the finite cron loop and service-cycle handoff. Slice 208
classifies explicit cron handler run history as `success`, `failure`, or
`aborted`, with cancelled handler errors stored as `aborted`. Slice 209 adds
repository-backed cron execution leases for explicit cron loop owners, and
slice 210 adds stored cron job `agent_key` plus repository-backed cron agent
leases for the same explicit execution owner. Slice 211 adds durable triggered
job descriptors plus caller-owned triggered intake that matches external trigger
keys to stored jobs without queueing or agent execution, and slice 212 adds
durable triggered run history plus explicit caller-supplied triggered handler
execution over those matched descriptors. Slice 213 adds advisory triggered
job lifecycle metadata around that same explicit execution owner, slice 214
adds triggered agent leases, slice 215 adds bounded triggered queue
backpressure, slice 216 adds one-at-a-time triggered queue draining, and slice
217 adds drop-on-conflict handling for queued triggered descriptors blocked by
active triggered-agent leases. Slice 218 adds non-blocking queue polling and
finite available-batch draining over the same explicit queue execution/drop
path. Slice 219 adds required prompt input to cron and triggered job
descriptors so future agent firing has durable work text. Slice 220 adds an
injected prompt-runner adapter that maps those stored prompts into existing
cron/triggered handler surfaces without making automation depend on bootstrap or
starting a background owner. The current API evaluates
periodic and cron schedules from caller-supplied state, maps a long-term memory
retention policy into a due-only `memory::longterm::DecayRequest`, persists the
configured retention job plus run history and lease state through
`AutomationRepository`,
persists cron job schedule/agent/prompt/last-fired state through `AutomationRepository`,
persists cron success/failure/aborted run history through `AutomationRepository`,
persists cron execution lease and cron agent lease state through
`AutomationRepository`,
persists triggered job descriptor prompt input and success/failure/aborted run
history state through `AutomationRepository`,
explicitly opens/migrates automation state through `AutomationRuntime::open(...)`,
lets a runtime owner scan stored cron jobs for due work without mutating them,
lets a runtime owner execute due cron jobs through a supplied handler and mark
only handler-successful fires complete while recording one run row per handler
attempt and optionally leasing both the due job and the stored job's agent key,
can adapt a caller-supplied prompt runner into that cron handler shape using
the stored `agent_prompt`,
lets a runtime owner run a finite explicit cron loop that can catch up due
fires, wait within a caller budget, or stop cooperatively between explicit
execution iterations while defaulting to repository-backed cron execution
leases,
publishes advisory cron job lifecycle metadata when the caller supplies a hook
bus,
parses and maps config-authored cron schedule seeds without starting a
scheduler,
lets a caller-owned automation runtime explicitly apply those cron seeds into
`automation.db`,
lets a caller-owned automation runtime run one explicit cron service cycle that
applies seeds and drives the finite cron loop with a supplied handler plus the
same cooperative stop policy,
lets a caller-owned runtime match a supplied external `trigger_key` against
stored triggered job descriptors through `TriggeredService::intake(...)`,
lets that same explicit triggered owner execute matched descriptors through a
caller-supplied handler while recording one triggered run row per attempt,
can adapt a caller-supplied prompt runner into that triggered handler shape
using the stored `agent_prompt`,
publishes advisory triggered job lifecycle metadata when the caller supplies a
hook bus,
lets a queue consumer drain and execute exactly one queued triggered descriptor
at a time through `TriggeredQueue::drain_once(...)`, or drain currently
available queued descriptors up to a caller-owned `max_jobs` limit through
`TriggeredQueue::drain_available(...)`,
lets a runtime owner tick one stored retention job against a supplied long-term
memory backend, publishes advisory retention metadata when the caller supplies a
hook bus, can wait once within a caller budget for the earliest stored cron fire,
and can wait once within a caller budget for a stored retention job to become
due while leasing due execution or run a finite caller-owned loop over that
step. Bootstrap maps configured-route `memory.longterm.retention` into a stored
`MemoryRetentionJob` descriptor whose first fire is after the one-shot startup
decay pass, and maps `automation.cron.jobs[]` into
`UpsertCronJobRequest` descriptors including `agent_key` and `agent_prompt`,
but bootstrap still does not open `automation.db`, apply those rows, or run a
background service.

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
  requires non-empty `agent_prompt`, validates the cron expressions through
  `evaluate_cron_schedule(...)`, and maps them into repository upsert requests.
  Bootstrap performs that mapping for loaded config even when no provider route
  is configured.
- `RuntimeAssembly::cron_jobs()` exposes those mapped cron seeds for diagnostics
  and future persistence ownership; it is not persisted or run by
  `RuntimeAssembly::build`.
- `AutomationRuntime::apply_cron_job_seeds(...)` is the explicit persistence
  handoff. It upserts mapped cron seed rows through the caller-owned runtime
  repository, returns requested/upserted counts plus stored rows, and annotates
  failures with `seed_index` / `job_key` context.
- `AutomationRuntime::run_cron_service_cycle(...)` validates explicit
  service-cycle policy before repository mutation, applies supplied cron seeds,
  and delegates to `CronLoop::run(...)`. It returns both seed-apply and loop
  summaries, while still leaving lifecycle ownership with the caller.
- `AutomationRepository` runs migrations over a caller-supplied `storage::Pool`,
  upserts and loads retention jobs by durable `job_key`, persists
  `last_fired_at`, records success/failure run rows, lists recent runs, acquires
  retention job leases when no active lease exists or an existing lease has
  expired, releases leases only for the matching owner, and upserts/loads/lists
  cron jobs with durable agent key, prompt, schedule, last-fired state, and
  success/failure/aborted run history. It also acquires/releases cron execution
  leases for stored cron jobs with the same active-conflict and expired-takeover
  semantics, upserts/loads/lists prompt-bearing triggered job descriptors by
  external `trigger_key`, and records/lists triggered run rows with durable
  `success` / `failure` / `aborted` outcomes.
- `AutomationRuntime::open(...)` validates an explicit database path, creates
  parent directories, opens `automation.db` through an owned `storage::Pool`,
  runs automation migrations, exposes the migration report and repository, can
  explicitly apply cron seed descriptors or run one cron service cycle, and can
  construct `CronService`, `CronLoop`, `TriggeredService`,
  `MemoryRetentionService`, or `MemoryRetentionLoop` over that stable state.
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
  `PeriodicJobState::last_fired_at` plus the stored agent key and prompt,
  return `std::nullopt` for missing reads, return `ErrorKind::not_found` for
  missing mark-fired mutations, validate cron expressions and non-empty prompts
  before SQLite writes, and list rows by newest update.
- `CronService::tick(...)` scans stored cron jobs up to a caller limit,
  evaluates each stored schedule, and returns checked count, due jobs, and the
  earliest next fire without advancing `last_fired_at`.
- `CronLoop::run_once(...)` ticks immediately, waits through
  `async::sleep_for(...)` only when the earliest next cron fire is within the
  caller's `max_wait`, reports cancellation while waiting, and re-ticks after
  the wait.
- `CronService::execute_due(...)` reuses that scan result, invokes a
  caller-supplied handler for each due cron job, optionally acquires a stored
  cron execution lease plus the stored job's cron agent lease before the handler,
  advances `last_fired_at` only after the handler succeeds, records
  success/failure/aborted cron run rows, records `ErrorKind::cancelled` handler
  errors as `aborted`, reports handler errors per attempt, releases held leases
  after durable outcome work, and leaves failed or aborted handler jobs due for
  retry by the next explicit call. Active job or agent lease conflicts return
  `ErrorKind::conflict` before the handler runs.
- `CronLoop::run(...)` repeatedly calls `execute_due(...)` up to a caller
  iteration limit, waits only within `max_total_wait`, aggregates
  attempted/advanced/failed counters, stops on `no_due_work`,
  `iteration_limit`, `handler_failure`, or a cooperative `stop_requested`
  predicate, defaults to `automation-cron-loop` lease ownership for due
  execution, and does not immediately retry failed handlers inside the same run.
- `CronServiceOptions::hooks` lets callers provide a `hook::Bus`, source label,
  fallback agent key, and identity. Due cron execution publishes advisory
  `job_started` before the handler, `job_failed` after a handler failure while
  keeping cron state unadvanced, and `job_finished` only after handler success
  plus durable `last_fired_at` advancement. Cron hook payloads use the stored
  cron job `agent_key`; advisory sink failures remain non-fatal.
- `TriggeredService::intake(...)` validates a caller-supplied external trigger
  key and positive match limit, then returns stored triggered job descriptors
  with the stored agent prompt and intake timestamp. It does not enqueue,
  record runs, notify channels, or call agents.
- `TriggeredService::execute_one(...)` executes exactly one caller-provided
  triggered descriptor, records one triggered run row, classifies cancelled
  handler errors as `aborted`, publishes the same advisory lifecycle metadata
  as the multi-match execution path when hooks are configured, and can opt into
  the existing triggered agent lease boundary.
- `TriggeredService::execute(...)` reuses triggered intake, invokes a
  caller-supplied handler for each matched descriptor by delegating to
  `execute_one(...)`, records one triggered run row per handler attempt,
  records `ErrorKind::cancelled` handler errors as `aborted`, records other
  handler errors as `failure`, and continues through other matched jobs without
  queueing, notifying channels, or calling agents.
  When `TriggeredExecuteRequest::lease_owner_key` is supplied, it acquires the
  matched job's stored `agent_key` in `automation_triggered_agent_leases` before
  handler work, returns `ErrorKind::conflict` before handler/run/hook work on an
  active same-agent lease, and releases the lease after the durable success or
  failure outcome. When constructed with `TriggeredServiceOptions::hooks`, it
  publishes advisory `job_started`, `job_failed`, and `job_finished` metadata
  around handler execution after the corresponding durable outcome boundary.
- `AutomationRuntime::triggered_service(...)` constructs that triggered owner
  over the caller-owned automation repository and can pass through hook options.
- `TriggeredQueue` is a caller-owned bounded in-process queue for matched
  triggered descriptors. `enqueue(...)` reuses triggered intake, writes matched
  jobs into bounded queue state, returns enqueued/dropped rows, applies
  `drop_newest` on overflow, and publishes advisory `job_dropped` metadata when
  callers supply a hook bus. Consumers may explicitly `receive()` queued jobs or
  call `drain_once(...)`, which receives one queued descriptor and executes
  exactly that descriptor through `TriggeredService::execute_one(...)`.
  `drain_once(...)` records the triggered run row and lifecycle hooks through
  the service execution path on success. When callers supply lease ownership and
  the stored triggered `agent_key` is already leased, the queue applies
  `drop_on_conflict`, returns dropped metadata with
  `reason=agent_lease_conflict`, publishes advisory `job_dropped`, and skips
  the handler plus run-row/lifecycle-hook writes. It still does not define
  notifier routing, agent firing, background loop ownership, or hold/requeue
  semantics. `try_receive()` polls one queued descriptor without awaiting, and
  `drain_available(...)` repeatedly consumes available descriptors up to
  `max_jobs`, stopping on queue empty, queue closed, or the limit. Batch
  draining reuses the exact one-descriptor execution/drop path and reports
  drained/completed/failed/dropped counters plus per-item drain results.
- `<oran/automation/prompt.hpp>` exposes `AutomationPromptRunRequest`,
  `AutomationPromptRunResult`, and `AutomationPromptRunner` plus cron/triggered
  handler factories. Runtime owners can inject any prompt runner and reuse the
  existing `CronService`, `TriggeredService`, or `TriggeredQueue` execution
  paths; automation still does not construct `AgentPromptRunner` itself.
- `AutomationRuntime::triggered_queue(...)` constructs that queue over the
  caller-owned automation repository and runtime executor.
- `test-async` reports 14 cases / 76 assertions for the bounded channel
  polling primitive consumed by triggered queues.
- `test-automation` reports 94 cases / 1574 assertions.
- `test-hook` reports 38 cases / 313 assertions for the hook payload surface.
- `test-config` reports 51 cases / 468 assertions for the consuming config
  boundary, and `test-bootstrap` reports 129 cases / 1095 assertions for mapped
  cron seeds.
- `bench-automation` compares periodic schedule evaluation with retention
  request planning over a 1024-job batch.

Still open: detached/background service-loop startup over `AutomationRuntime`,
process service/timer shutdown policy, notifier callbacks, agent firing, queue
hold/requeue semantics for blocked agent leases, and the scheduler tick
performance criterion. Triggered descriptor intake, explicit one-item queue
draining, finite available-batch draining, and drop-on-conflict handling for
active triggered-agent leases exist, but full
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
3. A triggered job fires within 50 ms of the trigger event. Current status:
   slice 220 can persist prompt-bearing triggered descriptors, match a trigger
   event key to stored jobs through caller-owned intake, run caller-supplied
   handlers while recording run history, publish advisory lifecycle metadata,
   optionally lease the matched stored `agent_key`, adapt stored prompts into
   injected prompt-runner calls, and enqueue matched jobs into bounded
   in-process queue state with drop-newest backpressure. Queue consumers can
   now drain and execute one queued descriptor at a time, finite-drain all
   currently available queued descriptors up to a caller limit, or explicitly
   drop a queued descriptor blocked by an active triggered-agent lease, while
   notifier routing and actual agent firing latency remain downstream.
4. Per-agent lease prevents two concurrent runs of the same agent_key; the queued
   firing is held or dropped per policy. Current status: slice 210 prevents
   overlapping explicit cron execution for the same stored `agent_key` through
   repository-backed cron agent leases, and slice 214 prevents overlapping
   explicit triggered handler execution for the same stored `agent_key` when
   callers opt into triggered lease ownership. Slice 215 adds drop-newest
   backpressure for a full triggered queue, slice 216 drains one queued
   descriptor at a time without consuming additional queued jobs, and slice 217
   drops a drained triggered descriptor on active same-agent lease conflicts
   with `job_dropped(reason=agent_lease_conflict)`. Slice 218 adds finite
   available-batch draining over the same drop path, slice 219 makes stored
   cron/triggered jobs carry the required prompt text future agent firing will
   use, and slice 220 adds injected prompt-runner handler factories over those
   prompts. Richer hold/requeue
   policy, notifier routing, and actual agent firing remain downstream.
5. A failing job is recorded with the failure reason; the next firing happens on
   schedule. Current status: slice 206 records explicit cron handler failures
   with the failure reason and leaves stored state due for retry, while slice
   212 records explicit triggered handler failures with their failure reason;
   broader scheduler retry/drop policy remains downstream.
6. Cancelling a job mid-run respects the executor's cancellation semantics; the run
   is recorded as `aborted`. Current status: slice 208 records explicit cron
   handler errors with `ErrorKind::cancelled` as `aborted` run rows while
   leaving cron state due for retry, and slice 212 records cancelled triggered
   handler errors as `aborted` run rows; broader scheduler cancellation
   semantics remain downstream.
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
