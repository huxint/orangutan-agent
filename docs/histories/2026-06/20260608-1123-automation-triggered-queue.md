## [2026-06-08 11:23] | Task: automation triggered queue

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue implementing the most valuable product slice, keep docs in sync, validate, and commit with a compliant message.

### Changes Overview

- Areas: `oran-automation`, `oran-hook`, bootstrap version metadata, automation docs.
- Key actions:
  - Added `TriggeredQueue`, `TriggeredQueuedJob`, `TriggeredDroppedJob`, queue options, enqueue request/result shapes, and `AutomationRuntime::triggered_queue(...)`.
  - Added bounded caller-owned triggered intake queueing over `TriggeredService::intake(...)` with explicit `receive()` and `drop_newest` overflow behavior.
  - Added advisory `hook::Event::job_dropped` plus metadata-only `JobDroppedPayload` for triggered queue backpressure.
  - Added focused automation and hook coverage for enqueue/receive, overflow/drop metadata, invalid queue inputs, runtime factory construction, and no triggered run rows for queued or dropped work.

### Design Intent

This slice closes the smallest useful triggered queue/backpressure boundary from the automation cron/category plan without making `oran-automation` a hidden scheduler. The queue is process-local and caller-owned: it stores matched descriptors in bounded `async::Channel` state, reports accepted and dropped jobs, and lets consumers explicitly drain work. It deliberately does not execute handlers, acquire triggered agent leases, record triggered run rows, notify channels, call agents, or start a detached service loop. That keeps queue-drain policy, blocked-agent hold/drop semantics, notifier routing, and actual agent firing as later scheduler-service slices.

The only shipped overflow policy is `drop_newest`; it is explicit and observable through `job_dropped` advisory metadata. Queue capacity must be positive at enqueue time so a zero-capacity queue is treated as invalid caller policy instead of silently dropping every matched descriptor.

### Files Modified

- `include/oran/automation.hpp`
- `include/oran/automation/queue.hpp`
- `include/oran/automation/runtime.hpp`
- `include/oran/hook/event.hpp`
- `include/oran/hook/payload.hpp`
- `src/oran-automation/queue.cpp`
- `src/oran-automation/runtime.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_queue.cpp`
- `tests/automation/test_runtime.cpp`
- `tests/hook/test_bus.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped slice/status, history pointer, next boundary, and test/assertion counts.
- `docs/QUALITY_SCORE.md` — refreshed automation and hook coverage summaries and counts.
- `docs/design-docs/automation-runtime.md` — documented the triggered queue API, backpressure behavior, runtime factory, validation, and remaining downstream ownership.
- `docs/design-docs/permissions-and-hooks.md` — documented `job_dropped` and `JobDroppedPayload`.
- `docs/product-specs/0006-automation.md` — updated current automation status and acceptance criteria for triggered queue/backpressure.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — marked triggered queue/backpressure as shipped and narrowed remaining queue work to drain and blocked-agent policy.
- `docs/releases/feature-release-notes.md` — added the slice 215 release-note row.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `xmake run test-automation`
  - `xmake build test-hook`
  - `xmake run test-hook`
  - `xmake build oran-automation`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - Added `tests/automation/test_queue.cpp`.
  - Extended `tests/automation/test_runtime.cpp` to cover `AutomationRuntime::triggered_queue(...)`.
  - Extended `tests/hook/test_bus.cpp` to cover `JobDroppedPayload` delivery.
- Bench impact: not perf-relevant; this is correctness and API ownership for queue/backpressure.
- Compile-budget delta: no new third-party dependency; one small `oran-automation` translation unit was added for the queue implementation.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none; remaining notifier routing, queue drain ownership, blocked-agent hold/drop policy, detached service-loop startup, and agent firing stay tracked in the active automation cron/category plan.
- Linked release note: `docs/releases/feature-release-notes.md`
