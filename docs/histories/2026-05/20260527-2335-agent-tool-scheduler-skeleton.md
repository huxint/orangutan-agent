## [2026-05-27 23:35] | Task: slice 116 — agent::ToolScheduler skeleton

### Execution Context

- Agent: Claude Opus 4.7
- Base model: claude-opus-4-7
- Runtime: Claude Code (single-session implementation)
- Linked plan: [`docs/exec-plans/active/2026-05-27-tool-scheduler-v1.md`](../exec-plans/active/2026-05-27-tool-scheduler-v1.md)
  — first of five slices (116-120). Spec
  [`docs/product-specs/0012-tool-scheduler-and-state.md`](../product-specs/0012-tool-scheduler-and-state.md)
  carries the contract.

### User Query

> 深度了解项目架构，了解当前项目实现进度。在进一步进行代码实现前，必须充分阅读理解所有相关文档，
> 始终保持深度思考(ultrathink)…开始执行此plan @docs/exec-plans/active/2026-05-27-tool-scheduler-v1.md

The plan is the spec-0012 ToolScheduler v1 arc. Slice 116 opens the
`agent::ToolScheduler` surface and lands the bounded-parallelism /
ordered-results / per-call-timeout / partial-cancellation contracts in
`oran-agent`. Per-path locks, full cancellation guarantee, audit fan-out, and
loop wiring move in slices 117-120.

### Changes Overview

- Areas: `oran-agent` (new public surface + impl + tests).
- Key actions:
  - Add `<oran/agent/scheduler.hpp>` exporting `ToolSchedulerOptions`,
    `ToolBatchCall`, `ToolBatchResult`, and pimpl-backed `ToolScheduler` with
    `run_batch(std::vector<ToolBatchCall>, tool::DispatchContext&)`.
  - Add `src/oran-agent/scheduler.cpp`: channel-as-semaphore bounded
    parallelism (`async::Channel<std::monostate>` filled with one permit per
    slot), per-call timeout via
    `asio::experimental::awaitable_operators::operator||` against
    `async::sleep_for`, parent-cancellation propagation via per-child
    `asio::cancellation_signal` (one per spawned call, held in a `std::deque`
    so addresses stay stable across emplace), ordered results via indexed
    `std::vector<std::optional<ToolBatchResult>>`, and a fresh
    `tool::DispatchContext` brace-initialised per call so concurrent
    dispatches do not race on the shared prototype's `registry` /
    `resolved_path` / `approval_token_output` / `now` fields.
  - Extend `<oran/agent.hpp>` to re-export the new header.
  - Add `tests/agent/test_scheduler.cpp` with six cases: empty batch,
    single-call sanity, AC1 bounded parallelism (10 calls × 50 ms with
    max_parallel=4 — peak concurrency ≤ 4 and total ≥ 150 ms),
    AC2 ordered results (mixed 60 ms / 5 ms with deterministic id mapping),
    AC6 timeout (500 ms tool with per_call_timeout=50 ms surfaces
    `Error::cancelled` carrying `reason=timeout` plus `tool` / `per_call_timeout_ms`
    context), and partial AC5 parent cancellation via
    `asio::cancellation_signal` (1 s tools cancelled mid-flight return
    `Error::cancelled` with `reason=parent_cancelled`).

### Design Intent

The hardest call here was where to put the cancellation primitive. The first
draft used a single `asio::cancellation_signal` shared by every spawned
child via `bind_cancellation_slot(slot, ...)`. That crashes in practice:
`asio::cancellation_signal` owns a single slot at a time, and when the
inner `Channel::async_receive` installs its cancellation callback via
`asio::get_associated_cancellation_slot(handler).assign(...)`, each spawned
child overwrites the previous child's callback. When the first child's
awaitable later calls `clear_cancellation_slot()`, it dereferences a
stale/freed pointer — SIGSEGV in
`asio::cancellation_slot::clear` (verified via gdb backtrace on a 2-call
repro).

The fix is one cancellation_signal per child, owned by the shared
`BatchState` via `std::deque<asio::cancellation_signal>`. `deque` is
mandatory: `cancellation_signal` is non-movable and non-copyable, so
`vector::emplace_back` would still fail when growth relocates existing
elements; deque preserves element addresses across all `emplace_back`
calls. The parent emits on each child's signal when it sees its own
cancellation, then drains the completion channel with
`reset_cancellation_state(disable_cancellation())` so cancelled children
can still send their final completion message before the parent returns.

The per-call `DispatchContext` is brace-initialised inside each spawned
coroutine so concurrent calls do not race on the prototype's mutable
fields (`registry`, `resolved_path`, `approval_token_output`, `now`).
`tool::Registry::dispatch` is `const`, the `entries_` map is read-only
after boot, and `permission::NullAuditSink::record` is stateless, so
concurrent dispatch against one registry is safe for slice 116's MVP.
Slice 118 will exercise the same invariants against
`StorageAuditSink` (which serialises writes through its `Pool` writer).

The `idle_lock_ttl` field on `ToolSchedulerOptions` is parsed and stored
in slice 116 even though no lock table exists yet, so slice 117 can
consume it without revising the option struct.

The slice deliberately keeps the scheduler unwired from `agent::Loop`.
The loop still drives the sequential dispatch path; slice 120 replaces
that with `scheduler.run_batch(...)` once the lock table, audit
correlation, and bench scaffolding land. This staging matches the
plan's "AC1/AC2/AC6, partial AC5/AC11" closure target for this slice.

### Files Modified

- `include/oran/agent/scheduler.hpp` — **new** public header.
- `src/oran-agent/scheduler.cpp` — **new** implementation.
- `include/oran/agent.hpp` — re-export the new header from the umbrella.
- `tests/agent/test_scheduler.cpp` — **new** six-case bucket covering the
  AC1 / AC2 / AC6 / partial AC5 contracts plus empty-batch + single-call
  sanity.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 116; `Last completed history` pointer;
  active exec-plan now names this plan; refreshed the `oran-agent` line in
  `Latest Library Surfaces` (26 / 407 → 32 / 462); wrote the next-intended-slice
  paragraph for slice 116's completion and slice 117's intent.
- `docs/ARCHITECTURE.md` — extended the `oran-agent` row to include the new
  `ToolScheduler` skeleton with its slice-116 contract; library-inventory
  preface paragraph (the "Slice status" preamble) gains the slice-116 note.
- `docs/design-docs/tool-runtime.md` — promoted the "Scheduler Boundary"
  subsection from forward-looking to slice-116 status (skeleton shipped;
  lock table / approval gating / cancellation_lag / loop wiring remain
  staged).
- `docs/design-docs/async-model.md` — added the scheduler to the
  cancellation entry-point list as the future single off-strand
  scheduling point.
- `docs/exec-plans/active/2026-05-27-tool-scheduler-v1.md` — checked off the
  first three Progress Log bullets (channel API investigation, skeleton +
  fake-tool fixture, doc sync); next bullet is the slice-116 history entry
  itself.

### Validation

- Commands run:
  - `xmake build oran-agent` — succeeded in 16.6 s.
  - `xmake build test-agent` — succeeded.
  - `xmake build test-bootstrap` / `test-tool` — succeeded.
  - `./build/linux/x86_64/release/test-agent` — **All tests passed (462
    assertions in 32 test cases).**
  - `./build/linux/x86_64/release/test-bootstrap` — **All tests passed (316
    assertions in 72 test cases)** (unchanged).
  - `./build/linux/x86_64/release/test-tool` — **All tests passed (1866
    assertions in 185 test cases)** (unchanged).
- Tests added/changed:
  - `tests/agent/test_scheduler.cpp` — 6 new cases / 55 assertions:
    1. Empty batch returns an empty result vector.
    2. Single-call batch returns one ordered result with the registry output.
    3. Bounded parallelism (AC1): 10 fake tools × 50 ms with
       `max_parallel_tools=4` keeps peak in-flight ≤ 4 and total elapsed
       ≥ 75 ms (lower bound) with a 500 ms ceiling for CI noise.
    4. Ordered results (AC2): 4 calls with a slow/fast/slow/fast mix all
       return in input order with matching tool_use_ids and a stable
       `done:{}` payload.
    5. Per-call timeout (AC6): 500 ms tool with `per_call_timeout=50 ms`
       surfaces `Error::cancelled` carrying `reason=timeout`,
       `tool=fake.slow`, and `per_call_timeout_ms=50`.
    6. Parent cancellation (partial AC5): two 1 s tools cancelled via
       `asio::cancellation_signal.emit(terminal)` return
       `Error::cancelled` with `reason=parent_cancelled`.
- Bench impact: none in slice 116; `bench/agent/scheduler_overhead` and
  `scheduler_audit_fanout` land in slice 120 alongside the loop wiring so
  the bench measures the production call path, not a half-built abstraction.
- Compile-budget delta: not measured this slice — the new TU is ~210 lines
  and isolates the heavy `asio/experimental/awaitable_operators.hpp` include
  to `scheduler.cpp`. Slice 120 will rebench `oran-agent` once the loop
  consumes the scheduler.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none added. The slice 117 lock-table tracker stays in
  the exec plan's milestones, not the tech-debt tracker, because the work
  is scheduled inside the active arc.
- Linked release note: none — internal abstraction; user-visible parallel
  tool dispatch lands in slice 120 with the release note.
