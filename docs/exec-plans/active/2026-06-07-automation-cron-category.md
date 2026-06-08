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
classification, repository-backed cron execution leases for explicit loop
owners, the first per-agent cron lease policy, triggered intake, triggered
execution/run history, triggered lifecycle hooks, triggered agent leases,
bounded caller-owned triggered queue/backpressure, one-at-a-time triggered
queue draining, finite available-batch draining, and drop-on-conflict handling
for drained descriptors blocked by triggered-agent leases now exist. Cron and
triggered descriptors also now carry required prompt input so future agent
firing has durable work text, slice 220 adapts those stored prompts into
injected cron/triggered handlers without making automation own
`AgentPromptRunner`, and slice 221 bridges that seam into bootstrap-owned
configured-route `AgentPromptRunner` execution without making bootstrap own
`automation.db`. Detached
service-loop startup, richer blocked-agent hold/requeue semantics, notifiers,
and agent firing stay in later scheduler slices.

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
- Carry a stored cron job `agent_key` and lease explicit cron handler execution
  per agent key so caller-owned loops and service cycles do not overlap active
  work for the same agent, without adding queueing, notifier routing, or agent
  execution ownership.
- Persist triggered job descriptors and match caller-supplied external
  `trigger_key` values through an explicit intake service without adding
  queueing, notifier routing, or agent execution ownership.
- Execute matched triggered descriptors through a caller-supplied handler and
  record success/failure/aborted triggered run rows without adding queueing,
  notifier routing, leases, or agent execution ownership.
- Publish advisory triggered lifecycle metadata around explicit triggered
  handler execution when callers supply a hook bus, without adding queueing,
  notifier routing, leases, or agent execution ownership.
- Lease explicit triggered handler execution per stored triggered job
  `agent_key` so caller-owned execution does not overlap active work for the
  same agent, without adding queueing, notifier routing, or agent execution
  ownership.
- Buffer matched triggered descriptors in a bounded caller-owned queue with
  explicit receive/drain by callers and advisory drop metadata on overflow,
  without adding notifier routing, blocked-agent hold/requeue policy, or agent
  execution ownership.
- Drop a drained queued triggered descriptor explicitly on active
  triggered-agent lease conflicts, returning dropped metadata and advisory
  `job_dropped` while suppressing handler/run-row side effects, without adding
  notifier routing, hold/requeue policy, or agent execution ownership.
- Poll and finite-drain currently available triggered queue descriptors up to a
  caller-owned `max_jobs` limit, reusing the same single-descriptor
  execution/drop path without adding notifier routing, hold/requeue policy,
  detached queue ownership, or agent execution ownership.
- Require prompt text on config-authored cron seeds plus stored cron and
  triggered job descriptors so agent-firing work has durable input, without
  wiring `AgentPromptRunner` or notifier routing yet.
- Adapt stored cron and triggered prompts into injected caller-owned prompt
  execution through the existing handler surfaces, without moving provider,
  CLI, notifier, or detached service ownership into `oran-automation`.
- Bridge that injected automation prompt-runner seam into bootstrap's
  configured-route `AgentPromptRunner` one durable job at a time, without
  making bootstrap open `automation.db` or own a background scheduler.
- Keep the slice free of agent, detached timer, automatic bootstrap
  persistence, or background task ownership.
- Update automation docs/status/history/release notes in the same slice.
- Out of scope:
- Scheduler service startup that automatically reads config and applies/runs
  cron seeds.
- Process timers, blocked-agent queue hold/requeue policy,
  notifier routing, and agent firing.
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
- `include/oran/automation/queue.hpp`
- Constraints:
- Keep `oran-automation` independent of `oran-config`, `oran-agent`, and
  bootstrap scheduling ownership.
- Public headers must stay lightweight and third-party-free.
- Cron evaluation must not skip/coalesce missed firings; later service policy
  decides catch-up/drop behavior.
- Compile-budget impact (if any):
- Implementation mostly stays in existing service/runtime translation units.
  Slice 215 adds one small `queue.cpp` translation unit over the existing
  `async::Channel` primitive; no new third-party dependency is added.

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
   cancelled handlers, slice 209 adds repository-backed cron execution leases
   used by the finite cron loop and service-cycle handoff, and slice 210 adds
   stored cron job `agent_key` plus repository-backed cron agent leases for the
   same explicit execution owner. Later: add detached service/timer ownership
   without bootstrap-owned background work.
4. **Triggered/notifier/queue policy.**
   In progress: slice 211 adds durable triggered job descriptors and
   caller-owned `TriggeredService::intake(...)` matching by external
   `trigger_key`, slice 212 adds durable triggered run history plus
   caller-supplied handler execution over those matches, and slice 213 adds
   advisory triggered lifecycle metadata around those handler attempts. Slice
   214 adds repository-backed triggered agent leases for explicit handler
   owners. Slice 215 adds bounded caller-owned triggered queue/backpressure with
   advisory drop metadata. Slice 216 adds explicit one-at-a-time queue draining
   through `TriggeredQueue::drain_once(...)` and
   `TriggeredService::execute_one(...)`. Slice 217 adds `drop_on_conflict`
   handling for drained triggered descriptors blocked by active triggered-agent
   leases. Slice 218 adds non-blocking queue polling and finite
   available-batch draining through `TriggeredQueue::try_receive()` and
   `drain_available(...)`. Slice 219 adds required `agent_prompt` to cron config
   seeds plus stored cron/triggered descriptors so the future agent-firing owner
   has durable prompt input, slice 220 adds
   `make_cron_prompt_handler(...)` / `make_triggered_prompt_handler(...)` so
   callers can inject prompt execution through the same durable service/queue
   paths, slice 221 adds bootstrap-owned
   `make_automation_agent_prompt_runner(...)` wiring into configured-route
   `AgentPromptRunner` while keeping `automation.db` and service ownership
   caller-owned, slice 222 adds caller-owned notifier callbacks plus
   output-carrying cron/triggered attempt results after durable outcomes, slice
   223 adds the first caller-owned composed automation service owner over
   buffered triggered work plus one cron cycle, slice 224 adds caller-owned
   blocked-agent hold/retry on top of that owner, and slice 225 adds the
   finite caller-owned service-loop policy above that owner. Later: add
   concrete notifier routing, long-running startup/shutdown ownership above the
   finite loop, and agent firing.
5. **Scheduler performance.**
   Later: measure the 1 000-job scheduler tick criterion once the scheduler
   exists.

## Validation

- Commands:
- `xmake build test-config`
- `xmake run test-config`
- `xmake build test-bootstrap`
- `xmake run test-bootstrap`
- `xmake build test-async`
- `xmake run test-async`
- `xmake build test-automation`
- `build/linux/x86_64/release/test-automation "AutomationRuntime applies cron job seeds explicitly"`
- `build/linux/x86_64/release/test-automation "AutomationRuntime runs a caller-awaited cron service cycle"`
- `build/linux/x86_64/release/test-automation "AutomationRuntime validates cron service cycles before applying seeds"`
- `build/linux/x86_64/release/test-automation "AutomationRepository records and lists cron runs"`
- `build/linux/x86_64/release/test-automation "[unit][automation][repository][cron][lease]"`
- `build/linux/x86_64/release/test-automation "[unit][automation][service][cron][lease]"`
- `build/linux/x86_64/release/test-automation "[unit][automation][runtime][cron][loop][lease]"`
- `build/linux/x86_64/release/test-automation "AutomationRepository acquires, expires, and releases cron agent leases"`
- `build/linux/x86_64/release/test-automation "AutomationRepository round-trips triggered jobs by trigger key"`
- `build/linux/x86_64/release/test-automation "AutomationRepository records and lists triggered runs"`
- `build/linux/x86_64/release/test-automation "TriggeredService::intake matches stored jobs for a trigger key"`
- `build/linux/x86_64/release/test-automation "TriggeredService::execute records explicit triggered handler attempts"`
- `build/linux/x86_64/release/test-automation "TriggeredService::execute records cancelled triggered handlers as aborted"`
- `build/linux/x86_64/release/test-automation "TriggeredService::execute publishes lifecycle metadata for handler success"`
- `build/linux/x86_64/release/test-automation "TriggeredService::execute publishes lifecycle metadata for handler failure"`
- `build/linux/x86_64/release/test-automation "[unit][automation][repository][triggered][lease]"`
- `build/linux/x86_64/release/test-automation "TriggeredService::execute leases triggered handlers and releases after outcomes"`
- `build/linux/x86_64/release/test-automation "TriggeredService::execute blocks active triggered agent leases before handlers"`
- `build/linux/x86_64/release/test-automation "AutomationRuntime constructs triggered service execution over owned state"`
- `build/linux/x86_64/release/test-automation "TriggeredQueue enqueues matched triggered jobs for explicit receive"`
- `build/linux/x86_64/release/test-automation "TriggeredQueue drops newest overflow and publishes job_dropped metadata"`
- `build/linux/x86_64/release/test-automation "TriggeredService::execute_one records one explicit triggered descriptor"`
- `build/linux/x86_64/release/test-automation "TriggeredQueue drains one queued descriptor through the triggered service"`
- `build/linux/x86_64/release/test-automation "TriggeredQueue drops drained descriptors on active triggered agent lease conflicts"`
- `build/linux/x86_64/release/test-automation "TriggeredQueue drains available queued descriptors without waiting"`
- `build/linux/x86_64/release/test-automation "TriggeredQueue drain_available counts handler failures and lease-conflict drops"`
- `build/linux/x86_64/release/test-automation "TriggeredQueue rejects invalid enqueue policy"`
- `build/linux/x86_64/release/test-automation "Cron prompt handler runs stored cron job prompt"`
- `build/linux/x86_64/release/test-automation "Triggered prompt handler runs stored triggered job prompt"`
- `build/linux/x86_64/release/test-automation "CronService::execute_due notifies after durable success with handler output"`
- `build/linux/x86_64/release/test-automation "TriggeredService::execute reports notifier failures without failing durable success"`
- `build/linux/x86_64/release/test-async "Channel try_receive reports empty without waiting and drains FIFO values"`
- `build/linux/x86_64/release/test-async "Channel try_receive drains buffered values before reporting closed"`
- `build/linux/x86_64/release/test-async "Channel try_receive completes a pending sender without awaiting"`
- `build/linux/x86_64/release/test-automation "CronService::execute_due blocks active cron agent leases before handlers"`
- `build/linux/x86_64/release/test-automation "CronLoop::run uses cron agent leases before calling handlers"`
- `build/linux/x86_64/release/test-automation "CronService::execute_due records cancelled cron handlers as aborted"`
- `build/linux/x86_64/release/test-automation "CronService::execute_due advances only successful due cron jobs"`
- `build/linux/x86_64/release/test-automation "AutomationRuntime::open creates parent directories and migrates state"`
- `build/linux/x86_64/release/test-automation "AutomationRuntime::open reuses an already migrated automation database"`
- `build/linux/x86_64/release/test-automation "CronLoop::run honors stop requests before starting work"`
- `build/linux/x86_64/release/test-automation "CronLoop::run stops after a successful iteration when requested"`
- `build/linux/x86_64/release/test-automation "AutomationRuntime forwards cron service cycle stop requests"`
- `build/linux/x86_64/release/test-automation "AutomationRuntime creates a caller-owned automation service cycle over triggered and cron work"`
- `build/linux/x86_64/release/test-automation "AutomationService validates one-cycle policy before draining or applying seeds"`
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
- Confirm cron agent leases reject active same-agent conflicts before handler
  execution, release after durable success/failure paths, and allow expired
  takeover.
- Confirm triggered intake matches only stored descriptors for the supplied
  external trigger key and does not enqueue work, notify channels, or call
  agents.
- Confirm triggered execution records one success/failure/aborted run row per
  matched descriptor through a caller-supplied handler and still does not
  enqueue work, notify channels, or call agents.
- Confirm triggered execution publishes advisory lifecycle metadata only when a
  caller supplies hook options, and sink failures remain non-vetoing.
- Confirm triggered execution can acquire/release triggered agent leases when
  callers opt in, rejects active same-agent conflicts before handler/run/hook
  work, and releases after durable success/failure outcomes.
- Confirm triggered queueing reuses triggered intake, stores matched descriptors
  in bounded caller-owned state, lets callers explicitly receive queued jobs,
  and records no triggered run rows for queued or dropped work.
- Confirm full triggered queues apply `drop_newest`, return dropped metadata,
  publish advisory `job_dropped` only when callers supply hooks, and reject
  invalid queue policy such as zero capacity.
- Confirm triggered queue draining receives and executes exactly one queued
  descriptor, records only that descriptor's triggered run row, leaves later
  queued descriptors buffered, and rejects empty drain handlers.
- Confirm triggered queue draining can drop exactly the consumed descriptor on
  active triggered-agent lease conflicts, publishes advisory
  `job_dropped(reason=agent_lease_conflict)`, runs no handler, and records no
  triggered run row for the dropped descriptor.
- Confirm triggered queue available-batch draining consumes only descriptors
  returned by `try_receive()`, stops on empty/closed/`max_jobs`, shares handler
  state across drained items, and reports completed/failed/dropped counters.
- Confirm cron notifier callbacks run only after the durable cron run row and
  successful `last_fired_at` advancement are visible, preserve handler output
  text, and do not roll state back on notifier failure.
- Confirm triggered notifier callbacks run only after the durable triggered run
  row is visible, preserve handler output text, and surface notifier failure as
  advisory attempt metadata without changing the durable run outcome.
- Confirm no new dependency direction crosses from automation into bootstrap,
  config, or agent.
- Observability checks:
- `job_dropped` is advisory metadata-only and carries no trigger body, channel
  payload, prompt bytes, or agent input.
- Bench comparison (if perf-relevant):
- Triggered queue/backpressure is not perf-relevant; correctness coverage is the
  right gate until a scheduler tick loop exists.

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
- [x] 2026-06-08 02:36 +0800: Implemented stored cron `agent_key` plus
  repository-backed cron agent leases through migration v7. Config-authored
  cron seeds default missing `agent_key` to `automation`; bootstrap maps it into
  `UpsertCronJobRequest`; `CronService::execute_due(...)` acquires both the
  per-job and per-agent lease when lease ownership is enabled, releases both
  after durable outcome work, and returns `ErrorKind::conflict` before handler
  execution when another owner holds the same agent key. Hook payloads now use
  the stored cron job agent key.
- [x] 2026-06-08 02:58 +0800: Implemented triggered job descriptor intake
  through migration v8, repository upsert/load/list-by-trigger APIs,
  `TriggeredService::intake(...)`, and `AutomationRuntime::triggered_service()`.
  Runtime owners can match external `trigger_key` values to stored jobs without
  queueing work, notifying channels, calling agents, or making bootstrap own
  automation state.
- [x] 2026-06-08 03:30 +0800: Implemented explicit triggered handler
  execution plus durable triggered run history through migration v9,
  repository record/list APIs, and `TriggeredService::execute(...)`. Runtime
  owners can invoke a supplied handler once per matched descriptor and persist
  `success`, `failure`, or `aborted` attempt rows without queueing work,
  notifying channels, acquiring triggered leases, calling agents, or making
  bootstrap own automation state.
- [x] 2026-06-08 03:51 +0800: Implemented advisory triggered lifecycle hooks
  around explicit handler execution through `TriggeredServiceOptions::hooks`,
  `AutomationRuntime::triggered_service(...)` option pass-through, and
  metadata-only `job_started` / `job_failed` / `job_finished` publishing.
  Runtime owners can observe triggered handler attempts without queueing work,
  notifying channels, acquiring triggered leases, calling agents, or making
  bootstrap own automation state.
- [x] 2026-06-08 09:59 +0800: Implemented repository-backed triggered agent
  leases through migration v10,
  `AutomationRepository::acquire_triggered_agent_lease(...)`, and
  `release_triggered_agent_lease(...)`. `TriggeredService::execute(...)` can
  opt into same-agent lease ownership with `lease_owner_key` / `lease_ttl`,
  returns `ErrorKind::conflict` before handler/run/hook work on active
  conflicts, and releases after durable success/failure/aborted outcomes
  without adding queueing, notifier routing, agent calls, or bootstrap-owned
  automation state.
- [x] 2026-06-08 11:23 +0800: Implemented bounded caller-owned triggered
  queue/backpressure through `TriggeredQueue`, `AutomationRuntime::triggered_queue(...)`,
  and advisory `job_dropped` metadata. Runtime owners can buffer matched
  triggered descriptors, explicitly `receive()` queued jobs, and observe
  drop-newest overflow without executing handlers, recording triggered run rows,
  notifying channels, calling agents, or starting detached work.
- [x] 2026-06-08 12:15 +0800: Implemented explicit one-at-a-time triggered
  queue draining through `TriggeredService::execute_one(...)` and
  `TriggeredQueue::drain_once(...)`. Runtime owners can consume and execute
  exactly one queued descriptor without re-intaking by trigger key, while later
  queued work remains buffered and no notifier/agent/background-loop ownership
  is added.
- [x] 2026-06-08 12:47 +0800: Implemented triggered queue
  `drop_on_conflict` handling for active triggered-agent leases. Draining can
  consume exactly the blocked descriptor, return
  `TriggeredDroppedJob(reason=agent_lease_conflict)`, publish advisory
  `job_dropped`, and suppress handler/run-row/lifecycle-hook side effects
  without defining richer hold/requeue policy.
- [x] 2026-06-08 13:13 +0800: Implemented non-blocking triggered queue polling
  and finite available-batch draining through `async::Channel<T>::try_receive`,
  `TriggeredQueue::try_receive()`, and `TriggeredQueue::drain_available(...)`.
  Runtime owners can drain up to `max_jobs`, stop on empty/closed queue or the
  limit, and receive completed/failed/dropped counters over the same
  single-descriptor execution/drop path, without notifier routing, agent calls,
  detached queue ownership, or hold/requeue semantics.
- [x] 2026-06-08 14:01 +0800: Implemented required automation job prompts for
  stored cron and triggered descriptors. `automation.cron.jobs[]` now requires
  non-empty `agent_prompt`, bootstrap maps it into `UpsertCronJobRequest`, and
  `AutomationRepository` validates plus round-trips prompt text for cron and
  triggered rows. Because the automation schema is still pre-v1/pre-generation,
  the base cron/triggered migrations were updated in place instead of adding a
  compatibility migration. This intentionally stops before notifier routing,
  `AgentPromptRunner` wiring, detached loops, or agent invocation.
- [x] 2026-06-08 14:40 +0800: Implemented injected automation prompt-runner
  handler adapters through `AutomationPromptRunRequest`,
  `AutomationPromptRunner`, `make_cron_prompt_handler(...)`, and
  `make_triggered_prompt_handler(...)`. Stored cron and triggered descriptors
  can now drive caller-supplied prompt execution without moving bootstrap,
  provider, CLI, or detached service ownership into `oran-automation`.
- [x] 2026-06-09 01:39 +0800: Implemented bootstrap-owned
  `make_automation_agent_prompt_runner(...)` as the configured-route bridge
  from automation prompt runs into `AgentPromptRunner`. Durable automation jobs
  now reuse stable per-job session identity and configured-agent overlays while
  keeping automation state ownership caller-owned and noninteractive asks
  fail-closed by default.
- [x] 2026-06-09 02:37 +0800: Implemented caller-owned notifier callbacks plus
  output-carrying automation attempt results. Cron and triggered execution now
  preserve optional handler text through `AutomationJobHandlerResult`, publish
  one post-outcome callback only after durable run/state transitions, and keep
  notifier failures advisory on the attempt result without rolling durable
  outcomes back.
- [x] 2026-06-09 04:03 +0800: Implemented the first caller-owned composed
  automation service owner through `AutomationRuntime::automation_service(...)`
  and `AutomationService::run_cycle(...)`. One owner can now keep a bounded
  triggered queue beside stable runtime state, validate a full triggered-plus-
  cron cycle request before side effects, drain buffered triggered work first,
  then apply cron seeds and await the existing finite cron cycle. This sets the
  correct ownership locus for downstream blocked-agent hold/requeue without
  making bootstrap own `automation.db` or hiding detached background work.
- [x] 2026-06-09 05:17 +0800: Implemented caller-owned blocked-agent
  hold/retry on top of `AutomationService`. `requeue_on_conflict` now parks
  same-agent triggered lease conflicts on the service owner for later cycles,
  retries held descriptors before newer queued work, keeps public
  `TriggeredQueue::drain_*` semantics drop-only, and records retried triggered
  lease/finish timing at the actual attempt time while preserving the original
  fired timestamp.
- [x] 2026-06-09 06:48 +0800: Implemented the finite caller-owned loop policy
  above `AutomationService`. `AutomationService::run(...)` now repeats
  explicit service cycles over caller-owned iteration and retry-wait budgets,
  aggregates triggered and cron counters across cycles, sleeps only when held
  blocked triggered work remains, and stops with explicit
  `iteration_limit` / `no_due_work` / `handler_failure` / `stop_requested` /
  `held_jobs_remaining` reasons.
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
- 2026-06-08: Add the first cron agent-key lease policy before queue/notifier
  and agent firing policy. Explicit loops can now avoid overlapping due work for
  the same stored agent key, while queue hold/drop semantics and actual agent
  invocation remain downstream.
- 2026-06-08: Add triggered descriptor intake before queue/notifier/agent
  firing policy. Matching external trigger keys to stored jobs is the smallest
  useful triggered-category boundary, while trigger event durability, hold/drop
  behavior, notifier routing, and actual agent invocation remain downstream.
- 2026-06-08: Add triggered run history and caller-supplied execution before
  queue/notifier/agent firing policy. Durable per-attempt outcome rows make
  triggered handler behavior inspectable now, while hold/drop semantics,
  lifecycle hooks, leases, notifier routing, and actual agent invocation remain
  downstream.
- 2026-06-08: Add triggered lifecycle hooks before queue/notifier/agent firing
  policy. The explicit triggered handler boundary now has durable outcome rows,
  so advisory start/outcome metadata can be published without changing retry,
  hold/drop, lease, notifier, or agent invocation semantics.
- 2026-06-08: Add triggered agent leases before queue/notifier/agent firing
  policy. Explicit triggered handlers can now avoid overlapping same-agent work
  for stored descriptors, while queue hold/drop semantics, notifier routing,
  and actual agent invocation remain downstream.
- 2026-06-08: Add bounded triggered queue/backpressure before notifier and
  agent firing policy. The runtime now has a caller-owned place to hold matched
  triggered descriptors and report full-queue drops, while queue drain
  ownership, blocked-agent hold/requeue policy, notifier routing, and actual agent
  invocation remain downstream.
- 2026-06-08: Add explicit triggered queue drain-one before notifier and
  agent-firing policy. `TriggeredService::execute_one(...)` avoids re-intaking
  by trigger key, and `TriggeredQueue::drain_once(...)` consumes and executes
  exactly one queued descriptor while later queued work remains buffered.
- 2026-06-08: Add triggered queue drop-on-conflict before notifier and
  agent-firing policy. `TriggeredQueue::drain_once(...)` can now use triggered
  agent leases and explicitly drop the consumed descriptor with
  `agent_lease_conflict` metadata when another owner holds the same agent key,
  while richer hold/requeue semantics remain downstream.
- 2026-06-08: Add finite available-batch draining before notifier and
  agent-firing policy. The next service owner needs a non-blocking queue drain
  primitive, but a detached loop would prematurely own shutdown, wakeup, and
  notifier/agent dispatch policy. `drain_available(...)` therefore stays a
  caller-awaited finite batch over the existing one-descriptor execution/drop
  path.
- 2026-06-08: Add required job prompts before notifier and agent-firing policy.
  Stored job descriptors only named the target agent, so the next agent-firing
  slice had no durable prompt to pass to `cli::PromptRunRequest::prompt`.
  Because no generated automation schema exists yet, changing the base
  migrations directly is simpler than carrying a compatibility migration for a
  pre-v1 shape.
- 2026-06-09: Add caller-owned notifier callbacks before concrete delivery
  routing. The durable cron/triggered execution surfaces now carry enough
  outcome and output information to publish one post-outcome callback without
  making automation own cli/channel/desktop routing, bootstrap own
  `automation.db`, or any runtime own a detached scheduler.
- 2026-06-09: Add a caller-owned composed service owner before richer
  blocked-agent hold/requeue. Queue-level drain APIs intentionally stop at
  single-descriptor or currently-available batch execution; the next ownership
  locus for retry/parking policy should sit above buffered triggered work plus
  one cron cycle, not inside bootstrap and not hidden behind detached startup.
- 2026-06-09: Keep blocked-agent hold/retry on `AutomationService`, not on
  `TriggeredQueue`. Queue drains already document an execute-or-drop boundary
  over the descriptors they hold; retry policy belongs to the composed owner
  that can revisit held triggered work across later explicit cycles while
  preserving bounded queue semantics and caller-owned scheduler control.
- 2026-06-09: Add the finite caller-owned loop policy above `AutomationService`
  before any detached/background startup. The service owner already had the
  right state for held triggered retries, so the next boundary needed to be a
  bounded explicit run policy that callers can await, not a hidden daemon or a
  broader queue API.

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
- `docs/histories/2026-06/20260608-0236-automation-cron-agent-leases.md`
- `docs/histories/2026-06/20260608-0258-automation-triggered-intake.md`
- `docs/histories/2026-06/20260608-0330-automation-triggered-execution.md`
- `docs/histories/2026-06/20260608-0351-automation-triggered-lifecycle-hooks.md`
- `docs/histories/2026-06/20260608-0959-automation-triggered-agent-leases.md`
- `docs/histories/2026-06/20260608-1123-automation-triggered-queue.md`
- `docs/histories/2026-06/20260608-1215-automation-triggered-queue-drain.md`
- `docs/histories/2026-06/20260608-1247-automation-triggered-queue-lease-drop.md`
- `docs/histories/2026-06/20260608-1313-automation-triggered-queue-drain-available.md`
- `docs/histories/2026-06/20260608-1401-automation-agent-prompts.md`
- `docs/histories/2026-06/20260608-1440-automation-prompt-handlers.md`
- `docs/histories/2026-06/20260609-0139-automation-agent-prompt-bridge.md`
- `docs/histories/2026-06/20260609-0237-automation-notifier-callbacks.md`
- `docs/histories/2026-06/20260609-0329-automation-service-owner.md`
- Release note:
- `docs/releases/feature-release-notes.md#2026-06`
