## [2026-06-08 03:51] | Task: Automation Triggered Lifecycle Hooks

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue implementing the most valuable automation slice, avoid blind
> `STATUS.md` churn, keep docs in sync, validate, and commit with a compliant
> message.

### Changes Overview

- Areas: `oran-automation`, triggered job lifecycle hooks, automation docs.
- Key actions:
  - Added `TriggeredHookOptions` / `TriggeredServiceOptions`.
  - Let `AutomationRuntime::triggered_service(...)` pass triggered service
    options through.
  - Published advisory `job_started`, `job_failed`, and `job_finished`
    metadata around explicit triggered handler attempts.
  - Added service tests for triggered lifecycle success and failure paths.

### Design Intent

Slice 212 made triggered handler attempts durable, which gave the runtime a
real start/outcome boundary to observe. This slice reuses the existing
`hook::JobLifecyclePayload` surface instead of adding a triggered-specific hook
type, keeping triggered execution aligned with retention and cron lifecycle
metadata.

The hook remains advisory. Sink failures do not veto handler execution or
change durable run outcomes, and the slice still does not add queues,
notifiers, leases, or agent firing.

### Files Modified

- `include/oran/automation/service.hpp`
- `include/oran/automation/runtime.hpp`
- `src/oran-automation/service.cpp`
- `src/oran-automation/runtime.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_service.cpp`
- `docs/ARCHITECTURE.md`
- `docs/STATUS.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/automation-runtime.md`
- `docs/design-docs/permissions-and-hooks.md`
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md`
- `docs/product-specs/0006-automation.md`
- `docs/releases/feature-release-notes.md`
- `docs/histories/2026-06/20260608-0351-automation-triggered-lifecycle-hooks.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 213 and refreshed automation counts.
- `docs/product-specs/0006-automation.md` — moved triggered lifecycle metadata
  into current implementation and left leases/queues/notifiers downstream.
- `docs/design-docs/automation-runtime.md` — documented the triggered hook
  options, runtime factory pass-through, and advisory execution semantics.
- `docs/design-docs/permissions-and-hooks.md` — recorded the triggered producer
  reuse of `JobLifecyclePayload`.
- `docs/ARCHITECTURE.md` — updated automation and hook ownership notes.
- `docs/QUALITY_SCORE.md` — refreshed current automation coverage and gaps.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — logged the
  slice and updated remaining triggered gaps.
- `docs/releases/feature-release-notes.md` — added the user-visible release note.
- `docs/histories/2026-06/20260608-0351-automation-triggered-lifecycle-hooks.md`
  — recorded this slice.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `xmake run test-automation`
- Tests added/changed:
  - `TriggeredService::execute publishes lifecycle metadata for handler success`
  - `TriggeredService::execute publishes lifecycle metadata for handler failure`
- Bench impact: no benchmark change; no scheduler hot path was added.
- Compile-budget delta: not measured; this adds small service/runtime option
  plumbing and tests.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: triggered leases, queue/backpressure, notifier routing, and
  agent firing remain downstream in the active automation plan.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
