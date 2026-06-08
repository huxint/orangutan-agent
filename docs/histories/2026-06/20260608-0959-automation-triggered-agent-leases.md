## [2026-06-08 09:59] | Task: Automation Triggered Agent Leases

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

- Areas: `oran-automation`, triggered execution leases, automation docs.
- Key actions:
  - Added migration v10 for `automation_triggered_agent_leases`.
  - Added repository acquire/release APIs for triggered agent leases.
  - Let `TriggeredService::execute(...)` opt into same-agent lease ownership
    through `TriggeredExecuteRequest::lease_owner_key` / `lease_ttl`.
  - Added repository and service tests for active conflicts, expired takeover,
    owner-matched release, success/failure release, and invalid lease policy.

### Design Intent

Slice 213 made triggered execution observable through lifecycle hooks, but
explicit triggered handlers could still overlap work for the same stored
`agent_key`. This slice mirrors the established cron agent lease semantics at
the triggered execution boundary: callers opt in explicitly, active conflicts
stop before handler/run/hook work, expired leases can be taken over, and releases
are tied to the durable execution outcome. Queue/backpressure, notifier routing,
and actual agent invocation remain downstream.

### Files Modified

- `include/oran/automation.hpp`
- `include/oran/automation/repository.hpp`
- `include/oran/automation/service.hpp`
- `src/oran-automation/migrations/automation/0010-automation-triggered-agent-leases.sql`
- `src/oran-automation/repository.cpp`
- `src/oran-automation/service.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_repository.cpp`
- `tests/automation/test_runtime.cpp`
- `tests/automation/test_service.cpp`
- `docs/ARCHITECTURE.md`
- `docs/STATUS.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/automation-runtime.md`
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md`
- `docs/product-specs/0006-automation.md`
- `docs/releases/feature-release-notes.md`
- `docs/histories/2026-06/20260608-0959-automation-triggered-agent-leases.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 214 and refreshed automation counts.
- `docs/product-specs/0006-automation.md` — moved triggered agent leases into
  current implementation and left queues/notifiers/agent firing downstream.
- `docs/design-docs/automation-runtime.md` — documented migration v10,
  repository APIs, service opt-in policy, conflict behavior, and release
  cleanup semantics.
- `docs/ARCHITECTURE.md` — updated storage/automation ownership notes.
- `docs/QUALITY_SCORE.md` — refreshed current automation coverage and gaps.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — logged the
  slice and updated remaining triggered gaps.
- `docs/releases/feature-release-notes.md` — added the user-visible release note.
- `docs/histories/2026-06/20260608-0959-automation-triggered-agent-leases.md`
  — recorded this slice.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `build/linux/x86_64/release/test-automation "[unit][automation][repository][triggered][lease]"`
  - `build/linux/x86_64/release/test-automation "TriggeredService::execute leases triggered handlers and releases after outcomes"`
  - `build/linux/x86_64/release/test-automation "TriggeredService::execute blocks active triggered agent leases before handlers"`
  - `build/linux/x86_64/release/test-automation "TriggeredService::execute rejects invalid execution policy"`
  - `xmake run test-automation`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - `AutomationRepository acquires, expires, and releases triggered agent leases`
  - `TriggeredService::execute leases triggered handlers and releases after outcomes`
  - `TriggeredService::execute blocks active triggered agent leases before handlers`
- Bench impact: no benchmark change; no scheduler hot path was added.
- Compile-budget delta: not measured; this adds one embedded SQL migration and
  small repository/service API surfaces.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: triggered queue/backpressure, notifier routing, and agent
  firing remain downstream in the active automation plan.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
