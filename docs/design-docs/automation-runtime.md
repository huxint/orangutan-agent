# Automation Runtime

`oran-automation` owns automation scheduling decisions. The shipped surface is
still intentionally narrow: deterministic periodic schedule and POSIX cron
evaluation, long-term memory retention request planning, a bootstrap-owned
mapping from configured retention policy into that job descriptor,
automation-owned retention job/run/lease persistence, durable cron job state,
config-authored cron schedule seeds mapped by bootstrap into stored descriptors,
a caller-owned runtime state handle, explicit cron seed application, a
caller-awaited cron service cycle over the existing finite cron loop, a
caller-driven cron scan/wait/execute-due boundary plus finite caller-owned cron
loop policy, advisory cron lifecycle metadata, typed cron run outcome
classification, repository-backed cron execution leases and cron agent leases
for explicit loop owners, durable triggered job descriptors with caller-driven
triggered intake plus explicit triggered handler execution and durable
triggered run history, advisory triggered lifecycle hook metadata,
repository-backed triggered agent leases, bounded caller-owned triggered queue
backpressure with advisory drop metadata, explicit one-at-a-time triggered queue
draining, and a caller-driven retention service
tick with optional
advisory `memory_decay` plus job lifecycle metadata,
plus a caller-started retention loop step that can wait within a caller budget
for one stored job to become due and lease due execution, plus a finite
caller-owned loop policy over that step. It does not start detached background
work, own a long-running process service loop, persist configured cron seeds
from bootstrap automatically, run a detached queue-drain loop, notify channels, or call an
agent loop.

## Current Status

Slice 187 adds the `oran-automation` library with:

- `<oran/automation.hpp>` as the public umbrella header.
- `<oran/automation/periodic.hpp>` for periodic cadence and memory-retention
  planning.
- `src/oran-automation/periodic.cpp` as the implementation.
- `test-automation` and `bench-automation` target parity.

Slice 189 adds `<oran/automation/repository.hpp>`,
`src/oran-automation/repository.cpp`, and the built-in migration
`src/oran-automation/migrations/automation/0001-automation-retention-state.sql`.
`AutomationRepository` runs over a caller-supplied `storage::Pool`, applies the
`automation.db` schema when `migrate()` is called, and persists retention jobs,
last-fired state, and run rows keyed by `job_key`.

The library now depends on `oran-core`, `oran-async`, `oran-storage`,
`oran-memory`, and `oran-hook`. That dependency direction is intentional:
automation may plan work against memory's public
`memory::longterm::DecayRequest`, may use the generic storage pool/migration
primitives for its own schema, and may publish shared advisory hook payloads
when a caller explicitly supplies a hook bus.
`oran-memory`, `oran-storage`, and `oran-hook` stay independent of automation
and never schedule retention themselves.

Slice 188 consumes that boundary from bootstrap without turning bootstrap into a
scheduler. `bootstrap::longterm_memory_retention_job_from(...)` maps
`config::Config::memory().longterm.retention` into
`automation::MemoryRetentionJob`, and `RuntimeAssemblyOptions` can carry that
descriptor as `longterm_memory_retention_job`. Configured-route startup stores a
job whose `first_fire_at` is the startup decay clock plus
`decay_check_interval`, so the future periodic owner can start from the next
candidate fire after the one-shot startup pass. `RuntimeAssembly::build` stores
and exposes the descriptor but does not evaluate, persist, lease, or execute it.

Slice 189 persists that future owner's state without wiring bootstrap to open
`automation.db`. The repository can upsert and load the stored retention job,
mark `last_fired_at`, record successful or failed run rows, and list recent runs
in newest-first order. It still does not own a service loop, memory backend,
hook bus, queue, or cancellation policy for active jobs.

Slice 190 adds that first explicit execution owner without making it a process
loop. `<oran/automation/service.hpp>` exports `MemoryRetentionService`, which
borrows an `AutomationRepository` and a caller-supplied
`memory::longterm::Backend`. `tick(...)` loads one stored job by `job_key`,
reuses `plan_memory_retention(...)`, skips not-due work without mutation, calls
`Backend::decay(...)` only when due, records the run outcome, and advances
`last_fired_at` only after a successful backend run and run-row insert. Backend
failures are persisted as failed runs and returned to the caller with
`last_fired_at` unchanged so retry policy remains explicit.

Slice 191 turns the explicit tick into the first periodic retention hook
producer without turning it into a service loop. `MemoryRetentionServiceOptions`
accepts optional hook settings: a non-owning `hook::Bus*`, source label, agent
key, and identity. When a due tick succeeds and durable state has advanced, the
service publishes advisory `memory_decay` metadata through that bus. With no
bus, not-due ticks, or backend failures, no hook is published. Advisory sink
failures stay advisory: the tick still succeeds, and
`MemoryRetentionTickResult::hook_publish` reports sink/failure counts. The
service still does not open `automation.db`, own a timer, start a hidden
bootstrap loop, acquire leases, or call agents.

Slice 192 adds the explicit state handle for runtime owners that are ready to
open automation persistence without starting the service loop. `<oran/automation/runtime.hpp>`
exports `AutomationRuntimeOptions` and move-only `AutomationRuntime`.
`AutomationRuntime::open(...)` validates the database path, creates parent
directories, opens a `storage::Pool`, runs the automation migration through the
owned repository, stores the migration report, exposes that repository, and can
construct cron/retention services and loops over the same stable state. It
still does not start timers, acquire leases, publish job lifecycle hooks
itself, wire bootstrap to open `automation.db`, or call agents.

Slice 193 adds the first caller-started loop step without turning automation
into a daemon. `<oran/automation/loop.hpp>` exports
`MemoryRetentionLoopRunOnceRequest`, `MemoryRetentionLoopRunOnceResult`, and
`MemoryRetentionLoop`. `AutomationRuntime::memory_retention_loop(...)`
constructs the step owner from the runtime executor and the existing retention
service. `run_once(...)` ticks immediately, returns a not-due result when the
next fire is outside `max_wait`, sleeps with `async::sleep_for(...)` when the
next fire is within the caller's budget, and then ticks again at the scheduled
fire. Parent cancellation while waiting returns `ErrorKind::cancelled`, and a
negative `max_wait` returns `ErrorKind::invalid_argument`. It is still a single
explicit awaitable, not a detached background loop.

Slice 194 adds advisory job lifecycle metadata from the same explicit tick
owner. A due `MemoryRetentionService::tick(...)` publishes `job_started` before
calling the supplied backend, publishes `job_failed` after a backend failure has
been recorded as a failed run, and publishes `job_finished` after a successful
run row plus `last_fired_at` advancement. The `JobLifecyclePayload` carries the
configured identity/source, durable `job_key`, stable `job_type`, scope,
scheduled/start/finish timing, success, shadow count on success, and error
kind/message on backend failure. Not-due ticks publish no lifecycle events, and
sink failures remain advisory so lifecycle observers cannot veto or fail the
retention tick. Repository failures before a durable outcome still return to
the caller without inventing an outcome event.

Slice 195 adds repository-backed retention job leases and uses them from the
explicit loop step. `AutomationRepository` migration version 2 creates
`automation_memory_retention_leases`; callers can acquire a lease when no active
lease exists or the stored lease has expired, release only with the matching
owner, and receive `std::nullopt` on active-lease conflicts. The loop plans and
waits without a lease, then acquires the stored lease immediately before due
`MemoryRetentionService::tick(...)` execution and releases it after the tick.
This keeps cancellation while waiting from leaving retained lease state while
still preventing overlapping due execution for the same job.

Slice 196 adds the first finite loop policy above the leased step without
making automation a daemon. `MemoryRetentionLoop::run(...)` repeatedly calls
`run_once(...)` for one stored job until either the caller-provided
`max_iterations` limit is reached or the next step reports no due work within
the remaining wait budget. The result reports iteration count, due-run count,
total wait time, stop reason, and the last step. It can catch up an overdue
stored retention backlog because every successful tick advances
`last_fired_at` by exactly one scheduled fire, but it still remains one
explicit awaitable owned by the caller. Bootstrap still does not open
`automation.db`, start timers, spawn detached work, or run this loop.

Slice 197 adds the first cron-category planning primitive. `CronSchedule`
stores a POSIX 5-field UTC expression plus a `first_fire_at` anchor for
never-fired jobs, and `evaluate_cron_schedule(...)` parses `*`, lists, ranges,
and steps into deterministic next-fire metadata over caller-supplied
`PeriodicJobState` and `now`. It returns the same `PeriodicEvaluation` shape as
periodic jobs and intentionally does not persist cron jobs, open automation
state, start timers, spawn detached work, or call agents.

Slice 198 persists cron-category job state without introducing a scheduler.
`AutomationRepository` migration version 3 creates `automation_cron_jobs`; the
repository can upsert, load, mark fired, and list cron jobs keyed by durable
`job_key`. Stored rows carry `CronSchedule` (`expression`, `first_fire_at`),
nullable `PeriodicJobState::last_fired_at`, and created/updated timestamps.
Repository validation reuses `evaluate_cron_schedule(...)` before touching
SQLite, enforces positive list limits, returns `std::nullopt` for missing
`get_cron_job(...)`, and returns `ErrorKind::not_found` for mutation operations
that require an existing cron job. It still does not read config, start timers,
spawn detached work, or call agents.

Slice 199 adds the first explicit cron runtime scan/wait owner without adding a
background scheduler. `<oran/automation/service.hpp>` exports `CronService`,
whose `tick(...)` scans stored cron jobs up to a caller limit, evaluates each
schedule with the stored `PeriodicJobState`, and returns due jobs plus the
earliest next fire without mutating `last_fired_at`. `<oran/automation/loop.hpp>`
exports `CronLoop`, whose `run_once(...)` ticks immediately, optionally waits
within the caller's `max_wait` budget for the earliest next fire, and ticks
again after the wait. `AutomationRuntime` constructs both over the owned
repository/executor. This still does not read cron config, run job payloads,
mark jobs fired, enqueue work, notify channels, or call agents.

Slice 200 adds the first explicit cron due-execution owner without adding a
background scheduler. `CronService::execute_due(...)` reuses the scan result,
calls a supplied handler for every due cron job, advances
`last_fired_at` through `AutomationRepository::mark_cron_job_fired(...)` only
after the handler succeeds, and reports handler failures per attempt while
leaving failed jobs due for the next explicit call. It still does not read cron
config, enqueue work, notify channels, or call agents.

Slice 201 adds finite caller-owned cron loop policy over that explicit execution
owner. `CronLoop::run(...)` repeatedly calls `execute_due(...)` until the
caller-provided iteration limit is reached, no due work remains within the
remaining wait budget, or a handler failure is observed. It can catch up overdue
cron fires one scheduled fire at a time and can wait for the next fire within
`max_total_wait`, but it still does not start a process service, publish hooks,
queue work, notify channels, or call agents.

Slice 202 adds advisory cron job lifecycle metadata from the same explicit
execution owner. `CronServiceOptions::hooks` accepts a non-owning `hook::Bus*`,
source label, agent key, and identity. For each due cron job,
`CronService::execute_due(...)` publishes `job_started` before invoking the
caller handler, publishes `job_failed` after a handler failure while leaving
stored cron state unchanged, and publishes `job_finished` only after the handler
succeeds and `last_fired_at` has durably advanced. Sink failures remain advisory
and cannot veto the handler or state advancement. Repository failures before a
durable outcome still return to the caller without inventing an outcome event.

Slice 203 adds cron config ownership without adding scheduler startup.
`oran-config` parses `automation.cron.jobs[]` as typed schedule seeds with
`job_key`, POSIX cron `expression`, UTC `first_fire_at`, and optional UTC
`last_fired_at`. `bootstrap::cron_jobs_from(...)` validates those expressions
through `evaluate_cron_schedule(...)` and maps them into
`automation::UpsertCronJobRequest` rows. `RuntimeAssemblyOptions::cron_jobs`
and `RuntimeAssembly::cron_jobs()` store those descriptors for diagnostics and
future runtime owners, while `RuntimeAssembly::build(...)` still does not open
`automation.db`, upsert cron rows, start timers, enqueue work, notify channels,
or call agents.

Slice 204 adds explicit cron seed persistence on the caller-owned runtime
handle. `AutomationRuntime::apply_cron_job_seeds(...)` takes mapped
`automation::UpsertCronJobRequest` rows, upserts them through the owned
repository, and returns requested/upserted counts plus the stored rows. Failures
return the repository error with `seed_index` and, when present, `job_key`
context. The helper is deliberately not called by bootstrap: a caller must open
`AutomationRuntime` and invoke it explicitly before config-authored cron rows
exist in `automation.db`.

Slice 205 adds a small explicit cron service-cycle policy on the same runtime
handle. `AutomationRuntime::run_cron_service_cycle(...)` validates the supplied
handler, wait budget, iteration limit, and job limit before repository work;
then it applies caller-supplied cron seeds and delegates execution to the
existing finite `CronLoop::run(...)`. The helper returns both the seed-apply
summary and loop summary. Invalid cycle policy fails before seeds are written,
seed-apply errors stop before the handler runs, and loop errors propagate
unchanged. It is still an awaited call, not a detached process timer or
bootstrap-owned service.

Slice 206 adds cron run history without turning cron execution into a queue.
Migration version 4 creates `automation_cron_runs`, keyed by autoincrement id
and linked to `automation_cron_jobs(job_key)`. `AutomationRepository` can record
and newest-first list success/failure cron runs for one durable job key, and
validates positive list limits, non-empty job keys, finish time after fire time,
and failure rows carrying a non-empty error message. `CronService::execute_due`
records one run row for every due handler attempt, exposes it through
`CronExecuteAttempt::run`, records no rows for not-due scans, and still advances
stored cron state only after handler success. The repository writes remain
explicit and non-transactional, matching the existing retention run path.

Slice 207 adds a cooperative stop policy to the finite cron loop without
starting scheduler ownership. `CronLoopRunRequest::stop_requested` is an
optional synchronous predicate checked before each loop iteration and again
after a completed execution. When it returns true, `CronLoop::run(...)` returns
`stop_reason=stop_requested` instead of starting more work or sleeping again.
`AutomationRuntime::run_cron_service_cycle(...)` forwards the same predicate to
the owned loop. This is graceful loop shutdown policy only; it does not cancel a
handler already running, interrupt sleep without parent cancellation, start a
timer, enqueue work, notify channels, or call agents.

Slice 208 classifies explicit cron handler run history without adding queue,
notifier, or scheduler ownership. `CronRunOutcome` stores `success`,
`failure`, or `aborted` on `automation_cron_runs`; migration version 5 backfills
existing rows from the legacy success flag, and `CronRunRecord::success`
remains as a compatibility convenience. `CronService::execute_due(...)`
records `ErrorKind::cancelled` handler errors as `aborted`, other handler
errors as `failure`, and successful handlers as `success`. Failed and aborted
attempts still leave stored cron state unchanged for explicit retry, and the
existing advisory `job_failed` lifecycle event remains the hook surface for a
cancelled handler result in this slice.

Slice 209 adds repository-backed cron execution leases without adding queue,
notifier, agent, or detached scheduler ownership. Migration version 6 creates
`automation_cron_leases`; `AutomationRepository` can acquire a lease when no
active holder exists or the stored lease has expired, release only for the
matching owner, and return `std::nullopt` for active conflicts.
`CronService::execute_due(...)` enables that protection when callers provide
`lease_owner_key` and `lease_ttl`; it acquires before invoking the handler,
releases after durable run/state work, and returns `ErrorKind::conflict` before
handler execution when another owner holds the lease. `CronLoop::run(...)` and
`AutomationRuntime::run_cron_service_cycle(...)` default to the
`automation-cron-loop` owner, while direct service execution remains opt-in.

Slice 210 adds the first cron agent-key lease policy without adding queue,
notifier, agent invocation, or detached scheduler ownership. Config-authored
cron seeds and stored cron jobs now carry `agent_key`, defaulting to
`automation` when omitted. Migration version 7 appends
`automation_cron_jobs.agent_key` and creates `automation_cron_agent_leases`;
`AutomationRepository` can acquire/release agent leases by `agent_key` with the
same active-conflict and expired-takeover semantics as job leases.
`CronService::execute_due(...)` acquires both the per-job lease and the stored
job's per-agent lease when callers provide `lease_owner_key` and `lease_ttl`.
Active conflicts on either lease return `ErrorKind::conflict` before the handler
runs, and durable success/failure paths release both leases. Cron lifecycle hook
payloads use the stored cron job `agent_key`.

Slice 211 adds the first triggered-category intake boundary without adding
queue, notifier, agent invocation, or detached scheduler ownership. Migration
version 8 creates `automation_triggered_jobs`; `AutomationRepository` can
upsert/load/list stored descriptors by durable `job_key` and external
`trigger_key`, with a stored `agent_key` defaulting to `automation`.
`TriggeredService::intake(...)` validates a caller-supplied `trigger_key` plus
positive match limit and returns the matching stored descriptors with the
caller-supplied intake timestamp. `AutomationRuntime::triggered_service()`
constructs that service over caller-owned state. The boundary is descriptor
matching only: it does not persist trigger events, enqueue work, publish hooks,
notify channels, or call agents.

Slice 212 adds explicit triggered execution/run history without adding queue,
notifier, agent invocation, leases, or detached scheduler ownership. Migration
version 9 creates `automation_triggered_runs`; `AutomationRepository` can record
and newest-first list triggered run rows for one durable job key with
`success`, `failure`, or `aborted` outcomes. `TriggeredService::execute(...)`
reuses the caller-owned intake boundary, invokes a supplied handler once per
matched descriptor, records one run row per attempt, stores
`ErrorKind::cancelled` handler errors as `aborted`, stores other handler errors
as `failure`, and continues to other matched jobs. It still does not persist a
queue, publish hooks, notify channels, acquire triggered agent leases, call
agents, or make bootstrap open automation state.

Slice 213 adds advisory triggered lifecycle metadata to the same explicit
execution owner. `TriggeredServiceOptions::hooks` accepts a non-owning
`hook::Bus*` plus source/identity labels, and `AutomationRuntime` can pass those
options into `triggered_service(...)`. When a bus is supplied,
`TriggeredService::execute(...)` publishes metadata-only `job_started` before
the caller handler, `job_failed` after a failed handler attempt has been
durably recorded, and `job_finished` after a successful handler attempt has
been durably recorded. The payload uses `job_type=triggered`, the stored
triggered job `agent_key`, the durable `job_key`, and the caller-supplied
`received_at` timestamp for scheduled/start/finish timing. Sink failures remain
advisory and cannot veto or fail triggered execution.

Slice 214 adds repository-backed triggered agent leases to the same explicit
execution owner without adding queues, notifiers, agent invocation, or detached
scheduler ownership. Migration version 10 creates
`automation_triggered_agent_leases`; `AutomationRepository` can acquire and
release lease rows keyed by stored triggered `agent_key`, with the same
active-conflict, expired-takeover, and owner-matched release semantics as cron
agent leases. `TriggeredService::execute(...)` can opt into that lease boundary
with `TriggeredExecuteRequest::lease_owner_key` and `lease_ttl`; active
same-agent conflicts return `ErrorKind::conflict` before handlers, run rows, or
lifecycle hooks, and durable success/failure paths release the lease.

Slice 215 adds a caller-owned bounded `TriggeredQueue` above triggered intake.
It reuses `TriggeredService::intake(...)`, stores matched descriptors in
bounded process-local `async::Channel` state, and lets consumers explicitly
`receive()` queued work. When the queue is full, the first shipped overflow
policy is `drop_newest`: the attempted row is returned as a dropped item and,
when callers provide a hook bus, publishes advisory `hook::Event::job_dropped`
with metadata-only `JobDroppedPayload`. The queue still does not execute
handlers, record triggered run rows, notify channels, call agents, or start a
detached drain loop.

Slice 216 adds the explicit one-item drain boundary without making the queue a
background service. `TriggeredService::execute_one(...)` executes exactly one
caller-provided `TriggeredExecutionJob`, reusing the same run-record,
lifecycle-hook, and optional triggered-agent-lease behavior as the existing
multi-match `execute(...)` path. `TriggeredQueue::drain_once(...)` receives one
queued descriptor and executes only that descriptor through the service. It does
not re-intake by trigger key, drain later queued jobs, notify channels, call
agents, or define blocked-agent hold/drop policy for lease conflicts.

## Public API

```cpp
namespace orangutan::automation {

struct PeriodicSchedule {
  core::Time first_fire_at;
  std::chrono::nanoseconds interval;
};

struct PeriodicJobState {
  std::optional<core::Time> last_fired_at;
};

struct PeriodicEvaluation {
  bool due;
  core::Time next_fire_at;
  std::chrono::nanoseconds overdue_by;
};

struct CronSchedule {
  std::string expression;
  core::Time first_fire_at;
};

struct LongtermMemoryRetentionPolicy {
  std::chrono::days forget_after_unused;
  double importance_floor;
  std::size_t max_records_per_scope;
  std::chrono::hours decay_check_interval;
};

struct MemoryRetentionJob {
  std::string scope_key;
  LongtermMemoryRetentionPolicy policy;
  core::Time first_fire_at;
};

struct MemoryRetentionPlan {
  PeriodicEvaluation schedule;
  std::optional<memory::longterm::DecayRequest> decay_request;
};

core::Result<PeriodicEvaluation>
evaluate_periodic_schedule(PeriodicSchedule, PeriodicJobState, core::Time now);

core::Result<PeriodicEvaluation>
evaluate_cron_schedule(CronSchedule, PeriodicJobState, core::Time now);

core::Result<MemoryRetentionPlan>
plan_memory_retention(MemoryRetentionJob, PeriodicJobState, core::Time now);

struct AutomationRepositoryOptions {
  std::string migrations_directory;
};

struct AutomationRuntimeOptions {
  std::string database_path;
  std::size_t reader_count;
  std::size_t statement_cache_capacity;
  int busy_timeout_ms;
  bool enable_wal;
  bool enforce_foreign_keys;
  AutomationRepositoryOptions repository;
};

struct UpsertCronJobRequest {
  std::string job_key;
  std::string agent_key;
  CronSchedule schedule;
  PeriodicJobState state;
};

struct CronJobRecord {
  std::string job_key;
  std::string agent_key;
  CronSchedule schedule;
  PeriodicJobState state;
  std::string created_at;
  std::string updated_at;
};

struct ListCronJobsOptions {
  std::size_t limit;
};

struct UpsertTriggeredJobRequest {
  std::string job_key;
  std::string trigger_key;
  std::string agent_key;
};

struct TriggeredJobRecord {
  std::string job_key;
  std::string trigger_key;
  std::string agent_key;
  std::string created_at;
  std::string updated_at;
};

struct ListTriggeredJobsOptions {
  std::string trigger_key;
  std::size_t limit;
};

enum class TriggeredRunOutcome {
  success,
  failure,
  aborted,
};

struct RecordTriggeredRunRequest {
  std::string job_key;
  std::string trigger_key;
  core::Time fired_at;
  core::Time finished_at;
  TriggeredRunOutcome outcome;
  std::optional<std::string> error_message;
};

struct TriggeredRunRecord {
  std::int64_t id;
  std::string job_key;
  std::string trigger_key;
  core::Time fired_at;
  core::Time finished_at;
  bool success;
  TriggeredRunOutcome outcome;
  std::optional<std::string> error_message;
  std::string created_at;
};

struct ListTriggeredRunsOptions {
  std::string job_key;
  std::size_t limit;
};

enum class CronRunOutcome {
  success,
  failure,
  aborted,
};

struct RecordCronRunRequest {
  std::string job_key;
  core::Time fired_at;
  core::Time finished_at;
  CronRunOutcome outcome;
  std::optional<std::string> error_message;
};

struct CronRunRecord {
  std::int64_t id;
  std::string job_key;
  core::Time fired_at;
  core::Time finished_at;
  bool success;
  CronRunOutcome outcome;
  std::optional<std::string> error_message;
  std::string created_at;
};

struct ListCronRunsOptions {
  std::string job_key;
  std::size_t limit;
};

struct AcquireCronLeaseRequest {
  std::string job_key;
  std::string owner_key;
  core::Time acquired_at;
  core::Time expires_at;
};

struct CronLeaseRecord {
  std::string job_key;
  std::string owner_key;
  core::Time acquired_at;
  core::Time expires_at;
  std::string updated_at;
};

struct ReleaseCronLeaseRequest {
  std::string job_key;
  std::string owner_key;
};

struct AcquireCronAgentLeaseRequest {
  std::string agent_key;
  std::string owner_key;
  core::Time acquired_at;
  core::Time expires_at;
};

struct CronAgentLeaseRecord {
  std::string agent_key;
  std::string owner_key;
  core::Time acquired_at;
  core::Time expires_at;
  std::string updated_at;
};

struct ReleaseCronAgentLeaseRequest {
  std::string agent_key;
  std::string owner_key;
};

struct AcquireTriggeredAgentLeaseRequest {
  std::string agent_key;
  std::string owner_key;
  core::Time acquired_at;
  core::Time expires_at;
};

struct TriggeredAgentLeaseRecord {
  std::string agent_key;
  std::string owner_key;
  core::Time acquired_at;
  core::Time expires_at;
  std::string updated_at;
};

struct ReleaseTriggeredAgentLeaseRequest {
  std::string agent_key;
  std::string owner_key;
};

struct CronTickRequest {
  core::Time now;
  std::size_t job_limit;
};

struct CronDueJob {
  CronJobRecord job;
  PeriodicEvaluation schedule;
};

struct CronTickResult {
  core::Time now;
  std::size_t checked_count;
  std::vector<CronDueJob> due_jobs;
  std::optional<core::Time> next_fire_at;
};

using CronJobHandler =
    std::function<async::Awaitable<core::Result<void>>(CronDueJob)>;

struct CronExecuteRequest {
  core::Time now;
  std::size_t job_limit;
  CronJobHandler handler;
  std::string lease_owner_key;
  std::chrono::steady_clock::duration lease_ttl;
};

struct CronExecuteAttempt {
  CronDueJob due;
  bool advanced;
  std::optional<core::Error> error;
  std::optional<CronRunRecord> run;
  std::optional<CronJobRecord> marked_job;
};

struct CronExecuteResult {
  CronTickResult tick;
  std::size_t attempted_count;
  std::size_t advanced_count;
  std::vector<CronExecuteAttempt> attempts;
};

struct UpsertMemoryRetentionJobRequest {
  std::string job_key;
  MemoryRetentionJob job;
  PeriodicJobState state;
};

struct MemoryRetentionJobRecord {
  std::string job_key;
  MemoryRetentionJob job;
  PeriodicJobState state;
  std::string created_at;
  std::string updated_at;
};

struct RecordMemoryRetentionRunRequest {
  std::string job_key;
  core::Time fired_at;
  core::Time finished_at;
  bool success;
  std::size_t shadowed_count;
  std::optional<std::string> error_message;
};

struct MemoryRetentionRunRecord {
  std::int64_t id;
  std::string job_key;
  core::Time fired_at;
  core::Time finished_at;
  bool success;
  std::size_t shadowed_count;
  std::optional<std::string> error_message;
  std::string created_at;
};

struct ListMemoryRetentionRunsOptions {
  std::string job_key;
  std::size_t limit;
};

struct AcquireMemoryRetentionLeaseRequest {
  std::string job_key;
  std::string owner_key;
  core::Time acquired_at;
  core::Time expires_at;
};

struct MemoryRetentionLeaseRecord {
  std::string job_key;
  std::string owner_key;
  core::Time acquired_at;
  core::Time expires_at;
  std::string updated_at;
};

struct ReleaseMemoryRetentionLeaseRequest {
  std::string job_key;
  std::string owner_key;
};

class AutomationRepository {
 public:
  explicit AutomationRepository(storage::Pool&, AutomationRepositoryOptions = {});

  async::Awaitable<core::Result<storage::MigrationReport>> migrate();
  async::Awaitable<core::Result<CronJobRecord>>
  upsert_cron_job(UpsertCronJobRequest);
  async::Awaitable<core::Result<std::optional<CronJobRecord>>>
  get_cron_job(std::string job_key);
  async::Awaitable<core::Result<CronJobRecord>>
  mark_cron_job_fired(std::string job_key, core::Time fired_at);
  async::Awaitable<core::Result<std::vector<CronJobRecord>>>
  list_cron_jobs(ListCronJobsOptions = {});
  async::Awaitable<core::Result<TriggeredJobRecord>>
  upsert_triggered_job(UpsertTriggeredJobRequest);
  async::Awaitable<core::Result<std::optional<TriggeredJobRecord>>>
  get_triggered_job(std::string job_key);
  async::Awaitable<core::Result<std::vector<TriggeredJobRecord>>>
  list_triggered_jobs(ListTriggeredJobsOptions);
  async::Awaitable<core::Result<TriggeredRunRecord>>
  record_triggered_run(RecordTriggeredRunRequest);
  async::Awaitable<core::Result<std::vector<TriggeredRunRecord>>>
  list_triggered_runs(ListTriggeredRunsOptions);
  async::Awaitable<core::Result<CronRunRecord>>
  record_cron_run(RecordCronRunRequest);
  async::Awaitable<core::Result<std::vector<CronRunRecord>>>
  list_cron_runs(ListCronRunsOptions);
  async::Awaitable<core::Result<std::optional<CronLeaseRecord>>>
  acquire_cron_lease(AcquireCronLeaseRequest);
  async::Awaitable<core::Result<bool>>
  release_cron_lease(ReleaseCronLeaseRequest);
  async::Awaitable<core::Result<std::optional<CronAgentLeaseRecord>>>
  acquire_cron_agent_lease(AcquireCronAgentLeaseRequest);
  async::Awaitable<core::Result<bool>>
  release_cron_agent_lease(ReleaseCronAgentLeaseRequest);
  async::Awaitable<core::Result<std::optional<TriggeredAgentLeaseRecord>>>
  acquire_triggered_agent_lease(AcquireTriggeredAgentLeaseRequest);
  async::Awaitable<core::Result<bool>>
  release_triggered_agent_lease(ReleaseTriggeredAgentLeaseRequest);
  async::Awaitable<core::Result<MemoryRetentionJobRecord>>
  upsert_memory_retention_job(UpsertMemoryRetentionJobRequest);
  async::Awaitable<core::Result<std::optional<MemoryRetentionJobRecord>>>
  get_memory_retention_job(std::string job_key);
  async::Awaitable<core::Result<MemoryRetentionJobRecord>>
  mark_memory_retention_fired(std::string job_key, core::Time fired_at);
  async::Awaitable<core::Result<MemoryRetentionRunRecord>>
  record_memory_retention_run(RecordMemoryRetentionRunRequest);
  async::Awaitable<core::Result<std::vector<MemoryRetentionRunRecord>>>
  list_memory_retention_runs(ListMemoryRetentionRunsOptions);
  async::Awaitable<core::Result<std::optional<MemoryRetentionLeaseRecord>>>
  acquire_memory_retention_lease(AcquireMemoryRetentionLeaseRequest);
  async::Awaitable<core::Result<bool>>
  release_memory_retention_lease(ReleaseMemoryRetentionLeaseRequest);
};

struct CronLoopRunOnceRequest {
  core::Time now;
  std::chrono::steady_clock::duration max_wait;
  std::size_t job_limit;
};

struct CronLoopRunOnceResult {
  std::chrono::nanoseconds waited_for;
  CronTickResult tick;
};

enum class CronLoopRunStopReason {
  iteration_limit,
  no_due_work,
  handler_failure,
  stop_requested,
};

using CronLoopStopPredicate = std::function<bool()>;

struct CronLoopRunRequest {
  core::Time now;
  std::chrono::steady_clock::duration max_total_wait;
  std::size_t max_iterations;
  std::size_t job_limit;
  CronJobHandler handler;
  std::string lease_owner_key;
  std::chrono::steady_clock::duration lease_ttl;
  CronLoopStopPredicate stop_requested;
};

struct CronLoopRunResult {
  std::size_t iterations;
  std::size_t attempted_count;
  std::size_t advanced_count;
  std::size_t failed_count;
  std::chrono::nanoseconds waited_for;
  CronLoopRunStopReason stop_reason;
  std::optional<CronExecuteResult> last_execution;
};

struct CronHookOptions {
  hook::Bus* bus;
  std::string source;
  std::string agent_key;
  std::string identity;
};

struct CronServiceOptions {
  CronHookOptions hooks;
};

struct TriggeredIntakeRequest {
  std::string trigger_key;
  core::Time received_at;
  std::size_t job_limit;
};

struct TriggeredIntakeResult {
  std::string trigger_key;
  core::Time received_at;
  std::size_t matched_count;
  std::vector<TriggeredJobRecord> jobs;
};

struct TriggeredExecutionJob {
  TriggeredJobRecord job;
  std::string trigger_key;
  core::Time received_at;
};

using TriggeredJobHandler =
    std::function<async::Awaitable<core::Result<void>>(TriggeredExecutionJob)>;

struct TriggeredExecuteRequest {
  std::string trigger_key;
  core::Time received_at;
  std::size_t job_limit;
  TriggeredJobHandler handler;
  std::string lease_owner_key;
  std::chrono::steady_clock::duration lease_ttl;
};

struct TriggeredExecuteAttempt {
  TriggeredExecutionJob execution;
  bool completed;
  std::optional<core::Error> error;
  std::optional<TriggeredRunRecord> run;
};

struct TriggeredExecuteResult {
  TriggeredIntakeResult intake;
  std::size_t attempted_count;
  std::size_t completed_count;
  std::vector<TriggeredExecuteAttempt> attempts;
};

struct TriggeredExecuteOneRequest {
  TriggeredExecutionJob execution;
  TriggeredJobHandler handler;
  std::string lease_owner_key;
  std::chrono::steady_clock::duration lease_ttl;
};

struct TriggeredExecuteOneResult {
  TriggeredExecuteAttempt attempt;
  bool completed;
};

struct TriggeredHookOptions {
  hook::Bus* bus;
  std::string source;
  std::string agent_key;
  std::string identity;
};

struct TriggeredServiceOptions {
  TriggeredHookOptions hooks;
};

enum class TriggeredQueueOverflowPolicy {
  drop_newest,
};

enum class TriggeredQueueDropReason {
  queue_full,
};

struct TriggeredQueuedJob {
  TriggeredExecutionJob execution;
  core::Time enqueued_at;
};

struct TriggeredDroppedJob {
  TriggeredExecutionJob execution;
  TriggeredQueueDropReason reason;
  core::Time dropped_at;
  std::size_t queue_capacity;
  std::size_t queue_size;
};

struct TriggeredQueueOptions {
  std::size_t capacity;
  TriggeredQueueOverflowPolicy overflow_policy;
  TriggeredHookOptions hooks;
};

struct TriggeredQueueEnqueueRequest {
  std::string trigger_key;
  core::Time received_at;
  std::size_t job_limit;
};

struct TriggeredQueueEnqueueResult {
  TriggeredIntakeResult intake;
  std::size_t enqueued_count;
  std::size_t dropped_count;
  std::vector<TriggeredQueuedJob> enqueued;
  std::vector<TriggeredDroppedJob> dropped;
};

struct TriggeredQueueDrainOnceRequest {
  TriggeredJobHandler handler;
};

struct TriggeredQueueDrainOnceResult {
  TriggeredQueuedJob queued;
  TriggeredExecuteOneResult execution;
};

class TriggeredService {
 public:
  explicit TriggeredService(AutomationRepository&, TriggeredServiceOptions = {});

  async::Awaitable<core::Result<TriggeredIntakeResult>>
  intake(TriggeredIntakeRequest);
  async::Awaitable<core::Result<TriggeredExecuteOneResult>>
  execute_one(TriggeredExecuteOneRequest);
  async::Awaitable<core::Result<TriggeredExecuteResult>>
  execute(TriggeredExecuteRequest);
  AutomationRepository& repository() noexcept;
  const AutomationRepository& repository() const noexcept;
};

class TriggeredQueue {
 public:
  TriggeredQueue(asio::any_io_executor, TriggeredService, TriggeredQueueOptions = {});

  async::Awaitable<core::Result<TriggeredQueueEnqueueResult>>
  enqueue(TriggeredQueueEnqueueRequest);
  async::Awaitable<core::Result<TriggeredQueuedJob>> receive();
  async::Awaitable<core::Result<TriggeredQueueDrainOnceResult>>
  drain_once(TriggeredQueueDrainOnceRequest);
  void close() noexcept;
  std::size_t capacity() const noexcept;
  std::size_t size() const;
  bool closed() const;
  TriggeredService& service() noexcept;
  const TriggeredService& service() const noexcept;
};

class CronService {
 public:
  explicit CronService(AutomationRepository&, CronServiceOptions = {});

  async::Awaitable<core::Result<CronTickResult>>
  tick(CronTickRequest);
  async::Awaitable<core::Result<CronExecuteResult>>
  execute_due(CronExecuteRequest);
  AutomationRepository& repository() noexcept;
  const AutomationRepository& repository() const noexcept;
};

class CronLoop {
 public:
  CronLoop(asio::any_io_executor, CronService);

  async::Awaitable<core::Result<CronLoopRunOnceResult>>
  run_once(CronLoopRunOnceRequest);
  async::Awaitable<core::Result<CronLoopRunResult>>
  run(CronLoopRunRequest);
};

struct CronSeedApplyResult {
  std::size_t requested_count;
  std::size_t upserted_count;
  std::vector<CronJobRecord> jobs;
};

struct CronServiceCycleRequest {
  std::vector<UpsertCronJobRequest> seeds;
  CronServiceOptions service_options;
  core::Time now;
  std::chrono::steady_clock::duration max_total_wait;
  std::size_t max_iterations;
  std::size_t job_limit;
  CronJobHandler handler;
  std::string lease_owner_key;
  std::chrono::steady_clock::duration lease_ttl;
  CronLoopStopPredicate stop_requested;
};

struct CronServiceCycleResult {
  CronSeedApplyResult seed_apply;
  CronLoopRunResult loop;
};

class AutomationRuntime {
 public:
  static async::Awaitable<core::Result<AutomationRuntime>>
  open(asio::any_io_executor executor, AutomationRuntimeOptions);

  std::string_view database_path() const noexcept;
  const storage::MigrationReport& migration_report() const noexcept;
  AutomationRepository& repository() noexcept;
  const AutomationRepository& repository() const noexcept;

  async::Awaitable<core::Result<CronSeedApplyResult>>
  apply_cron_job_seeds(std::vector<UpsertCronJobRequest>);

  async::Awaitable<core::Result<CronServiceCycleResult>>
  run_cron_service_cycle(CronServiceCycleRequest);

  CronService cron_service(CronServiceOptions = {}) noexcept;
  CronLoop cron_loop(CronServiceOptions = {}) noexcept;
  TriggeredService triggered_service(TriggeredServiceOptions = {}) noexcept;
  TriggeredQueue triggered_queue(TriggeredQueueOptions = {});

  MemoryRetentionService memory_retention_service(
      memory::longterm::Backend&,
      MemoryRetentionServiceOptions = {}) noexcept;

  MemoryRetentionLoop memory_retention_loop(
      memory::longterm::Backend&,
      MemoryRetentionServiceOptions = {}) noexcept;
};

struct MemoryRetentionLoopRunOnceRequest {
  std::string job_key;
  core::Time now;
  std::chrono::steady_clock::duration max_wait;
  std::string lease_owner_key;
  std::chrono::steady_clock::duration lease_ttl;
};

struct MemoryRetentionLoopRunOnceResult {
  std::chrono::nanoseconds waited_for;
  MemoryRetentionTickResult tick;
};

enum class MemoryRetentionLoopRunStopReason {
  iteration_limit,
  no_due_work,
};

struct MemoryRetentionLoopRunRequest {
  std::string job_key;
  core::Time now;
  std::chrono::steady_clock::duration max_total_wait;
  std::size_t max_iterations;
  std::string lease_owner_key;
  std::chrono::steady_clock::duration lease_ttl;
};

struct MemoryRetentionLoopRunResult {
  std::size_t iterations;
  std::size_t due_runs;
  std::chrono::nanoseconds waited_for;
  MemoryRetentionLoopRunStopReason stop_reason;
  std::optional<MemoryRetentionLoopRunOnceResult> last_step;
};

struct MemoryRetentionTickRequest {
  std::string job_key;
  core::Time now;
};

struct MemoryRetentionHookOptions {
  hook::Bus* bus;
  std::string source;
  std::string agent_key;
  std::string identity;
};

struct MemoryRetentionServiceOptions {
  MemoryRetentionHookOptions hooks;
};

struct MemoryRetentionHookPublishResult {
  std::size_t sink_count;
  std::size_t failure_count;
};

struct MemoryRetentionTickResult {
  std::string job_key;
  PeriodicEvaluation schedule;
  bool ran;
  std::size_t shadowed_count;
  std::optional<MemoryRetentionJobRecord> job;
  std::optional<MemoryRetentionRunRecord> run;
  std::optional<MemoryRetentionHookPublishResult> hook_publish;
};

class MemoryRetentionService {
 public:
  MemoryRetentionService(AutomationRepository&,
                         memory::longterm::Backend&,
                         MemoryRetentionServiceOptions = {});

  async::Awaitable<core::Result<MemoryRetentionTickResult>>
  tick(MemoryRetentionTickRequest);
  AutomationRepository& repository() noexcept;
  const AutomationRepository& repository() const noexcept;
};

class MemoryRetentionLoop {
 public:
  MemoryRetentionLoop(asio::any_io_executor, MemoryRetentionService);

  async::Awaitable<core::Result<MemoryRetentionLoopRunOnceResult>>
  run_once(MemoryRetentionLoopRunOnceRequest);

  async::Awaitable<core::Result<MemoryRetentionLoopRunResult>>
  run(MemoryRetentionLoopRunRequest);
};

}  // namespace orangutan::automation
```

Config/bootstrap-facing cron seed surface:

```cpp
namespace orangutan::config {

struct AutomationCronJobConfig {
  std::string job_key;
  std::string agent_key;
  std::string expression;
  core::Time first_fire_at;
  std::optional<core::Time> last_fired_at;
};

struct AutomationCronConfig {
  std::vector<AutomationCronJobConfig> jobs;
};

struct AutomationConfig {
  AutomationCronConfig cron;
};

}  // namespace orangutan::config

namespace orangutan::bootstrap {

core::Result<std::vector<automation::UpsertCronJobRequest>>
cron_jobs_from(const config::Config&);

struct RuntimeAssemblyOptions {
  std::vector<automation::UpsertCronJobRequest> cron_jobs;
};

class RuntimeAssembly {
 public:
  const std::vector<automation::UpsertCronJobRequest>&
  cron_jobs() const noexcept;
};

}  // namespace orangutan::bootstrap
```

## Periodic Schedule Semantics

`PeriodicSchedule::first_fire_at` anchors a never-fired job. Once a caller has a
`last_fired_at`, the next scheduled fire is `last_fired_at + interval`.

`evaluate_periodic_schedule(...)` is pure and caller-clocked:

- It rejects non-positive intervals with `ErrorKind::invalid_argument` and
  `field=interval`.
- It returns `due=false` when `now` is before the next fire.
- It returns `due=true` when `now >= next_fire_at`.
- It reports `overdue_by = now - next_fire_at` only for due work.

The evaluator does not skip forward over multiple missed intervals. The first
real service loop will decide whether to catch up, coalesce, or drop missed
firings based on job policy and backpressure state.

## Cron Schedule Semantics

`CronSchedule` is the first cron-category primitive. Its `expression` uses the
POSIX 5-field shape:

```text
minute hour day-of-month month day-of-week
```

All evaluation is UTC and minute-granular. Supported field syntax is `*`, comma
lists, inclusive ranges, and `/step` suffixes. Numeric ranges are:

- minute: `0..59`
- hour: `0..23`
- day of month: `1..31`
- month: `1..12`
- day of week: `0..7`, where both `0` and `7` mean Sunday.

When both day-of-month and day-of-week are restricted, the evaluator uses the
standard cron OR behavior: either matching field can make the day eligible.
When one of those fields is `*`, the restricted field controls the day match.

`CronSchedule::first_fire_at` is the earliest scheduled minute a never-fired
job may return. If it is not already minute-aligned, evaluation starts at the
next UTC minute. Once `PeriodicJobState::last_fired_at` exists, evaluation
starts at the minute after that stored fire, clamped no earlier than
`first_fire_at`. This mirrors periodic evaluation by returning one next
scheduled fire without skipping over a backlog. The repository can now persist
that state for cron jobs, but later service-loop policy still owns catch-up,
coalescing, dropping, and timer execution.

Malformed expressions return `ErrorKind::invalid_argument` with `field` set to
`expression`, `minute`, `hour`, `day_of_month`, `month`, or `day_of_week` and a
`reason` such as `field_count`, `number`, `range`, `step`, or
`no_matching_fire`. The evaluator bounds its search window so impossible
schedules cannot spin forever.

## Cron Config Seed Semantics

`automation.cron.jobs[]` is the first operator-facing cron configuration
surface. Each row describes repository seed state only:

- `job_key`: durable non-empty repository identity, unique within the authored
  config.
- `agent_key`: optional non-empty agent execution key, defaulting to
  `automation` when omitted.
- `expression`: POSIX 5-field cron expression.
- `first_fire_at`: UTC ISO-8601 timestamp for the never-fired schedule anchor.
- `last_fired_at`: optional UTC ISO-8601 timestamp for a pre-seeded stored
  state.

`oran-config` validates the object shape, timestamps, non-empty strings, and
unique keys. Missing `agent_key` is normalized to `automation`; present empty
values are rejected. Config deliberately does not implement cron parsing,
because that syntax is owned by `oran-automation`.
`bootstrap::cron_jobs_from(...)` is the composition helper that can depend on
both libraries: it validates each expression through
`evaluate_cron_schedule(...)` and maps the row, including `agent_key`, into an
`AutomationRepository::upsert_cron_job(...)` request shape.

`RuntimeAssemblyOptions::cron_jobs` and `RuntimeAssembly::cron_jobs()` preserve
those mapped seeds for diagnostics and for the future runtime owner that will
choose when to persist them. `RuntimeAssembly::build(...)` still does not open
`automation.db`, run migrations, upsert rows, start timers, publish hooks,
enqueue work, notify channels, or call agents.
`bootstrap::run(...)` performs cron seed mapping for any loaded config before
building the assembly, even when no provider route is configured; long-term
memory retention descriptors remain configured-route-only.

`AutomationRuntime::apply_cron_job_seeds(...)` is the explicit persistence
bridge for those descriptors. It accepts the seed vector by value, sequentially
calls `AutomationRepository::upsert_cron_job(...)`, and returns
`CronSeedApplyResult { requested_count, upserted_count, jobs }`. On the first
error it returns without attempting later seeds; already-upserted rows remain
committed because this slice does not introduce a transaction wrapper. The
error includes `seed_index` and the seed `job_key` when available so runtime
owners can surface the failing authored row.

## Triggered Intake And Execution Semantics

`automation_triggered_jobs` is the first triggered-category state boundary. A
stored row has:

- `job_key`: durable non-empty repository identity.
- `trigger_key`: external event routing key supplied by a runtime owner, such
  as a webhook route, signal name, or file-watch topic.
- `agent_key`: target agent execution key, defaulting to `automation`.
- `created_at` / `updated_at`: repository timestamps.

`AutomationRepository::upsert_triggered_job(...)` validates all three keys
before touching SQLite. `get_triggered_job(...)` returns `std::nullopt` for a
missing durable job key, and `list_triggered_jobs(...)` requires a non-empty
`trigger_key` plus positive limit and returns newest-updated matching
descriptors.

`TriggeredService::intake(...)` is the caller-owned runtime surface over those
rows. The caller supplies the external `trigger_key`, an intake timestamp, and
a match limit. The service returns the matching stored descriptors and the same
timestamp so later queue/notifier/agent-firing owners can preserve the event
arrival time. Intake deliberately does not persist trigger events, enqueue
jobs, publish hooks, notify channels, acquire leases, or call agents.

`automation_triggered_runs` records explicit handler attempts made by
`TriggeredService::execute(...)`. A run row has the durable `job_key`, the
external `trigger_key`, caller-supplied fire/finish timestamps, a compatibility
`success` flag, typed `TriggeredRunOutcome`, optional error message, and
created timestamp. `AutomationRepository::record_triggered_run(...)` validates
non-empty keys, finish time ordering, and failure/aborted error messages;
`list_triggered_runs(...)` returns newest-first rows for one durable job key.

`TriggeredService::execute(...)` reuses intake, then invokes the supplied
handler once per matched descriptor. It records `success` for successful
handlers, `failure` for ordinary handler errors, and `aborted` for
`ErrorKind::cancelled`. Handler failures are per-attempt results and do not
stop other matched jobs. The execution surface is still explicit caller-owned
work: it does not persist a queue, notify channels, or call agents.

`automation_triggered_agent_leases` records optional same-agent execution
ownership for explicit triggered handlers. The row key is the stored triggered
job `agent_key`; values carry an owner key, acquisition time, expiry time, and
updated timestamp. `AutomationRepository::acquire_triggered_agent_lease(...)`
inserts a new row, takes over expired rows when `expires_at <= acquired_at`, and
returns `std::nullopt` for active conflicts.
`release_triggered_agent_lease(...)` deletes only rows whose owner matches the
request.

When `TriggeredExecuteRequest::lease_owner_key` is non-empty,
`TriggeredService::execute(...)` validates a positive `lease_ttl`, acquires the
matched job's `agent_key` before publishing lifecycle hooks or invoking the
handler, and returns `ErrorKind::conflict` without recording a run row on active
lease conflicts. After a success/failure/aborted attempt is durably recorded,
the service releases the lease before publishing the terminal lifecycle hook.
Release is run with cancellation disabled, matching the existing cron lease
cleanup boundary; if release fails, the execution returns that release error or
attaches it to the primary durable-recording error.

When `TriggeredServiceOptions::hooks.bus` is set, explicit execution also
publishes advisory `hook::Event::job_started` before each matched handler. It
publishes `hook::Event::job_failed` after the failed triggered run row is
recorded and `hook::Event::job_finished` after the successful triggered run row
is recorded. Lifecycle payloads use `job_type=triggered`, carry the durable
`job_key`, stored job `agent_key`, source/identity labels from
`TriggeredHookOptions`, and the caller-supplied `received_at` as scheduled,
started, and finished time for this no-hidden-clock boundary. Advisory sink
failures are ignored by this service; observers cannot veto triggered execution
or turn a successful handler into a failed attempt.

`TriggeredQueue` is the first triggered queue/backpressure owner. It is
process-local and caller-owned: construction receives an executor, a
`TriggeredService`, and `TriggeredQueueOptions`, and it does not open
`automation.db` or start detached work by itself. `enqueue(...)` validates a
non-empty `trigger_key` and positive `job_limit`, then calls
`TriggeredService::intake(...)`. Each matched descriptor becomes a
`TriggeredQueuedJob` preserving the stored job row, external trigger key,
caller-supplied received timestamp, and queue enqueue timestamp.

The queue is bounded by `TriggeredQueueOptions::capacity` and uses
`async::Channel<TriggeredQueuedJob>` as the backing primitive. The only shipped
overflow policy is `drop_newest`: when a matched job cannot be inserted because
the channel is full, the queue records a `TriggeredDroppedJob` with
`reason=queue_full`, queue capacity, queue size, and the same caller-supplied
timestamp as `dropped_at`. It then continues processing later matches instead
of failing the whole enqueue request. Other channel errors, such as closed
queue cancellation, still propagate as errors.

When `TriggeredQueueOptions::hooks.bus` is set, each drop publishes advisory
`hook::Event::job_dropped` with `hook::JobDroppedPayload`. The payload is
metadata-only: source, identity, job key/type, stored agent key, trigger key,
drop reason, queue capacity/size, and scheduled/drop timestamps. It carries no
trigger body, channel payload, prompt bytes, or agent input. Advisory sink
failures remain non-fatal.

`TriggeredQueue::drain_once(...)` is the first queue-drain owner. It validates a
non-empty handler, awaits `receive()` for one `TriggeredQueuedJob`, then calls
`TriggeredService::execute_one(...)` with the queued descriptor. The execution
step records exactly one `automation_triggered_runs` row, publishes the same
advisory lifecycle metadata as service execution when hooks are configured, and
returns both the queued descriptor and the execution attempt. It deliberately
does not call `TriggeredService::execute(...)`, because that would re-intake by
trigger key and execute every stored descriptor matching the key instead of the
single queued descriptor. `drain_once(...)` also does not expose lease fields in
this slice: consuming a queue item before discovering a same-agent lease
conflict would silently drop work unless a later hold/drop policy is defined.
For now, blocked-agent lease policy stays a downstream service-loop slice.

The queue still does not notify channels, call agents, start a detached drain
loop, or define service shutdown policy.

## Memory Retention Planning

`plan_memory_retention(...)` adapts the shipped long-term retention policy into
a due-only `memory::longterm::DecayRequest`:

- `scope_key` is copied from the job.
- `unused_before = now - forget_after_unused`.
- `importance_floor` and `limit` are copied from the policy.
- `decay_at = now`.
- `decay_request` is absent when the cadence is not due.

The planner validates the job before evaluating cadence:

- `scope_key` must be non-empty.
- `forget_after_unused` must be positive.
- `importance_floor` must be finite and in `[0.0, 1.0]`.
- `max_records_per_scope` must be positive.
- `decay_check_interval` must be positive.

The planner does not own a `memory::longterm::Backend`, does not publish
`memory_decay`, and does not update `last_fired_at`. Future scheduler/service
code owns those side effects after a successful run.

## Persistence Semantics

`AutomationRepository` owns the current `automation.db` domain schema above the
generic storage pool. Migration version 1 creates:

- `automation_memory_retention_jobs`, keyed by durable `job_key`, with
  `scope_key`, retention policy fields, `first_fire_at`, nullable
  `last_fired_at`, and timestamps.
- `automation_memory_retention_runs`, keyed by autoincrement `id`, with a
  foreign key to `job_key`, scheduled/finished timestamps, success flag,
  shadowed count, optional error message, and newest-first listing index.

Migration version 2 creates `automation_memory_retention_leases`, keyed by
`job_key`, with `owner_key`, `acquired_at`, `expires_at`, and `updated_at`. The
lease row has a foreign key to `automation_memory_retention_jobs(job_key)` with
`ON DELETE CASCADE`, so deleting a future job row drops the matching lease row.

Migration version 3 creates `automation_cron_jobs`, keyed by durable `job_key`,
with the cron `expression`, `first_fire_at`, nullable `last_fired_at`, and
timestamps. `idx_automation_cron_jobs_updated` lists rows by `updated_at DESC,
job_key ASC` for bounded scheduler scans or diagnostics.

Migration version 4 creates `automation_cron_runs`, keyed by autoincrement
`id`, with a foreign key to `automation_cron_jobs(job_key)`, scheduled fire
time, finished time, success flag, optional error message, and created
timestamp. `idx_automation_cron_runs_job_fired` lists recent runs newest-first
for one durable cron job key.

Migration version 5 adds `automation_cron_runs.outcome` with the typed
identifier spelling `success`, `failure`, or `aborted`, backfilling existing
rows from the v4 success flag. The success flag remains for compatibility, and
repository reads reject rows whose bool success value disagrees with the typed
outcome.

Migration version 6 creates `automation_cron_leases`, keyed by durable
`job_key`, with `owner_key`, `acquired_at`, `expires_at`, and `updated_at`.
The lease row has a foreign key to `automation_cron_jobs(job_key)` with
`ON DELETE CASCADE`, so deleting a future cron job row drops the matching lease
row.

Migration version 7 adds `automation_cron_jobs.agent_key`, defaulting existing
rows to `automation`, and creates `automation_cron_agent_leases`, keyed by
`agent_key`, with `owner_key`, `acquired_at`, `expires_at`, and `updated_at`.
Agent leases are keyed by the execution agent, not by one cron job row, so they
intentionally do not carry a foreign key to `automation_cron_jobs`.

`job_key` is the durable repository identity. `scope_key` remains the memory
decay scope inside the stored job descriptor, so future different automation
jobs or policies can share a memory scope without overwriting each other. Cron
jobs use the same durable `job_key` identity and store the cron expression
directly because there is not yet a higher-level config/job descriptor type.

Repository calls validate inputs before touching SQLite: empty job keys are
rejected, cron agent keys are rejected when empty, cron schedules must pass
`evaluate_cron_schedule(...)`, retention jobs must pass the same policy
validation used by `plan_memory_retention(...)`, failed runs require an error
message, run finish time must not precede fire time, failed or aborted cron runs
require an error message, list limits must be positive, lease owner keys must be
non-empty, and lease expiry must be after acquisition time. Missing jobs return
`std::nullopt` on `get_cron_job(...)` / `get_memory_retention_job(...)` and
`ErrorKind::not_found` from mutation operations that require an existing job.

`upsert_cron_job(...)` replaces the stored agent key, schedule, and last-fired
state for a durable `job_key` while preserving `created_at`;
`mark_cron_job_fired(...)` advances only `last_fired_at` and `updated_at`;
`list_cron_jobs(...)` returns a positive-limit bounded vector ordered by most
recently updated first.
`record_cron_run(...)` appends one `success`, `failure`, or `aborted` outcome
for a due handler attempt, and `list_cron_runs(...)` returns recent rows for a
single cron job by `fired_at DESC, id DESC`. These APIs are state storage only:
they do not evaluate due work, acquire leases, publish hooks, or call agents.

`acquire_cron_lease(...)` returns:

- `CronLeaseRecord` when the job exists and no active lease blocks acquisition.
- `CronLeaseRecord` after replacing an expired lease
  (`stored.expires_at <= request.acquired_at`).
- `std::nullopt` when another owner still holds an active lease.
- `ErrorKind::not_found` when the job row is missing.

`release_cron_lease(...)` deletes only the row whose `job_key` and `owner_key`
match. It returns `true` for a matching release and `false` when the lease is
absent or held by another owner.

`acquire_cron_agent_lease(...)` returns:

- `CronAgentLeaseRecord` when no active lease blocks acquisition for that
  `agent_key`.
- `CronAgentLeaseRecord` after replacing an expired lease
  (`stored.expires_at <= request.acquired_at`).
- `std::nullopt` when another owner still holds an active lease.

`release_cron_agent_lease(...)` deletes only the row whose `agent_key` and
`owner_key` match. It returns `true` for a matching release and `false` when the
lease is absent or held by another owner.

`acquire_memory_retention_lease(...)` returns:

- `MemoryRetentionLeaseRecord` when the job exists and no active lease blocks
  acquisition.
- `MemoryRetentionLeaseRecord` after replacing an expired lease
  (`stored.expires_at <= request.acquired_at`).
- `std::nullopt` when another owner still holds an active lease.
- `ErrorKind::not_found` when the job row is missing.

`release_memory_retention_lease(...)` deletes only the row whose `job_key` and
`owner_key` match. It returns `true` for a matching release and `false` when the
lease is absent or held by another owner.

## Runtime State Handle Semantics

`AutomationRuntime` is the first caller-owned automation state bundle. It
exists so a runtime owner can explicitly open and migrate `automation.db` once,
then keep the `storage::Pool` and `AutomationRepository` lifetimes stable while
service objects borrow them.

`AutomationRuntime::open(executor, options)`:

- Rejects an empty `database_path` with `ErrorKind::invalid_argument`.
- Creates missing parent directories for the configured database file.
- Opens `storage::Pool` with the supplied reader count, statement-cache
  capacity, busy timeout, WAL, and foreign-key options.
- Constructs `AutomationRepository` over the owned pool and runs its migration.
- Stores the returned `storage::MigrationReport` for diagnostics.

The handle exposes `repository()` for direct callers that seed or inspect jobs,
`cron_service()` / `cron_loop()` as convenience factories for the caller-driven
cron scan/wait/execute-due/run surface, and `memory_retention_service(...)` as a
convenience factory for `MemoryRetentionService` over the owned repository plus
a caller-supplied `memory::longterm::Backend`. It also exposes
`memory_retention_loop(...)` as a convenience factory for the caller-started loop
step over the same repository, backend, and hook options.

The runtime handle is intentionally not a scheduler. It does not sleep, spawn
detached coroutines, acquire process leases, own a hook bus, or decide whether
bootstrap should open `automation.db`.
Bootstrap maps the configured retention policy and configured cron schedule
seeds into descriptors only; a caller must explicitly call
`AutomationRuntime::open(...)` plus either `apply_cron_job_seeds(...)` or
`run_cron_service_cycle(...)` before authored cron rows exist in automation
state.

## Loop Step Semantics

`CronService::tick(...)` is the first cron runtime owner above durable cron job
state. It validates `job_limit > 0`, lists at most that many stored cron jobs,
evaluates each `CronSchedule` with its stored `PeriodicJobState`, and returns:

- `checked_count` for the number of stored cron jobs inspected.
- `due_jobs` with the stored record and `PeriodicEvaluation` for each due cron
  job.
- `next_fire_at` for the earliest scheduled fire among the scanned jobs, or
  `std::nullopt` when no jobs were scanned.

The tick is intentionally read-only. It does not call
`mark_cron_job_fired(...)`, create run rows, acquire leases, publish lifecycle
hooks, enqueue work, or call agents. A later execution owner must run the job
payload and advance `last_fired_at` only after its own success policy.

`CronService::execute_due(...)` is that first explicit execution owner, still
without making automation a scheduler. It validates `job_limit > 0` and a
non-empty `handler`; when `lease_owner_key` is non-empty, it also requires a
positive `lease_ttl`. It calls `tick(...)` and, for each due job, optionally
acquires both `automation_cron_leases` for the durable job key and
`automation_cron_agent_leases` for the stored `CronJobRecord::agent_key` before
invoking the handler. Active conflicts on either lease return
`ErrorKind::conflict` before the handler is called; when the job lease was
already acquired and the agent lease cannot be acquired, the job lease is
released before the conflict is returned. Successful handler returns advance the
cron job's stored `last_fired_at` to `due.schedule.next_fire_at` through
`AutomationRepository::mark_cron_job_fired(...)` after a successful cron run row
has been recorded, then release both leases when they were acquired. Handler
errors are recorded as failed cron run rows with the failure message, stored on
the matching `CronExecuteAttempt`, release any held leases, do not stop later
due jobs in the same call, and leave that job's stored state unchanged so the
next explicit call can retry the same scheduled fire.
Handler errors whose kind is `ErrorKind::cancelled` are stored with
`CronRunOutcome::aborted`; other handler errors are stored as
`CronRunOutcome::failure`. Not-due scans record no cron run rows and acquire no
cron leases.

When `CronServiceOptions::hooks.bus` is set, due execution also publishes
advisory `hook::Event::job_started` before the handler. It publishes
`job_failed` after a handler failure, and publishes `job_finished` only after a
handler succeeds and durable cron state has advanced. Payloads use
`job_type=cron`, carry the durable `job_key`, scheduled/start/finish timing,
the configured source label, the stored cron job `agent_key`, the configured
identity label, and failure kind/message when applicable. Cron jobs do not yet
have a domain scope, so
`JobLifecyclePayload::scope_key` and `who.scope_key` are empty for this
producer. Sink failures stay advisory and are not reported in
`CronExecuteResult`.

`CronExecuteResult` carries the original scan result plus `attempted_count`,
`advanced_count`, and one attempt row per due job whose handler ran. Leased
conflicts return an error before an attempt row is appended. An attempt has
`run` populated after the attempt outcome is durably recorded, and has
`advanced=true` plus `marked_job` populated only after durable state has
advanced. If the repository cannot record the run or cannot mark a
handler-successful job fired, `execute_due(...)` returns that repository error
instead of inventing a partial success policy. Repository failures before a
publishable durable outcome return without a `job_finished` or synthetic
`job_failed` outcome. It still does not enqueue work, notify channels, call
agents, or choose retry/backpressure policy for a process service.

`CronLoop::run_once(...)` is a single caller-started awaitable above that tick.
It validates `max_wait >= 0` and `job_limit > 0`, ticks immediately, and returns
without sleeping when any due job is already present or when no stored jobs
exist. If no job is due and the earliest next fire is outside `max_wait`, it
returns the not-due tick with `waited_for=0`. If the earliest next fire is
within budget, it sleeps through `async::sleep_for(...)` and then ticks again
using `now + waited_for`.

Cancellation while the cron loop is sleeping returns `ErrorKind::cancelled` and
does not mutate job state.

`CronLoop::run(...)` is the finite caller-owned loop policy above the same
scan/wait/execute surface. It validates `max_total_wait >= 0`,
`max_iterations > 0`, `job_limit > 0`, a non-empty handler, non-empty
`lease_owner_key`, and positive `lease_ttl`, then repeatedly checks an optional
`stop_requested` predicate and calls `CronService::execute_due(...)` using a
logical caller clock plus the lease policy when no stop has been requested. The
default owner is `automation-cron-loop`, so runtime loops protect due handler
execution unless a caller supplies a different owner. The result reports:

- `iterations`: successful `execute_due(...)` calls made by the loop.
- `attempted_count`: total due handler attempts across those calls.
- `advanced_count`: total cron jobs whose stored state advanced after handler
  success.
- `failed_count`: total handler failures observed in attempt rows.
- `waited_for`: total sleep time consumed by the loop.
- `stop_reason`: why this finite caller-owned run stopped.
- `last_execution`: the final `CronExecuteResult` for diagnostics.

When an execution has due jobs and all handlers succeed, the loop immediately
continues with the same logical `now`. This lets one explicit run catch up an
overdue backlog one stored fire at a time, because each successful execution
advances `last_fired_at` to exactly the scheduled fire. When an execution finds
no due jobs, the loop either sleeps until `next_fire_at` if it fits inside the
remaining `max_total_wait`, or stops with `stop_reason=no_due_work`. When the
iteration limit is reached after a successful execution, it stops with
`stop_reason=iteration_limit`.

If any handler attempt fails, `CronLoop::run(...)` stops with
`stop_reason=handler_failure` and leaves the failed attempt in
`last_execution`. It deliberately does not retry handler failures immediately
inside the same run; process retry/backpressure policy remains downstream. The
loop records only the run rows produced by `execute_due(...)`; it still does
not enqueue work, notify channels, call agents, choose shutdown behavior, or
decide whether bootstrap should open automation state. When the loop's owned
`CronService` was constructed with hook options, each underlying
  `execute_due(...)` call emits the same advisory lifecycle metadata described
  above. Active cron job or agent lease conflicts propagate as
  `ErrorKind::conflict` before an underlying handler is called.

If `stop_requested` returns true before an iteration starts, the loop returns
`stop_reason=stop_requested` with zero additional work. If it returns true after
an execution completes, the loop returns the execution summary already recorded
and does not continue catch-up or enter another sleep. The predicate is
cooperative: it is not polled inside the caller-supplied handler and does not
wake an in-progress `async::sleep_for(...)`; parent cancellation remains the
sleep-interruption path.

`AutomationRuntime::run_cron_service_cycle(...)` is the explicit startup-cycle
composition for callers that already opened automation state. The request
contains cron seed descriptors, cron hook options, the logical `now`, finite
`max_total_wait` / `max_iterations` / `job_limit` policy, and a
caller-supplied `CronJobHandler`, plus cron loop lease owner/TTL and the
optional cooperative stop predicate. Runtime validates the policy before
writing any seeds, applies the seeds through `apply_cron_job_seeds(...)`, then
runs `CronLoop::run(...)` with the same handler, budget, job/agent lease policy,
and stop policy. The result contains both the
`CronSeedApplyResult` and the `CronLoopRunResult`.

This helper exists so a process owner can perform a coherent startup cycle
without duplicating seed-apply-plus-loop code. It is still one awaited call: it
does not spawn detached coroutines, keep a timer alive after return, enqueue
work, notify channels, call agents, or make bootstrap open `automation.db`.
Any cron run rows come only from the delegated explicit `CronLoop::run(...)`
execution path.

`MemoryRetentionLoop::run_once(...)` is a single caller-started awaitable for
one stored retention job. It is the smallest useful owner above
`MemoryRetentionService::tick(...)`: it can wait for the next scheduled fire,
and it leases due execution, but it does not spawn detached coroutines or
decide process startup policy.

The request validates `job_key` non-empty, `max_wait >= 0`,
`lease_owner_key` non-empty, and `lease_ttl > 0`. Invalid inputs return
`ErrorKind::invalid_argument` with the matching `field` before repository or
backend work.

The step first loads the stored job and calls `plan_memory_retention(...)`
without acquiring a lease:

- If the first plan is due, `run_once(...)` acquires the job lease, delegates to
  `MemoryRetentionService::tick(...)`, releases the lease, and returns
  immediately with `waited_for=0`.
- If the first plan is not due and `schedule.next_fire_at - now` is greater
  than `max_wait`, `run_once(...)` returns that not-due tick with
  `waited_for=0` and no lease acquisition.
- If the next fire is within the caller's budget, `run_once(...)` sleeps for the
  exact wait duration through `async::sleep_for(...)` without holding a lease,
  then acquires the job lease and delegates to `MemoryRetentionService::tick(...)`
  using `now + waited_for`.

Lease acquisition uses `AcquireMemoryRetentionLeaseRequest` with the request's
`lease_owner_key`, acquisition time, and `acquisition + lease_ttl` expiry. An
active stored lease returns `ErrorKind::conflict`; an expired lease can be
replaced. After due tick execution, the loop releases the lease with
`ReleaseMemoryRetentionLeaseRequest`. The release path disables further
cancellation while it deletes the row so parent cancellation after due execution
does not strand a lease. If the tick fails and release succeeds, the tick error
is returned; if the tick succeeds and release fails, the release error is
returned. If both tick execution and release fail, the tick error is returned
with `lease_release_error_kind` and `lease_release_error_message` context so the
caller can still see that lease cleanup may need attention.

Cancellation while sleeping is surfaced as `ErrorKind::cancelled`; no second
tick runs after that cancellation result, and no lease has been acquired during
the wait. Repository, lease, backend, and hook errors from due execution are
returned unchanged, so the loop step does not hide failed decay or failed
durable recording.

The loop step intentionally does not skip forward over multiple missed
intervals or catch up a backlog. It reuses the same tick semantics as direct
callers, where `last_fired_at` advances only after successful due work.

`MemoryRetentionLoop::run(...)` is the finite caller-owned loop policy above
that step. It validates `job_key` non-empty, `max_total_wait >= 0`,
`max_iterations > 0`, `lease_owner_key` non-empty, and `lease_ttl > 0`. The
loop starts from caller-supplied `now`, calls `run_once(...)` with the
remaining wait budget, advances its logical `now` only by the wait duration
that actually occurred, and accumulates summary counters:

- `iterations` counts `run_once(...)` calls attempted successfully.
- `due_runs` counts iterations whose tick actually ran retention.
- `waited_for` is the total time slept by loop steps.
- `last_step` carries the final `run_once(...)` result for diagnostics.

`stop_reason=iteration_limit` means the caller's `max_iterations` bound stopped
the loop after the last successful step. `stop_reason=no_due_work` means a step
returned `ran=false`; in normal single-owner operation that means the next fire
is outside the remaining wait budget, and in multi-owner operation it can also
mean another owner advanced the stored job during the wait. Errors from
validation, repository operations, lease conflicts, backend execution, hook
publishing, and cancellation are returned as `std::unexpected` without
inventing a stop reason.

Because `run(...)` keeps the same logical `now` across zero-wait due ticks, it
can catch up an overdue backlog one stored fire at a time until the iteration
limit is reached. It still does not coalesce/drop missed fires, choose a
process shutdown policy, own a queue, spawn detached work, or decide whether
bootstrap should open automation state.

## Tick Semantics

`MemoryRetentionService::tick(...)` is a caller-clocked unit of work. It owns one
attempt to evaluate and possibly execute a stored memory-retention job; it does
not own timers, sleeps, background tasks, queueing, or hook subscription
lifetime.

The tick validates non-empty `job_key`, loads the stored job through
`AutomationRepository`, and returns `ErrorKind::not_found` if the job is absent.
For present jobs, it calls `plan_memory_retention(...)` with the stored job,
stored `PeriodicJobState`, and caller-supplied `now`.

When the plan is not due, the tick returns `ran=false`, the schedule evaluation,
and the stored job record. It does not call the memory backend, record a run row,
or update `last_fired_at`.

When the plan is due, the tick calls the supplied
`memory::longterm::Backend::decay(...)` with the planned `DecayRequest`. A
successful backend result is recorded as a successful run with
`shadowed_count = DecayResult::shadowed_records.size()`, then
`last_fired_at` is advanced to the scheduled fire time
`PeriodicEvaluation::next_fire_at`. This uses the scheduled fire rather than the
wall-clock finish time so cadence does not drift when callers tick late.

If the service was constructed with `MemoryRetentionHookOptions::bus`, a
due tick first publishes advisory `hook::Event::job_started` before calling the
memory backend. On success, it publishes advisory `hook::Event::job_finished`
after the run row is recorded and `last_fired_at` advances, then publishes
advisory `hook::Event::memory_decay` with the planned `DecayRequest` values
(`scope_key`, `unused_before`, `importance_floor`, `limit`, `decay_at`), the
configured source/identity fields, and the number of shadowed records. Because
the tick remains caller-clocked and does not own a separate wall-clock span, the
periodic decay payload uses `decay_at` for both `started_at` and `finished_at`
and reports `duration=0`. Advisory sink errors are summarized only for
`memory_decay` in `MemoryRetentionTickResult::hook_publish`; lifecycle sink
errors are ignored as advisory side-channel failures and do not roll back the
durable run.

If the backend returns an error, the tick records a failed run with the scheduled
fire time, caller-supplied `now` as `finished_at`, zero shadowed records, and a
non-empty error message. After that row is durable, the tick publishes advisory
`hook::Event::job_failed` with the backend error kind/message, then returns the
backend error and does not advance `last_fired_at`. If that best-effort
failure-row write itself fails, the
repository error is returned because the run outcome was not durably recorded.

The public awaitable is composed entirely of repository/backend awaitables and
the optional advisory hook publish; it does not add its own blocking wait or
detached coroutine. Future service-loop ownership can add timers, shutdown,
repeated scheduling, and cancellation policy around this tick without moving
those concerns into bootstrap or `oran-memory`.

## Future Ownership

The next automation slices should not be picked by `STATUS.md` alone. The open
spec 0006 boundaries now start after this explicit runtime/retention loop
surface, cron evaluator, cron repository state, and caller-driven cron
scan/wait/execute-due/run surface plus config-authored cron seeds, explicit
seed application/service-cycle policy, run history, stop policy, typed
outcomes, stored cron execution leases, stored cron agent leases, triggered
descriptor intake, triggered execution/run history, triggered lifecycle hooks,
triggered agent leases, bounded triggered queue/backpressure, and explicit
one-at-a-time queue draining: detached service startup policy, notifier
routing, blocked-agent hold/drop policy, and actual agent firing for
agent-facing jobs.

## Validation

Focused checks for this boundary:

```sh
xmake build oran-automation
xmake build test-automation && xmake run test-automation
xmake build bench-automation && xmake run bench-automation
```

Slice 187 reports `test-automation` at 7 cases / 40 assertions. Slice 188 adds
bootstrap coverage for config-to-job mapping and assembly descriptor storage,
reported under `test-bootstrap`. Slice 189 reports `test-automation` at 12
cases / 110 assertions for migration idempotence, job round-trips, policy/state
updates, run recording/listing, and repository validation. Slice 190 reports
`test-automation` at 16 cases / 169 assertions for not-due ticks, due-success
backend execution, backend-failure run recording without advancing state, and
invalid/missing job handling. Slice 191 reports `test-automation` at 18 cases /
207 assertions for successful periodic `memory_decay` publishing and advisory
sink-failure reporting from the explicit tick owner. Slice 192 reports
`test-automation` at 22 cases / 245 assertions for runtime open/migration,
already-migrated reopen, empty-path validation, and retention-service creation
over the owned repository state. Slice 193 reports `test-automation` at 26
cases / 274 assertions for the caller-started loop step's wait-budget skip,
within-budget wait/run, cancellation while waiting, and negative-budget
validation. Slice 194 reports `test-automation` at 27 cases / 327 assertions
for retention job lifecycle publishing on not-due, due-success, and backend
failure paths. Slice 195 reports `test-automation` at 30 cases / 390 assertions
for retention lease migration/repository acquisition semantics, loop active-lease
conflicts, due-run release, cancellation-while-waiting without held leases, and
backend-failure release, plus lease input validation. Slice 196 reports
`test-automation` at 33 cases / 429 assertions for finite loop backlog
catch-up, no-due-work stopping, and loop-policy input validation. Slice 197
reports `test-automation` at 41 cases / 467 assertions for cron exact/future
fires, stored-state advancement, steps/lists/ranges, DOM/DOW OR semantics, and
malformed-expression validation.
Slice 198 reports `test-automation` at 44 cases / 515 assertions for cron
repository migration v3, round-trip/update/list/mark-fired behavior, missing
mutation errors, cron validation, and `AutomationRuntime::open(...)` migration
report version 3.
Slice 199 reports `test-automation` at 49 cases / 568 assertions for
caller-driven cron service scans, explicit cron wait steps, cancellation while
waiting, invalid scan policy, and `AutomationRuntime` cron factories.
Slice 200 reports `test-automation` at 52 cases / 618 assertions for explicit
cron due execution, success-only state advancement, handler failure retry state,
not-due handler skipping, and invalid execution policy validation.
Slice 201 reports `test-automation` at 54 cases / 663 assertions for finite cron
loop backlog catch-up, success-only state advancement through the loop, handler
failure stopping without immediate retry, and cron run-policy input validation.
Slice 202 reports `test-automation` at 56 cases / 730 assertions for advisory
cron job lifecycle metadata on handler success and handler failure.
Slice 204 reports `test-automation` at 57 cases / 747 assertions for explicit
cron seed application through `AutomationRuntime`, including update and failure
context coverage; `test-bootstrap` reports 129 cases / 1087 assertions for the
cross-boundary assembly-to-runtime seed application path.
Slice 205 reports `test-automation` at 59 cases / 768 assertions for the
caller-awaited cron service cycle, including seed-apply-plus-loop execution and
validation-before-seed-apply coverage.
Slice 206 reports `test-automation` at 60 cases / 810 assertions for cron run
history, including migration v4, repository record/list validation, success and
failure run rows from explicit due execution, not-due run suppression, and
`AutomationRuntime::open(...)` migration report version 4.
Slice 207 reports `test-automation` at 63 cases / 854 assertions for
cooperative cron loop stop policy, covering stop-before-work, stop-after-one
successful execution, and runtime service-cycle pass-through of the same
predicate.
Slice 208 reports `test-automation` at 64 cases / 893 assertions for cron run
outcome classification, covering migration v5, success/failure/aborted
repository round-trips, missing aborted error validation, cancelled-handler
classification as `aborted`, and `AutomationRuntime::open(...)` migration
report version 5.
Slice 209 reports `test-automation` at 67 cases / 954 assertions for cron
execution leases, covering migration v6, repository acquire/conflict/expired
takeover/release behavior, service-level lease conflict before handler
execution, release after durable success, loop default lease ownership, and
`AutomationRuntime::open(...)` migration report version 6.
Slice 210 reports `test-automation` at 70 cases / 1010 assertions for cron
agent leases, covering migration v7, cron job `agent_key` round-trips, repository
agent lease acquire/conflict/expired-takeover/release behavior, service-level
same-agent conflict before handler execution, loop default agent lease
ownership, and `AutomationRuntime::open(...)` migration report version 7.
`test-config` reports 51 cases / 462 assertions for optional cron seed
`agent_key` parsing, and `test-bootstrap` reports 129 cases / 1091 assertions
for mapping it into stored cron seed descriptors.
Slice 211 reports `test-automation` at 75 cases / 1078 assertions for triggered
intake, covering migration v8, triggered descriptor round-trip/update/listing,
repository validation, service-level intake matching and validation, runtime
factory coverage, and `AutomationRuntime::open(...)` migration report version
8.
Slice 212 reports `test-automation` at 79 cases / 1178 assertions for triggered
execution history, covering migration v9, triggered run record/list APIs,
success/failure/aborted handler-attempt recording, invalid execution policy
validation, runtime factory execution coverage, and `AutomationRuntime::open(...)`
migration report version 9.
Slice 213 reports `test-automation` at 81 cases / 1246 assertions for triggered
lifecycle hooks, covering advisory `job_started`/`job_finished` metadata on
handler success and `job_started`/`job_failed` metadata on handler failure
through `TriggeredService::execute(...)`.
Slice 214 reports `test-automation` at 84 cases / 1302 assertions for triggered
agent leases, covering migration v10, repository acquire/conflict/expired
takeover/release behavior, service-level same-agent conflict before handlers,
durable success/failure release, invalid lease input validation, and
`AutomationRuntime::open(...)` migration report version 10.
Slice 215 reports `test-automation` at 87 cases / 1376 assertions for the
caller-owned triggered queue, covering matched job enqueue/receive behavior,
drop-newest overflow, `job_dropped` metadata publishing, run-row suppression for
queued/dropped work, and invalid enqueue policy validation. `test-hook` reports
38 cases / 313 assertions for the public `JobDroppedPayload` delivery surface.
Slice 216 reports `test-automation` at 89 cases / 1409 assertions for
one-at-a-time triggered queue draining, covering explicit single-descriptor
execution through `TriggeredService::execute_one(...)`, queue `drain_once(...)`
run recording, preservation of later queued descriptors, and invalid drain
policy validation.

`bench-automation` planning rows are:

- `automation.periodic_evaluate_1024` at about 4.49 us / 1024-job batch.
- `automation.memory_retention_plan_1024` at about 15.10 us / 1024-job batch.
