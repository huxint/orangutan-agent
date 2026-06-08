## [2026-06-09 05:45] | Task: automation service hold retry

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue the highest-value current slice in `orangutan-refactor`, follow the
> full repo workflow, and keep shipping slices end to end instead of stopping
> at partial code or STATUS churn.

### Changes Overview

- Areas: `oran-automation`, automation runtime/service semantics, automation
  docs/spec/plan/status tracking.
- Key actions: added the first owner-local blocked-triggered hold/retry policy
  on `AutomationService`, kept public `TriggeredQueue::drain_*` APIs at their
  documented execute-or-drop boundary, and added delayed-retry timing support
  through `TriggeredExecuteOneRequest::attempted_at` so retried leases and
  durable `finished_at` timestamps reflect the actual retry attempt.

### Design Intent

Slice 223 created the correct ownership seam for future blocked-agent policy by
putting one bounded triggered queue beside one explicit cron cycle on
`AutomationService`. The next highest-value step was to land the smallest real
retry policy on that owner without polluting the queue API. Moving
hold/requeue into `TriggeredQueue::drain_once(...)` or `drain_available(...)`
would have violated the boundary already documented in
`docs/design-docs/automation-runtime.md`: those public queue drains only
execute or drop the descriptors they already hold. Slice 224 therefore keeps
`requeue_on_conflict` private to `AutomationService::run_cycle(...)`, where one
caller-owned owner can preserve blocked triggered descriptors across explicit
cycles, retry them before newer queue work, and still leave detached service
loops, notifier routing, and bootstrap automation ownership downstream.

### Files Modified

- `include/oran/automation/queue.hpp`
- `include/oran/automation/runtime.hpp`
- `include/oran/automation/service.hpp`
- `src/oran-automation/queue.cpp`
- `src/oran-automation/runtime.cpp`
- `src/oran-automation/service.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_queue.cpp`
- `tests/automation/test_runtime.cpp`
- `tests/automation/test_service.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumps the snapshot to slice 224 and records the first
  caller-owned blocked-triggered hold/retry boundary plus the next intended
  finite service-loop step above `AutomationService`.
- `docs/ARCHITECTURE.md` — updates the automation inventory and ownership notes
  to include owner-local hold/retry, queue-level rejection of
  `requeue_on_conflict`, and delayed retry timing semantics.
- `docs/design-docs/automation-runtime.md` — documents the new triggered cycle
  result types, owner-local blocked backlog behavior, public queue-boundary
  rejection of `requeue_on_conflict`, and `attempted_at` timing rules.
- `docs/product-specs/0006-automation.md` — updates shipped prework and current
  implementation notes to reflect owner-local hold/retry on
  `AutomationService`.
- `docs/QUALITY_SCORE.md` — updates the automation and test-framework rows for
  slice-224 coverage/results and the next recommended automation step.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — records
  slice-224 progress, rationale, and the downstream finite service-loop follow-
  up instead of broadening queue semantics.
- `docs/releases/feature-release-notes.md` — adds the user-facing release note
  for automation service hold/retry.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `build/linux/x86_64/release/test-automation "AutomationService holds blocked triggered work for a later cycle retry"`
  - `build/linux/x86_64/release/test-automation "TriggeredService::execute_one validates and uses attempted_at for delayed retries"`
  - `build/linux/x86_64/release/test-automation "TriggeredQueue rejects invalid enqueue policy"`
  - `xmake run test-automation`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `git diff --check`
  - `make ci`
- Tests added/changed: added runtime-level hold/retry coverage for preserving
  blocked triggered descriptors across explicit cycles, service-level coverage
  for delayed retry timing through `attempted_at`, and queue-level validation
  coverage that rejects `requeue_on_conflict` on public drain APIs.
- Bench impact: none; the slice changes retry semantics and coverage only.
- Compile-budget delta: no new dependency direction and no new heavy public
  includes; implementation stays in existing automation queue/runtime/service
  translation units.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
