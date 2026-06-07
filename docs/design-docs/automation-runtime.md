# Automation Runtime

`oran-automation` owns automation scheduling decisions. The shipped surface is
still intentionally narrow: deterministic periodic schedule and POSIX cron
evaluation, long-term memory retention request planning, a bootstrap-owned
mapping from configured retention policy into that job descriptor,
automation-owned retention job/run/lease persistence, durable cron job state, a
caller-owned runtime state handle, and a caller-driven retention service tick
with optional advisory `memory_decay` plus job lifecycle metadata, plus a
caller-started retention loop step that can wait within a caller budget for one
stored job to become due and lease due execution, plus a finite caller-owned
loop policy over that step. It does not start detached background work, own a
process service loop, read cron config, or call an agent loop.

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
construct `MemoryRetentionService` and `MemoryRetentionLoop` instances over the
same stable state. It still does not start timers, acquire leases, publish job
lifecycle hooks itself, wire bootstrap to open `automation.db`, or call agents.

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
spawn detached work, publish cron lifecycle hooks, or call agents.

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
  CronSchedule schedule;
  PeriodicJobState state;
};

struct CronJobRecord {
  std::string job_key;
  CronSchedule schedule;
  PeriodicJobState state;
  std::string created_at;
  std::string updated_at;
};

struct ListCronJobsOptions {
  std::size_t limit;
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

class AutomationRuntime {
 public:
  static async::Awaitable<core::Result<AutomationRuntime>>
  open(asio::any_io_executor executor, AutomationRuntimeOptions);

  std::string_view database_path() const noexcept;
  const storage::MigrationReport& migration_report() const noexcept;
  AutomationRepository& repository() noexcept;
  const AutomationRepository& repository() const noexcept;

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

`job_key` is the durable repository identity. `scope_key` remains the memory
decay scope inside the stored job descriptor, so future different automation
jobs or policies can share a memory scope without overwriting each other. Cron
jobs use the same durable `job_key` identity and store the cron expression
directly because there is not yet a higher-level config/job descriptor type.

Repository calls validate inputs before touching SQLite: empty job keys are
rejected, cron schedules must pass `evaluate_cron_schedule(...)`, retention
jobs must pass the same policy validation used by `plan_memory_retention(...)`,
failed runs require an error message, run finish time must not precede fire
time, list limits must be positive, lease owner keys must be non-empty, and
lease expiry must be after acquisition time. Missing jobs return `std::nullopt`
on `get_cron_job(...)` / `get_memory_retention_job(...)` and
`ErrorKind::not_found` from mutation operations that require an existing job.

`upsert_cron_job(...)` replaces the stored schedule and last-fired state for a
durable `job_key` while preserving `created_at`; `mark_cron_job_fired(...)`
advances only `last_fired_at` and `updated_at`; `list_cron_jobs(...)` returns a
positive-limit bounded vector ordered by most recently updated first. These APIs
are state storage only: they do not evaluate due work, acquire leases, publish
hooks, or call agents.

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
and `memory_retention_service(...)` as a convenience factory for
`MemoryRetentionService` over the owned repository plus a caller-supplied
`memory::longterm::Backend`. It also exposes `memory_retention_loop(...)` as a
convenience factory for the caller-started loop step over the same repository,
backend, and hook options.

The runtime handle is intentionally not a scheduler. It does not sleep, spawn
detached coroutines, acquire process leases, own a hook bus, or decide whether
bootstrap should open `automation.db`.
Bootstrap still only maps the configured retention policy into a descriptor; a
caller must explicitly call `AutomationRuntime::open(...)` before automation
state exists.

## Loop Step Semantics

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
surface, cron evaluator, and cron repository state: cron config ownership,
process service/timer startup policy, broader per-agent/category leases once
agent-facing jobs exist, triggered categories, queueing/backpressure, and
notifier routing for agent-facing jobs.

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

`bench-automation` planning rows are:

- `automation.periodic_evaluate_1024` at about 4.49 us / 1024-job batch.
- `automation.memory_retention_plan_1024` at about 15.10 us / 1024-job batch.
