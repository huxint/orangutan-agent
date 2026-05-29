## [2026-05-29 22:30] | Task: slice 119 — ToolScheduler cancellation propagation + cancellation_lag

### Execution Context

- Agent: Claude Opus 4.8
- Base model: claude-opus-4-8
- Runtime: Claude Code (single-session implementation)
- Linked plan: [`docs/exec-plans/active/2026-05-27-tool-scheduler-v1.md`](../exec-plans/active/2026-05-27-tool-scheduler-v1.md)
  — fourth of five slices (116-120). Spec
  [`docs/product-specs/0012-tool-scheduler-and-state.md`](../product-specs/0012-tool-scheduler-and-state.md)
  carries the contract (AC5).

### User Query

> Plan remaining: 119 (cancellation propagation + cancellation_lag), and 120
> (loop wiring + bench + config). continue.

Slices 116-118 were already committed with a clean tree. This slice is 119:
make a parent cancellation end every in-flight tool call within the 100 ms
budget spec 0012 AC5 names, and name any tool that ignores its cancellation
slot. Closes AC5.

### Changes Overview

- Areas: `oran-agent` (`src/oran-agent/scheduler.cpp` production;
  `tests/agent/test_scheduler.cpp` tests).
- Key actions:
  - `ToolScheduler::run_batch` now bounds the post-cancellation drain. Phase 1
    is the existing cancel-sensitive `completion.receive()` loop; a cancelled
    receive flips to phase 2, which emits on every child's
    `asio::cancellation_signal`, disables `run_batch`'s own cancellation filter
    (`reset_cancellation_state(disable_cancellation())`), then races the
    remaining drain (`drain_remaining`) against
    `async::sleep_for(kCancellationGrace = 100 ms)`. The batch returns
    `Error::cancelled` with `reason=parent_cancelled` at the grace deadline
    instead of waiting unbounded for stragglers.
  - For every call still unreported at the deadline, the scheduler records a
    `cancellation_lag` audit row through the prototype's `permission::AuditSink`
    (`record_cancellation_lag`): `event_kind=cancellation_lag`,
    `tool_name=<offending tool>`, `verdict/outcome=allow` (the call was already
    in flight), reusing the prototype's scope/agent/identity/`parent_turn_id`
    correlation, and `metadata_json={"error_kind":"cancellation_lag",
    "cancellation_grace_ms":100,"per_call_timeout_ms":<n>}`.
  - The laggard is left detached but alive: it keeps the shared `BatchState`
    (and so the semaphore permit / path lock) until its handler finally
    returns, at which point `run_call` finishes and releases them. The
    per-call timeout still backstops a handler that never returns.
  - Tests: `add_uncancellable_tool` fixture (handler disables its own
    cancellation state, then sleeps — models a tool that never polls its
    slot); a mixed-batch case asserting selective naming (the cancel-aware tool
    is not flagged, the ignoring tool is) plus a 100 ms-guarantee timing bound;
    and a no-false-positive case asserting a purely cancel-aware batch records
    zero `cancellation_lag` rows.

### Design Intent

The blocking question was whether the 100 ms guarantee can be enforced inside
`run_call`'s `co_await (dispatch || timeout)` race or has to move up to
`run_batch`. Reading the vendored asio source settled it:
`awaitable_operators::operator||` is
`make_parallel_group(...).async_wait(wait_for_one_success())`, and
`asio/experimental/impl/parallel_group.hpp` dispatches the group's completion
handler only at `--outstanding_ == 0` — i.e. after **every** operation
finishes. `wait_for_one_success` cancels the loser once one op succeeds, but
the group still awaits the cancelled loser. asio cancellation is cooperative,
so a handler that ignores its cancellation slot never completes, the `||`
never resolves, and the timeout firing cannot free it. No amount of
restructuring the per-call race bounds such a handler.

The only place that can honour the 100 ms budget is therefore the orchestration
layer, which can stop *awaiting* a laggard without being able to stop it:
`run_batch` propagates cancellation to the children, then races the drain
against `kCancellationGrace` and returns regardless of stragglers. This keeps
`run_call` unchanged (so AC1/AC2/AC3/AC4/AC6 stay intact) and adds the bound
exactly once, at the batch boundary.

The plan anticipated naming the laggard "once the per-call timeout fires." That
mechanism does not survive a parent cancel — the per-call timer is cancelled
along with the rest of the race — and waiting out the 60 s default before
naming the tool is worse for an operator than naming it at 100 ms. The slice
therefore records `cancellation_lag` at the grace boundary, using a distinct
`event_kind` so it does not perturb AC7's "exactly N `permission_decision`
rows" invariant, and keeps `per_call_timeout_ms` in the row's metadata so the
backstop is still discoverable. The decision is logged in the plan's Decision
Log for the next agent.

### Files Modified

- `src/oran-agent/scheduler.cpp` — `#include <oran/permission/audit.hpp>`; add
  `kCancellationGrace` + `cancellation_lag_metadata_json`; split `run_batch`'s
  drain into cancel-sensitive phase 1 and grace-bounded phase 2; add the
  `drain_remaining` (static) and `record_cancellation_lag` (member) coroutines.
- `tests/agent/test_scheduler.cpp` — add `<asio/cancellation_state.hpp>` /
  `<asio/this_coro.hpp>`; add `add_uncancellable_tool`; add the two slice-119
  `[cancellation]` cases.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 119; `Last completed history` → this file;
  `oran-agent` surface 43 / 10 590 → 45 / 10 607; `Active exec-plan` now reads
  "slices 116-119 close … AC5 …"; snapshot prose gains a slice-119 paragraph
  and `Next intended slice` retargets to slice 120.
- `docs/design-docs/tool-runtime.md` — "Scheduler Boundary" gains a slice-119
  status note (grace window + `cancellation_lag`), trimming the forward note to
  slice 120.
- `docs/design-docs/async-model.md` — the `ToolScheduler` cancellation bullet
  now describes the 100 ms grace window + `cancellation_lag` as shipped, with
  the cooperative-cancellation caveat.
- `docs/ARCHITECTURE.md` — the `oran-agent` row's scheduler clause records
  slice 118 (verified) and slice 119 (shipped); only slice 120 stays staged.
- `docs/exec-plans/active/2026-05-27-tool-scheduler-v1.md` — checked off the
  slice-119 bullets with the design rationale, and added a Decision Log entry.
- No spec `0012` edit (slices 116-118 set the precedent of not annotating the
  spec per-slice; slice 120 is the closing slice). No `QUALITY_SCORE.md` edit
  (the `oran-agent` row revisit is a slice-120 task per the plan). No
  release-note edit (the user-visible parallel-dispatch note lands with the
  slice-120 loop wiring).

### Validation

- Commands run:
  - `xmake build oran-agent` — succeeded (release; production TU clean under
    warnings-as-errors).
  - `xmake build test-agent` — succeeded.
  - `./build/linux/x86_64/release/test-agent "[cancellation]"` — **All tests
    passed (87 assertions in 8 test cases)**; run 5× for stability, identical.
  - `./build/linux/x86_64/release/test-agent` — full suite: **All tests passed
    (10 607 assertions in 45 test cases).**
- Tests added/changed: `tests/agent/test_scheduler.cpp` — 2 new cases
  (43 → 45; +17 assertions, 10 590 → 10 607).
- Bench impact: none. `bench/agent/scheduler_overhead` /
  `scheduler_audit_fanout` land in slice 120 against the production loop path.
- Compile-budget delta: `scheduler.cpp` gains ~50 lines and one already-transitive
  permission include (no new heavy header); within the `oran-agent` budget.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none. Slice 120's loop wiring + bench + config closes the
  arc.
- Linked release note: none yet — the user-visible parallel-tool-dispatch note
  lands in slice 120 with the loop wiring.
