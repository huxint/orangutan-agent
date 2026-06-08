## [2026-06-08 12:47] | Task: automation triggered queue lease drop

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue implementing the most valuable product slice, keep docs in sync,
> validate, and commit with a compliant message.

### Changes Overview

- Areas: `oran-automation`, automation hook docs, bootstrap version metadata.
- Key actions:
  - Added `TriggeredQueueBlockedAgentPolicy::drop_on_conflict`.
  - Added drain-time `lease_owner_key` / `lease_ttl` fields and optional
    `TriggeredQueueDrainOnceResult::dropped` metadata.
  - Taught `TriggeredQueue::drain_once(...)` to pass lease ownership into
    `TriggeredService::execute_one(...)` and convert active triggered-agent
    lease conflicts into explicit `TriggeredDroppedJob(reason=agent_lease_conflict)`.
  - Preserved handler/run-row/lifecycle-hook suppression for the dropped
    descriptor and reused `job_dropped` advisory metadata for observability.

### Design Intent

Slice 216 deliberately avoided drain-time lease fields because consuming a queue
item before discovering a same-agent lease conflict would otherwise lose work
silently. This slice makes that policy explicit for the smallest useful
service-loop boundary: callers can choose `drop_on_conflict`, consume exactly
the received queued descriptor, and observe the loss through both the returned
drop metadata and optional `job_dropped` hook payload.

The slice does not implement hold or requeue behavior. A hold/requeue policy
needs additional ownership for parking, wakeup, and retry ordering; adding it
inside this single-item drain API would make hidden scheduling decisions before
notifier routing or agent firing exists.

### Files Modified

- `include/oran/automation/queue.hpp`
- `src/oran-automation/queue.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_queue.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped slice/status, history pointer, next boundary, and
  automation test counts.
- `docs/ARCHITECTURE.md` — documented drain-time `agent_lease_conflict` drop
  handling as a shipped automation boundary.
- `docs/design-docs/automation-runtime.md` — documented the new queue API
  fields, drop-on-conflict behavior, run-row suppression, and remaining
  hold/requeue/notifier/agent ownership.
- `docs/design-docs/permissions-and-hooks.md` — documented the new
  `job_dropped(reason=agent_lease_conflict)` producer path.
- `docs/product-specs/0006-automation.md` — updated current implementation and
  acceptance status for blocked triggered-agent queue drains.
- `docs/QUALITY_SCORE.md` — refreshed automation counts and coverage summary.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — marked the
  drop-on-conflict slice shipped and narrowed remaining blocked-agent queue work.
- `docs/releases/feature-release-notes.md` — added the slice 217 release-note row.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `build/linux/x86_64/release/test-automation "TriggeredQueue drops drained descriptors on active triggered agent lease conflicts"`
  - `build/linux/x86_64/release/test-automation "TriggeredQueue drains one queued descriptor through the triggered service"`
  - `build/linux/x86_64/release/test-automation "TriggeredQueue rejects invalid enqueue policy"`
  - `build/linux/x86_64/release/test-automation "TriggeredQueue drops newest overflow and publishes job_dropped metadata"`
  - `xmake run test-automation`
- Tests added/changed:
  - Added `TriggeredQueue drops drained descriptors on active triggered agent lease conflicts`.
  - Extended triggered queue validation coverage for drain-time lease TTL and
    blocked-agent policy validation.
- Bench impact: not perf-relevant; this is correctness and API ownership for
  blocked triggered queue drains.
- Compile-budget delta: no new translation units or third-party dependencies;
  implementation stays in the existing automation queue files.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none; richer hold/requeue policy, notifier routing, agent
  firing, and detached service-loop startup stay tracked in the active
  automation cron/category plan.
- Linked release note: `docs/releases/feature-release-notes.md`
