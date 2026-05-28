# Tool Scheduler v1

## Goal

Land [`product-specs/0012-tool-scheduler-and-state.md`](../../product-specs/0012-tool-scheduler-and-state.md)
v1: `agent::ToolScheduler` owning bounded parallelism, per-path read/write
locks, ordered results, per-call timeout, and parent-cancellation
propagation between `agent::Loop` and `tool::Registry`. The arc ends with
`agent::Loop` driving every tool batch through the scheduler in production
(via `bootstrap::AgentPromptRunner`) and the registry remaining
single-threaded. AC1–AC7, AC10, and AC12 close inside this plan; AC11
(≥ 90% matrix coverage) accumulates across slices. AC8 and AC9
(`BoundedCache` invariants + stats) already shipped via slice 44's
`core::BoundedCache`; this arc only adds new consumers, not new
cache primitives.

## Scope

- In scope:
  - New public surface `<oran/agent/scheduler.hpp>` with `ToolScheduler`,
    `ToolSchedulerOptions`, `ToolBatchCall`, `ToolBatchResult`.
  - Channel-as-semaphore bounded parallelism using existing
    `async::Channel<std::monostate>` (no new `oran-async` public type).
  - Per-canonical-path read/write lock table classified from
    `core::ToolDef::required_capabilities`; TTL-based idle reap.
  - Per-call timeout via `asio::experimental::awaitable_operators::operator||`
    against `async::sleep_for`; surfaces `Error::cancelled` with
    `reason=timeout`.
  - Parent-cancellation propagation: parent cancel ends every in-flight
    call within 100 ms; tools that ignore the cancellation slot are
    audited with `error_kind=cancellation_lag` once the per-call timeout
    fires.
  - Audit/hook-bus invariants preserved end-to-end: exactly N audit rows
    and exactly N `tool_after` publishes per N-call batch, slice-67
    same-row usage enrichment scoped by `parent_turn_id` even when two
    parallel calls hit the same `(tool, identity, input_hash)` triple.
  - `config.runtime.tool_scheduler.{max_parallel_tools,
    per_call_timeout_ms, idle_lock_ttl_ms}` parsed by `oran-config`,
    threaded through `bootstrap::RuntimeAssembly` /
    `AgentPromptRunner`, with documented defaults (`4`, `60000`,
    `300000`) matching the spec.
  - `agent::Loop` final wiring: replace today's sequential
    `for (use : tool_uses) registry.dispatch(...)` with
    `scheduler.run_batch(...)`; loop calls the scheduler for *every*
    batch (including N=1) so there is one code path.
  - `bench/agent/scheduler_overhead`: A-vs-B comparing batched dispatch
    overhead against single-call direct dispatch; AC12 target ≤ 1.5×.
  - `bench/agent/scheduler_audit_fanout`: 8-call batch over
    `StorageAuditSink` confirming the audit `Pool` writer is not
    starved (spec risk).
  - Docs in sync: `docs/STATUS.md`, `docs/ARCHITECTURE.md`
    (library inventory + dataflow), `docs/design-docs/tool-runtime.md`
    (Scheduler Boundary subsection), `docs/design-docs/async-model.md`
    (third-party concurrency entry point), `docs/product-specs/0012-*.md`
    status notes, `docs/QUALITY_SCORE.md` (oran-agent row),
    `docs/exec-plans/tech-debt-tracker.md` (closes the singleflight
    regression-test row when slice 119's cancellation tests touch the
    relevant code path; otherwise stays open).
- Out of scope:
  - Singleflight by `(tool_name, input_hash)` on dispatch (spec 0012 v1.1
    — separate exec plan).
  - Index caches under `<workspace>/.orangutan/cache/indexes/` (spec 0012
    v1.1).
  - Cost-aware scheduling, inter-agent fairness, dedicated CPU-class
    executor (spec 0012 v2).
  - A dedicated `async::Semaphore` public type in `oran-async`. The
    channel-as-semaphore pattern stays private to `scheduler.cpp` until
    a second consumer exists (`BoundedCache` over-generalisation risk
    cited in the spec).
  - Modifications to `tool::Registry` to take internal locks — the
    registry stays single-threaded per the spec.
  - Loop-side ReAct changes beyond replacing the sequential dispatch
    line with a `run_batch` call.

## Context

- Relevant docs:
  - [`product-specs/0012-tool-scheduler-and-state.md`](../../product-specs/0012-tool-scheduler-and-state.md)
    — spec; 12 acceptance criteria.
  - [`design-docs/tool-runtime.md`](../../design-docs/tool-runtime.md)
    "Scheduler Boundary" — the registry stays single-threaded; the
    scheduler sits between provider and registry.
  - [`design-docs/async-model.md`](../../design-docs/async-model.md)
    — one executor; cancellation is universal.
  - [`rules/async-and-concurrency.md`](../../rules/async-and-concurrency.md)
    A1–A14 — no `std::thread`, bounded queues by default, no blocking
    calls on the executor thread, timer cancellation is checked,
    coroutine lifetime by-value capture.
  - [`rules/critical-rules.md`](../../rules/critical-rules.md) C2, C3,
    C8, C11, C17 — concurrency vocabulary, `Result<T>` boundaries,
    RAII, cancellation, modern C++.
  - [`rules/compile-budget.md`](../../rules/compile-budget.md) —
    `oran-agent` p95 3.0 s / hard cap 3.5 s.
- Relevant code paths:
  - `include/oran/agent/loop.hpp`, `src/oran-agent/loop.cpp` — current
    sequential dispatch path lives at `loop.cpp:572` inside
    `for (const auto& use : tool_uses)`. The scheduler replaces that
    loop body.
  - `include/oran/tool/registry.hpp`, `src/oran-tool/registry.cpp` —
    `Registry::dispatch` stays untouched; the scheduler owns
    concurrency at the call-graph boundary.
  - `include/oran/async/channel.hpp` — existing
    `async::Channel<std::monostate>` is the parallelism primitive.
  - `include/oran/async/sleep.hpp` — `async::sleep_for` for the
    timeout race.
  - `include/oran/bootstrap/prompt_runner.hpp`,
    `src/oran-bootstrap/prompt_runner.cpp` —
    `AgentPromptRunner` owns assembly services; will own the
    `ToolScheduler` and thread it into `RunTurnInputs`.
  - `include/oran/config/runtime.hpp` etc. — typed config surface for
    `runtime.tool_scheduler.*`.
- Constraints:
  - `oran-agent` compile-budget category p95 3.0 s. `scheduler.cpp` is a
    new TU; keep it ≤ 1.2 s median by isolating
    `asio::experimental::*` includes there and forward-declaring in
    the public header.
  - No new third-party dependency.
  - `oran-agent` must not gain a dependency on `oran-hook` —
    `tool::Registry` already owns hook publishing.
- Compile-budget impact (if any):
  - One new ~400-line TU in `oran-agent`. Estimated ~1.0 s on the
    reference hardware. No public-header weight added: scheduler header
    forward-decls `tool::Registry`, `tool::Output`, and uses
    `core::Result` only.
  - One new TU in `tests/agent/test_scheduler.cpp` (Catch2). Estimated
    ~1.5 s.
  - No bench TU added until slice 120.

## Risks

- Risk: **Premature distribution.** Spec 0012 calls this out — a
  scheduler before `oran-agent` ships risks an unused abstraction.
  Mitigation: `oran-agent` already exists (slice 72+); slice 120
  wires `agent::Loop` to call `run_batch` for every batch in
  production, so the abstraction has a real caller before this arc
  closes.
- Risk: **Lock-table memory leak.** A pathological agent touches a
  fresh path per call and the lock table grows. Mitigation: slice 117
  TTL-reaps idle rows on a periodic tick and asserts AC10 (≤ 100 rows
  after 10 000 distinct paths + one reap at `idle_lock_ttl + 1 s`).
- Risk: **Cancellation lag in handlers.** A handler that does not poll
  its cancellation slot blocks the scheduler's 100 ms guarantee.
  Mitigation: per-call timeout fires regardless (slice 116);
  `cancellation_lag` audit kind names the offending tool (slice 119).
- Risk: **Audit fan-out on parallel batches.** N parallel calls means N
  `AuditSink::record` calls in flight against the storage writer.
  Mitigation: slice 120's `scheduler_audit_fanout` bench targets an
  8-call batch and fails if the writer becomes the bottleneck;
  `parent_turn_id` scoping (slice 79) and the audit row matcher already
  prevent enrichment cross-talk.
- Risk: **Channel-as-semaphore API drift.** If `async::Channel` does
  not expose the `try_send`/`receive` pair we need, the fallback is
  `asio::experimental::concurrent_channel` (still asio, no new dep).
  Mitigation: investigate API before writing `scheduler.cpp`
  (memory: "verify before retrying").
- Risk: **Loop test churn.** `tests/agent/test_loop.cpp` is 65 KB and
  may need broad updates when slice 120 wires through the scheduler.
  Mitigation: keep the loop's external observable behavior identical
  (ordered transcript suffix, audit rows, trace rows, error semantics)
  so existing assertions pass unchanged; new tests target scheduler
  behavior under `tests/agent/test_scheduler.cpp`, not loop tests.
- Risk: **Compile budget on `loop.cpp`.** `loop.cpp` is already 30 KB.
  Adding scheduler usage must not push its TU time past the
  `oran-agent` hard cap. Mitigation: scheduler header is forward-decl
  heavy; loop only sees a pointer-or-reference to `ToolScheduler`,
  with the call body forwarding to `scheduler.cpp`.

## Milestones

1. **Slice 116 — Skeleton.** `ToolScheduler::run_batch` with
   channel-semaphore bounded parallelism, ordered results, per-call
   timeout, parent cancellation. Fake-tool fixture in
   `tests/agent/test_scheduler.cpp`. Closes AC1, AC2, AC6, partial
   AC5, partial AC11.
2. **Slice 117 — Per-path lock table.** Read/write classification from
   `ToolDef::required_capabilities`; shared/exclusive locks keyed by
   `DispatchContext::resolved_path`; TTL reap on idle. Lock-table
   `Stats` snapshot exposed for `--explain-rules`-style consumers.
   Closes AC3, AC4, AC10.
3. **Slice 118 — Approval + audit + hook fan-out correctness.** Ask
   path short-circuits a batch slot; verify slice-67 same-row audit
   enrichment under parallel execution; verify exactly N audit rows
   and N `tool_after` publishes per N-call batch with no cross-talk.
   Closes most of AC7.
4. **Slice 119 — Cancellation propagation + `cancellation_lag`.**
   Parent cancel ⇒ every in-flight call ends within 100 ms;
   `error_kind=cancellation_lag` audit field added; tool handlers that
   don't poll get named. Closes AC5.
5. **Slice 120 — Loop wiring + bench + config.** Replace sequential
   dispatch in `agent::Loop` with `scheduler.run_batch`;
   `bootstrap::AgentPromptRunner` constructs and owns the scheduler;
   `runtime.tool_scheduler.*` config landed; `bench/agent/scheduler_overhead`
   and `bench/agent/scheduler_audit_fanout` shipped. Closes the full AC7,
   AC12, and the rest of AC11.

## Validation

- Commands:
  - `xmake build oran-agent` per slice (budget check).
  - `xmake run test-agent` — incremental case count grows each slice;
    final target ≥ 38 cases / ≥ 500 assertions.
  - `xmake build bench-agent && xmake run bench-agent
    scheduler_overhead` — slice 120 only; assert ≤ 1.5× single-call
    dispatch overhead (~50 µs ceiling from spec 0002).
  - `xmake run bench-agent scheduler_audit_fanout` — slice 120 only.
  - `make ci` — docs + STATUS freshness gate.
- Manual checks:
  - For each slice: `docs/STATUS.md` bumped (Slice, last-history,
    test counts, tech-debt rows); matching history under
    `docs/histories/2026-05/`.
  - Slice 120: `docs/ARCHITECTURE.md` data-flow section reflects the
    scheduler entry point; `docs/design-docs/tool-runtime.md`
    "Scheduler Boundary" promoted from "forward-looking" to current.
- Observability checks:
  - Slice 117 lock table exposes a `Stats` snapshot
    (`max_held`, `current_held`, `lru_evictions` style — TBD on
    naming) consumable by `--explain-rules`-style debug surfaces
    before `oran-log` exists.
  - Audit metadata under `metadata_json.scheduler` (or extend the
    existing `usage` block) carries `parallelism`,
    `path_lock_wait_ms`, and `timed_out` for each call. Final field
    layout decided in slice 120 alongside the bench.
- Bench comparison (if perf-relevant):
  - `scheduler_overhead`: A = direct `Registry::dispatch` of a no-op
    tool, B = single-call `ToolScheduler::run_batch` of the same
    no-op tool. Spec 0002's single-call ceiling is ≤ 50 µs; scheduler
    overhead allowed up to ≤ 75 µs (1.5×).
  - `scheduler_audit_fanout`: 8 calls of a fake mutating tool against
    `StorageAuditSink` writing through the `Pool`. Report
    p50/p95 wall time and writer queue depth.

## Progress Log

- [x] Slice 116: investigate `async::Channel<std::monostate>` API for
      `try_send`/`receive` (verified — the channel exposes both; the
      semaphore pattern fills capacity with `try_send` and acquires with
      `receive`).
- [x] Slice 116: implement skeleton + fake-tool fixture; close AC1,
      AC2, AC6, partial AC5. Key implementation choice: one
      `asio::cancellation_signal` per spawned call held in a
      `std::deque<asio::cancellation_signal>` for stable addresses (the
      type is non-copyable and non-movable). A first draft sharing one
      signal's slot across all children crashed with SIGSEGV in
      `cancellation_slot::clear` because the channel's
      `assign(...)` callback was being overwritten between children.
- [x] Slice 116: **update `docs/STATUS.md`, `docs/ARCHITECTURE.md`
      library inventory + dataflow, `docs/design-docs/tool-runtime.md`
      Scheduler Boundary in the same PR** (`docs/rules/docs-in-sync.md`).
- [x] Slice 116: write history entry; bump test counts on `STATUS.md`
      (`oran-agent`: 26 / 407 → 32 / 462).
- [x] Slice 117: lock table + TTL reap; close AC3, AC4, AC10. Key
      implementation choices documented in slice 117's history entry: the
      lock-table primitive lives in `src/oran-agent/_impl/path_lock_table.hpp`
      (single-strand by contract, mirroring `core::BoundedCache`); read/write
      classification reads `core::ToolDef::required_capabilities` directly
      from the registry (`Capability::write_file`/`edit_file`/`delete_path`
      → exclusive, `read_file`/`list_directory` → shared); lock keys come
      from the scheduler-driven `tool::Workspace::resolve_*` so the
      `derive_lock_key` step matches the registry's own
      pre-resolution canonical path for the common case; FIFO with shared
      fan-out and writer-priority bypass; cancellation during wait
      reconciles a pre-incremented permit so a cancelled waiter cannot
      orphan the queue; `ToolScheduler::lock_stats()` exposes the public
      `ToolSchedulerLockStats` snapshot and `reap_idle_locks(core::Time)`
      drives the TTL sweep.
- [x] Slice 117: docs + history (`docs/STATUS.md`,
      `docs/ARCHITECTURE.md`, `docs/design-docs/tool-runtime.md`
      "Scheduler Boundary" slice-117 status, this progress log, history
      entry under `docs/histories/2026-05/`).
- [ ] Slice 118: audit / hook fan-out invariants; close most of AC7.
- [ ] Slice 118: docs + history.
- [ ] Slice 119: cancellation propagation + `cancellation_lag` audit;
      close AC5.
- [ ] Slice 119: docs + history.
- [ ] Slice 120: loop wiring + bench + config; close AC7 fully, AC12,
      AC11.
- [ ] Slice 120: `bench/agent/README.md` updated; docs + history.
- [ ] Slice 120: `docs/QUALITY_SCORE.md` `oran-agent` row revisited.
- [ ] Slice 120: feature release note in `docs/releases/feature-release-notes.md`
      (user-visible: parallel tool calls now go through the scheduler).
- [ ] Plan moves to `docs/exec-plans/completed/`.

## Decision Log

- 2026-05-27: **Scheduler lives in `oran-agent`, not `oran-tool`.**
  Spec 0012 specifies "lives in `oran-agent` once the lib exists";
  the lib exists as of slice 72. Avoids a future no-behavior-change
  migration slice.
- 2026-05-27: **Channel-as-semaphore over a dedicated
  `async::Semaphore` public type.** Reuses an existing primitive;
  defers introducing a second `oran-async` public type until a second
  consumer exists (matches the spec's `BoundedCache`
  over-generalisation note).
- 2026-05-27: **`agent::Loop` always routes through the scheduler,
  including N=1 batches.** Single code path; matches the spec's
  per-call-invariants wording; the channel-semaphore overhead with no
  contention is sub-microsecond.
- 2026-05-27: **Bench lands in slice 120, not slice 116.** Benching a
  half-built scheduler invites tuning the abstraction against itself
  rather than the production caller.
- 2026-05-27: **No `oran-async::Semaphore` in this arc.** Out of scope
  for v1; revisit when a second caller appears.
- 2026-05-27: **Config tree: `runtime.tool_scheduler.*`.** Sits next
  to the existing `runtime.tool_output` block; symmetric with how
  output caps are scoped.

## Linked Artifacts

- Related design doc:
  [`design-docs/tool-runtime.md`](../../design-docs/tool-runtime.md)
  ("Scheduler Boundary" subsection).
- Related product spec:
  [`product-specs/0012-tool-scheduler-and-state.md`](../../product-specs/0012-tool-scheduler-and-state.md).
- PRs: TBD.
- History entry: one per slice under `docs/histories/2026-05/`.
- Release note: slice 120 adds an entry to
  `docs/releases/feature-release-notes.md`.
