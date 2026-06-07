## [2026-06-08 02:59 +0800] | Task: Automation triggered intake

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

Continue implementing the most valuable next slice in `orangutan-refactor`,
without blindly following `STATUS.md`; keep docs/status/history/release in sync
and close with a Conventional Commit.

### Changes Overview

- Areas: `oran-automation`, automation docs/status/release/history.
- Key actions:
  - Added migration v8 for durable triggered job descriptors in
    `automation_triggered_jobs`.
  - Added `AutomationRepository` upsert/load/list-by-trigger APIs for
    triggered jobs carrying `job_key`, `trigger_key`, and `agent_key`.
  - Added `TriggeredService::intake(...)` plus
    `AutomationRuntime::triggered_service()` so callers can match external
    trigger keys against stored descriptors without queueing or agent firing.
  - Added repository, service, and runtime coverage for triggered intake.

### Design Intent

Spec 0006 still needed the first triggered-category boundary. The smallest
valuable slice is durable descriptor intake: runtime owners can map an external
event key to matching stored jobs, while queue/backpressure policy, notifier
routing, run history, and actual agent execution remain explicit downstream
work. This preserves the automation-runtime rule that callers own lifecycle and
bootstrap does not gain hidden `automation.db` or detached scheduler side
effects.

### Files Modified

- `include/oran/automation.hpp`
- `include/oran/automation/repository.hpp`
- `include/oran/automation/runtime.hpp`
- `include/oran/automation/service.hpp`
- `src/oran-automation/migrations/automation/0008-automation-triggered-jobs.sql`
- `src/oran-automation/repository.cpp`
- `src/oran-automation/runtime.cpp`
- `src/oran-automation/service.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_repository.cpp`
- `tests/automation/test_runtime.cpp`
- `tests/automation/test_service.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 211 and summarized triggered intake.
- `docs/product-specs/0006-automation.md` — moved triggered intake from fully
  open to shipped descriptor/intake state.
- `docs/design-docs/automation-runtime.md` — documented migration v8,
  repository APIs, `TriggeredService`, and runtime factory.
- `docs/ARCHITECTURE.md` — updated automation boundary and storage notes.
- `docs/QUALITY_SCORE.md` — updated automation and test-framework rows.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — recorded
  milestone-4 triggered intake progress without renaming/archiving the active
  plan.
- `docs/releases/feature-release-notes.md` — added the user-visible release
  note.
- `docs/histories/2026-06/20260608-0258-automation-triggered-intake.md` —
  recorded this slice.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `xmake run test-automation` — 75 cases / 1078 assertions
  - `xmake build orangutan`
  - `xmake run orangutan -- --help` — reports `v2.0.0-slice211`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - Added repository round-trip and validation coverage for triggered jobs.
  - Added service intake matching and validation coverage.
  - Added runtime triggered-service factory coverage.
- Bench impact:
  - Not perf-relevant; no scheduler tick loop or queue was introduced.
- Compile-budget delta:
  - Reused existing automation translation units plus one embedded SQL
    migration; no new third-party dependency.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md#2026-06`
