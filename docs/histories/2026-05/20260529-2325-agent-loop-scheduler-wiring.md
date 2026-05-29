## [2026-05-29 23:25] | Task: slice 120 — wire agent::Loop through ToolScheduler + config + benches

### Execution Context

- Agent: Claude Opus 4.8
- Base model: claude-opus-4-8
- Runtime: Claude Code (single-session implementation)
- Linked plan: [`docs/exec-plans/completed/2026-05-27-tool-scheduler-v1.md`](../exec-plans/completed/2026-05-27-tool-scheduler-v1.md)
  — fifth and final slice (116-120) of the tool-scheduler v1 arc, moved to
  `completed/` by this slice. Spec
  [`docs/product-specs/0012-tool-scheduler-and-state.md`](../product-specs/0012-tool-scheduler-and-state.md).

### User Query

> Plan remaining: 119 (cancellation propagation + cancellation_lag), and 120
> (loop wiring + bench + config). continue.

Slice 120 closes the arc: route every production tool batch through
`ToolScheduler::run_batch`, land the `runtime.tool_scheduler.*` config surface,
have `bootstrap::AgentPromptRunner` own the scheduler, and ship the AC12
overhead bench plus the audit-fan-out bench. Closes the full AC7, AC12, and
the rest of AC11.

### Changes Overview

- Areas: `oran-config` (typed surface), `oran-agent` (loop wiring + a scheduler
  cancellation hardening), `oran-bootstrap` (runner ownership), `bench/agent`.
- Key actions:
  - **Loop wiring.** `agent::Loop` replaces its sequential
    `for (use : tool_uses) registry.dispatch(...)` loop with a single
    `scheduler->run_batch(batch, *dispatch_context)` call for every batch
    (including N == 1), so bounded parallelism, per-path locks, per-call
    timeout, and parent-cancellation propagation apply on the ReAct path. The
    ordered batch is converted into `tool_result` blocks with the same
    semantics the sequential path used: a model-repairable per-call error
    becomes a `tool_result` error block; a parent-cancellation batch error or a
    per-call infrastructure error (cancelled / storage / internal, including a
    per-call timeout) ends the turn with `cancellation_phase=tools` via the
    same trace-write handling. `RunTurnInputs` gains `ToolScheduler* scheduler`;
    when null but tools/dispatch_context are present the loop builds a per-turn
    fallback with default options (preserves embedder/test behaviour).
  - **Runner ownership.** `bootstrap::AgentPromptRunner` owns a persistent
    `agent::ToolScheduler` built from the runner executor, the builtin
    registry, and config, and threads `&scheduler_` into every turn.
  - **Config surface.** `oran-config` parses
    `runtime.tool_scheduler.{max_parallel_tools, per_call_timeout_ms,
    idle_lock_ttl_ms}` (defaults 4 / 60000 / 300000) into
    `config::ToolSchedulerRuntimeConfig`; `config.example.json` documents it;
    bootstrap converts it to `agent::ToolSchedulerOptions`.
  - **Approval-token replay under the scheduler.** `make_per_call_context`
    threads the prototype's `approval_token_output` only for a single-call
    batch (no concurrency → no race); a parallel batch drops it because one
    output slot cannot disambiguate N concurrently issued tokens. This keeps
    the loop's blocking-ask-approval replay working for N == 1.
  - **Queued-cancel hardening.** The scheduler's semaphore `!acquired`
    early-return now resets its cancellation filter before sending its
    completion (matching the lock-fail path), so a call cancelled while queued
    reports cleanly instead of dropping its completion (`Channel::send`
    short-circuits to `cancelled` while the slot is armed) and being mis-named
    a `cancellation_lag` laggard by the slice-119 grace drain.
  - **Benches.** `bench/agent/scheduler_overhead` (A = direct
    `Registry::dispatch` of a no-op, B = single-call `run_batch`) and
    `scheduler_audit_fanout` (A = 8-call batch over `NullAuditSink`, B = over
    `StorageAuditSink` on an in-memory `Pool`).

### Design Intent

Three decisions shaped the wiring:

1. **One dispatch path, with a per-turn fallback.** The plan calls for the loop
   to route every batch through the scheduler and for the runner to own one.
   Requiring `inputs.scheduler` would have churned every existing loop tool-use
   test. Instead the loop uses `inputs.scheduler` when supplied (production) and
   otherwise lazily builds a per-turn scheduler with default options — same
   `run_batch` code path, zero behaviour change for callers that do not supply
   one. The runner-owned scheduler is the persistent one the spec's lock-stats /
   reap story wants; the fallback is functionally complete because v1 turns are
   sequential (the lock table only matters within a batch).

2. **`approval_token_output` is single-call only.** Slice 116 nulled
   `approval_token_output` per call so concurrent dispatches do not race on one
   output pointer. That dropped the loop's blocking-ask replay (the registry
   stores a freshly issued token there). Production never set the field, but a
   loop test pinned it. Threading it for N == 1 only (no concurrency) restores
   the behaviour without reintroducing the race a parallel batch would have.

3. **Queued-cancel must not look like a lag.** Slice 119 names any call that
   misses the 100 ms grace window. Tracing `run_call` showed the semaphore
   `!acquired` arm sent its completion with cancellation still armed, so
   `Channel::send` short-circuited and the completion was dropped — a queued
   call cancelled before running would have been mis-named a `cancellation_lag`
   laggard (or, before slice 119, hung the drain). Resetting the filter first
   (matching the existing lock-fail arm) makes the queued call report cleanly.

The AC12 bench reads as a large B/A ratio (≈ 2.8×) only because a no-op
`NullAuditSink` dispatch is itself ≈ 2.4 µs; the scheduler's fixed per-batch
overhead is ≈ 4.3 µs, so a single-call `run_batch` is ≈ 6.7 µs — an order of
magnitude under spec 0002's ≤ 75 µs scheduler allowance (the plan's reading of
"≤ 1.5× the ≤ 50 µs single-call ceiling"). For a realistic tool it is
negligible. The audit-fan-out bench uses an in-memory `Pool` so the ratio
reflects writer-strand coordination, not disk fsync.

### Files Modified

- `include/oran/agent/loop.hpp` — forward-declare `ToolScheduler`; add
  `RunTurnInputs::scheduler`.
- `src/oran-agent/loop.cpp` — include `scheduler.hpp`; per-turn fallback
  scheduler; replace the sequential dispatch loop with `run_batch` + unified
  tool-phase error handling.
- `src/oran-agent/scheduler.cpp` — single-call `approval_token_output`
  threading (`make_per_call_context` + `BatchState::thread_approval_token_output`);
  reset cancellation before the semaphore `!acquired` completion send.
- `include/oran/config/config.hpp`, `src/oran-config/config.cpp` —
  `ToolSchedulerRuntimeConfig` + `runtime.tool_scheduler` parsing.
- `config.example.json` — documented `tool_scheduler` block.
- `src/oran-bootstrap/prompt_runner.cpp` — `scheduler_options_from`;
  runner-owned `ToolScheduler scheduler_`; `inputs.scheduler = &scheduler_`.
- `tests/config/test_config.cpp` — extract / defaults / reject cases + example
  assertions.
- `tests/agent/test_loop.cpp` — caller-supplied-scheduler honoured case.
- `tests/agent/test_scheduler.cpp` — queued-cancel-not-mis-named case.
- `bench/agent/scenarios/scheduler_overhead.cpp`,
  `bench/agent/scenarios/scheduler_audit_fanout.cpp`, `bench/agent/main.cpp`,
  `bench/agent/README.md`.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 120; last-history; `Active exec-plan` → none (arc
  complete, plan moved to `completed/`); AC closure; `test-config`
  33 / 241 → 36 / 258, `test-agent` 45 / 10 607 → 47 / 10 618, `test-bootstrap`
  unchanged 72 / 316; snapshot prose gains a slice-120 paragraph.
- `docs/ARCHITECTURE.md` — `oran-agent` row records the scheduler wired through
  the loop + runner ownership + config; the data-flow note names the scheduler
  as the tool-dispatch entry point.
- `docs/design-docs/tool-runtime.md` — "Scheduler Boundary" promoted from
  forward-looking to the shipped, loop-wired contract.
- `docs/design-docs/async-model.md` — the `ToolScheduler` bullet records the
  loop wiring as shipped.
- `docs/product-specs/0012-tool-scheduler-and-state.md` — closing status note.
- `docs/QUALITY_SCORE.md` — `oran-agent` row revisited for the scheduler arc.
- `docs/releases/feature-release-notes.md` — user-visible parallel tool dispatch.
- `docs/exec-plans/active/2026-05-27-tool-scheduler-v1.md` → moved to
  `docs/exec-plans/completed/`; slice-120 + progress bullets checked off.

### Validation

- Commands run:
  - `xmake build oran-config oran-agent oran-bootstrap orangutan` — clean.
  - `./build/.../test-config` — **258 assertions / 36 cases.**
  - `./build/.../test-agent` — **10 618 assertions / 47 cases.**
  - `./build/.../test-bootstrap` — **316 assertions / 72 cases.**
  - `xmake build bench-agent && xmake run bench-agent` — overhead
    direct ≈ 2.4 µs, run_batch ≈ 6.7 µs (≤ 75 µs allowance, AC12); audit fan-out
    null ≈ 44 µs, storage ≈ 134 µs (≈ 11 µs/audit-row, no starvation).
  - `make ci` — docs + STATUS freshness gate.
- Tests added/changed: `test-config` +3 cases; `test-agent` +2 cases
  (caller-supplied scheduler, queued-cancel naming); 47 cases total.
- Bench impact: two new `bench-agent` scenarios (above). Existing prompt-cache
  scenario unchanged.
- Compile-budget delta: `loop.cpp` gains the `scheduler.hpp` include and the
  batched-dispatch body; `scheduler.hpp` is forward-decl-heavy so `loop.hpp`
  only sees a pointer. Within the `oran-agent` budget.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none. The tool-scheduler v1 arc (116-120) is complete; the
  spec's v1.1 items (dispatch singleflight, persisted index caches) and a
  periodic `reap_idle_locks` tick remain future specs/slices.
- Linked release note: `docs/releases/feature-release-notes.md` — parallel tool
  calls now run through the scheduler on the production ReAct path.
