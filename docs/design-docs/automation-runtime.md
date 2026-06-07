# Automation Runtime

`oran-automation` owns automation scheduling decisions. The shipped surface is
still intentionally narrow: deterministic periodic schedule evaluation,
long-term memory retention request planning, a bootstrap-owned mapping from
configured retention policy into that job descriptor, automation-owned
retention job/run persistence, and a caller-driven retention service tick with
optional advisory `memory_decay` publishing. It does not start background work,
publish job lifecycle hooks, acquire leases, or call an agent loop.

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
primitives for its own schema, and may publish the shared advisory
`memory_decay` hook payload when a caller explicitly supplies a hook bus.
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
in newest-first order. It still does not own a service loop, lease table, memory
backend, hook bus, queue, or cancellation policy for active jobs.

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
bootstrap loop, publish job lifecycle hooks, acquire leases, or call agents.

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

core::Result<MemoryRetentionPlan>
plan_memory_retention(MemoryRetentionJob, PeriodicJobState, core::Time now);

struct AutomationRepositoryOptions {
  std::string migrations_directory;
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

class AutomationRepository {
 public:
  explicit AutomationRepository(storage::Pool&, AutomationRepositoryOptions = {});

  async::Awaitable<core::Result<storage::MigrationReport>> migrate();
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
generic storage pool. The first migration creates:

- `automation_memory_retention_jobs`, keyed by durable `job_key`, with
  `scope_key`, retention policy fields, `first_fire_at`, nullable
  `last_fired_at`, and timestamps.
- `automation_memory_retention_runs`, keyed by autoincrement `id`, with a
  foreign key to `job_key`, scheduled/finished timestamps, success flag,
  shadowed count, optional error message, and newest-first listing index.

`job_key` is the durable repository identity. `scope_key` remains the memory
decay scope inside the stored job descriptor, so future different automation
jobs or policies can share a memory scope without overwriting each other.

Repository calls validate inputs before touching SQLite: empty job keys are
rejected, retention jobs must pass the same policy validation used by
`plan_memory_retention(...)`, failed runs require an error message, run finish
time must not precede fire time, and list limits must be positive. Missing jobs
return `std::nullopt` on `get_memory_retention_job(...)` and
`ErrorKind::not_found` from `mark_memory_retention_fired(...)`.

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
successful due tick publishes advisory `hook::Event::memory_decay` after the
run row is recorded and `last_fired_at` advances. The payload reuses the planned
`DecayRequest` values (`scope_key`, `unused_before`, `importance_floor`,
`limit`, `decay_at`), copies the configured source/identity fields, and reports
the number of shadowed records. Because the tick remains caller-clocked and does
not own a separate wall-clock span, the periodic payload uses `decay_at` for
both `started_at` and `finished_at` and reports `duration=0`. Advisory sink
errors are summarized in `MemoryRetentionTickResult::hook_publish` and do not
roll back the durable run.

If the backend returns an error, the tick records a failed run with the scheduled
fire time, caller-supplied `now` as `finished_at`, zero shadowed records, and a
non-empty error message. It then returns the backend error and does not advance
`last_fired_at`. If that best-effort failure-row write itself fails, the
repository error is returned because the run outcome was not durably recorded.

The public awaitable is composed entirely of repository/backend awaitables and
the optional advisory hook publish; it does not add its own blocking wait or
detached coroutine. Future service-loop ownership can add timers, leases,
shutdown, and cancellation policy around this tick without moving those concerns
into bootstrap or `oran-memory`.

## Future Ownership

The next automation slices can build on this boundary in this order:

1. Publish job lifecycle hooks from the actual periodic service owner.
2. Add per-agent leases and cancellation semantics around service-loop-owned
   runs.
3. Add cron and triggered job categories.
4. Add queueing/backpressure and notifier routing for agent-facing jobs.

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
sink-failure reporting from the explicit tick owner. The local
`bench-automation` planning rows are:

- `automation.periodic_evaluate_1024` at about 4.84 us / 1024-job batch.
- `automation.memory_retention_plan_1024` at about 14.84 us / 1024-job batch.
