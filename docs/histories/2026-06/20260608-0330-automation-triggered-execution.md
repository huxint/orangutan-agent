## [2026-06-08 03:30] | Task: Automation Triggered Execution History

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue implementing the most valuable next automation slice, do not blindly
> follow the next `STATUS.md` line, keep docs in sync, validate, and commit with
> a compliant message.

### Changes Overview

- Areas: `oran-automation`, runtime versioning, automation docs.
- Key actions:
  - Added durable triggered run history as automation migration v9.
  - Added repository record/list APIs and typed triggered run outcomes.
  - Added caller-driven `TriggeredService::execute(...)` over matched triggered
    descriptors.
  - Extended repository, service, and runtime automation tests.

### Design Intent

Slice 211 made triggered jobs durable and matchable, but there was still no
explicit execution/history boundary for a runtime owner to call. This slice adds
the smallest useful execution surface: callers provide the handler, automation
records one run row per matched descriptor, and handler failures stay per
attempt so later matches can continue.

The boundary deliberately keeps queueing, notifier routing, lifecycle hooks,
leases, and agent firing out of scope. That matches
`docs/design-docs/automation-runtime.md`: automation owns durable descriptors
and history, while a future runtime owner will decide how external triggers are
queued, notified, leased, and dispatched to agents.

### Files Modified

- `include/oran/automation/repository.hpp`
- `include/oran/automation/service.hpp`
- `src/oran-automation/migrations/automation/0009-automation-triggered-runs.sql`
- `src/oran-automation/repository.cpp`
- `src/oran-automation/service.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_repository.cpp`
- `tests/automation/test_service.cpp`
- `tests/automation/test_runtime.cpp`
- `docs/ARCHITECTURE.md`
- `docs/STATUS.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/automation-runtime.md`
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md`
- `docs/product-specs/0006-automation.md`
- `docs/releases/feature-release-notes.md`
- `docs/histories/2026-06/20260608-0330-automation-triggered-execution.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped the current slice to 212, updated the latest
  history pointer, and refreshed the automation test count.
- `docs/product-specs/0006-automation.md` — recorded the triggered
  execution/history surface and remaining triggered gaps.
- `docs/design-docs/automation-runtime.md` — documented the new public API,
  storage row, and caller-owned execution semantics.
- `docs/ARCHITECTURE.md` — updated automation and storage ownership notes for
  triggered run rows.
- `docs/QUALITY_SCORE.md` — refreshed current coverage counts, ownership notes,
  and automation open gaps.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — logged slice
  212 progress under the active automation category plan.
- `docs/releases/feature-release-notes.md` — added the user-visible release note.
- `docs/histories/2026-06/20260608-0330-automation-triggered-execution.md` —
  recorded this slice.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build test-automation`
  - `xmake run test-automation`
- Tests added/changed:
  - `AutomationRepository records and lists triggered runs`
  - triggered run repository validation coverage
  - `TriggeredService::execute records explicit triggered handler attempts`
  - `TriggeredService::execute records cancelled triggered handlers as aborted`
  - `TriggeredService::execute rejects invalid execution policy`
  - `AutomationRuntime constructs triggered service execution over owned state`
- Bench impact: no benchmark change; no steady-state scheduler path was added.
- Compile-budget delta: not measured; the slice adds small repository/service
  functions and one embedded migration.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: queue/backpressure, notifier routing, triggered
  lifecycle hooks/leases, and agent firing remain downstream in the active
  automation plan.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
