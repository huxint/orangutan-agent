## [2026-06-08 12:15] | Task: automation triggered queue drain

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue implementing the most valuable product slice, keep docs in sync,
> validate, and commit with a compliant message.

### Changes Overview

- Areas: `oran-automation`, bootstrap version metadata, automation docs.
- Key actions:
  - Added `TriggeredService::execute_one(...)` plus request/result shapes for
    exact single-descriptor triggered execution.
  - Refactored `TriggeredService::execute(...)` to delegate each matched
    descriptor through `execute_one(...)` while preserving one shared handler
    instance across attempts.
  - Added `TriggeredQueue::drain_once(...)` plus request/result shapes so queue
    consumers can receive and execute exactly one queued descriptor.
  - Added focused coverage for service-level single descriptor execution,
    queue drain-one behavior, later queued descriptor preservation, and invalid
    drain policy.

### Design Intent

This slice closes the smallest useful queue-drain boundary after slice 215's
triggered queue/backpressure work. The important constraint is that draining a
queued descriptor must not call `TriggeredService::execute(...)`, because that
would re-intake by trigger key and execute every stored descriptor matching the
same trigger. `execute_one(...)` makes the single-descriptor operation explicit
while reusing the existing run-row, lifecycle-hook, cancellation outcome, and
optional triggered-agent-lease behavior.

`TriggeredQueue::drain_once(...)` intentionally does not expose lease fields in
this slice. If a queue item were consumed before discovering a same-agent lease
conflict, the runtime would need a defined hold/drop/requeue policy to avoid
silently losing work. That blocked-agent policy remains a later service-loop
slice; this change only establishes caller-owned drain-one semantics.

### Files Modified

- `include/oran/automation/service.hpp`
- `include/oran/automation/queue.hpp`
- `src/oran-automation/service.cpp`
- `src/oran-automation/queue.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_service.cpp`
- `tests/automation/test_queue.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped slice/status, history pointer, next boundary, and
  automation test counts.
- `docs/ARCHITECTURE.md` — documented `TriggeredQueue::drain_once(...)` and
  single-descriptor triggered execution as shipped automation boundaries.
- `docs/design-docs/automation-runtime.md` — documented the new API shapes,
  queue drain-one semantics, non-reintake design constraint, validation, and
  remaining downstream blocked-agent/notifier/agent ownership.
- `docs/product-specs/0006-automation.md` — updated current implementation and
  triggered acceptance status for one-at-a-time queued execution.
- `docs/QUALITY_SCORE.md` — refreshed automation counts and coverage summary.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — marked
  queue drain-one shipped and narrowed remaining queue work.
- `docs/releases/feature-release-notes.md` — added the slice 216 release-note
  row.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `xmake run test-automation`
  - `xmake build oran-automation`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - Added `TriggeredService::execute_one records one explicit triggered descriptor`.
  - Added `TriggeredQueue drains one queued descriptor through the triggered service`.
  - Extended triggered queue validation coverage for empty drain handlers.
- Bench impact: not perf-relevant; this is correctness and API ownership for queue draining.
- Compile-budget delta: no new translation units or third-party dependencies;
  implementation stays in existing automation service/queue files.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none; remaining blocked-agent queue hold/drop semantics,
  notifier routing, agent firing, and detached service-loop startup stay
  tracked in the active automation cron/category plan.
- Linked release note: `docs/releases/feature-release-notes.md`
