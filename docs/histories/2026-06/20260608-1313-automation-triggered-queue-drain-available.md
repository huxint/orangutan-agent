## [2026-06-08 13:13] | Task: automation triggered queue drain available

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue implementing the most valuable product slice, keep docs in sync,
> validate, and commit with a compliant message.

### Changes Overview

- Areas: `oran-async`, `oran-automation`, bootstrap version metadata.
- Key actions:
  - Added `async::Channel<T>::try_receive()` as a non-blocking polling
    primitive that returns a buffered value, `std::nullopt` for an open empty
    queue, or `ErrorKind::cancelled` for a closed empty queue.
  - Added `TriggeredQueue::try_receive()` over the same bounded channel state.
  - Added `TriggeredQueueDrainAvailableStopReason`,
    `TriggeredQueueDrainAvailableRequest`, and
    `TriggeredQueueDrainAvailableResult`.
  - Added `TriggeredQueue::drain_available(...)`, a finite caller-awaited batch
    drain that consumes up to `max_jobs`, stops on open-empty, closed-empty, or
    the limit, and reports drained/completed/failed/dropped counters plus
    per-item drain results.
  - Refactored queue execution through a private `execute_queued(...)` helper so
    `drain_once(...)` and `drain_available(...)` share the exact
    single-descriptor execution/drop-on-conflict path.

### Design Intent

The future automation service needs a way to clear currently buffered
triggered queue work without inventing a detached queue owner yet. A background
loop would prematurely choose wakeup, shutdown, notifier, agent-firing, and
blocked-agent retry policy. This slice instead adds the smallest reusable
foundation: non-blocking FIFO polling and a finite caller-owned batch drain.

`drain_available(...)` deliberately executes the descriptor returned by
`try_receive()` directly through the shared `execute_queued(...)` helper. It
does not call `drain_once(...)` after polling, because `drain_once(...)` would
perform a second receive and skip the descriptor already consumed from the
queue.

### Files Modified

- `include/oran/async/channel.hpp`
- `include/oran/automation.hpp`
- `include/oran/automation/queue.hpp`
- `src/oran-automation/queue.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/async/test_async.cpp`
- `tests/automation/test_queue.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped slice/status, history pointer, next boundary, and
  async/automation test counts.
- `docs/ARCHITECTURE.md` — documented async channel polling and finite
  triggered queue batch draining as shipped boundaries.
- `docs/design-docs/async-model.md` — documented `Channel<T>::try_receive()`
  semantics and the triggered queue consumer.
- `docs/design-docs/automation-runtime.md` — documented the batch drain API,
  stop reasons, counters, shared execution/drop path, and remaining
  notifier/agent/background-loop gaps.
- `docs/product-specs/0006-automation.md` — updated current implementation and
  acceptance status for finite triggered queue draining.
- `docs/QUALITY_SCORE.md` — refreshed async/automation counts and coverage
  summary.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — recorded
  the slice 218 boundary, validation targets, and decision log.
- `docs/releases/feature-release-notes.md` — added the slice 218 release-note
  row.

### Validation

- Commands run:
  - `xmake build test-async`
  - `build/linux/x86_64/release/test-async "Channel try_receive reports empty without waiting and drains FIFO values"`
  - `build/linux/x86_64/release/test-async "Channel try_receive drains buffered values before reporting closed"`
  - `build/linux/x86_64/release/test-async "Channel try_receive completes a pending sender without awaiting"`
  - `xmake run test-async`
  - `xmake build test-automation`
  - `build/linux/x86_64/release/test-automation "TriggeredQueue drains available queued descriptors without waiting"`
  - `build/linux/x86_64/release/test-automation "TriggeredQueue drain_available counts handler failures and lease-conflict drops"`
  - `build/linux/x86_64/release/test-automation "TriggeredQueue rejects invalid enqueue policy"`
  - `xmake run test-automation`
- Tests added/changed:
  - Added async channel polling coverage for empty FIFO polling,
    buffered-before-closed behavior, and zero-capacity pending-sender
    completion.
  - Added triggered queue available-batch drain coverage for empty/max-jobs
    stopping, completed counters, failed counters, drop-on-conflict counters,
    queue consumption, handler call counts, and run-row side effects.
  - Extended triggered queue validation coverage for `max_jobs = 0`.
- Bench impact: not perf-relevant; this is queue ownership and correctness
  coverage before a scheduler tick loop exists.
- Compile-budget delta: no new translation units or third-party dependencies;
  implementation stays in the existing async channel header and automation queue
  translation unit.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none; notifier routing, agent firing, richer hold/requeue
  semantics, and detached service-loop startup stay tracked in the active
  automation cron/category plan.
- Linked release note: `docs/releases/feature-release-notes.md`
