## [2026-06-07 22:38] | Task: Automation Cron Execute Due

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue implementing the most worthwhile slice with the proper repo process,
> understand current progress first, avoid STATUS-only next-slice churn, and use a
> compliant commit message.

### Changes Overview

- Areas: `oran-automation`, automation runtime docs, slice status.
- Key actions: added `CronService::execute_due(...)`, public cron execute
  request/result/attempt shapes, focused service coverage for success-only
  state advancement and handler failure retry state, and the binary slice tag
  `2.0.0-slice200`.

### Design Intent

Slice 199 gave runtime owners a read-only cron scan/wait surface, but callers
still had to duplicate the success-only mark-fired rule after running due work.
This slice adds the smallest execution owner above that scan: a caller supplies
the handler, and automation advances `last_fired_at` only when the handler
returns success. Handler failures are per-attempt results and leave the stored
state unchanged so the same scheduled fire can be retried by the next explicit
call. This deliberately avoids cron config ownership, process timers, hooks,
queues, notifiers, and agent firing; those remain scheduler/service-loop work.

### Files Modified

- `include/oran/automation/service.hpp`
- `include/oran/automation.hpp`
- `src/oran-automation/service.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_service.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 200 snapshot, latest history pointer, focused test
  count, and next boundary.
- `docs/ARCHITECTURE.md` — automation inventory and ownership notes for cron
  due execution.
- `docs/design-docs/automation-runtime.md` — public API, execution semantics,
  future ownership, and validation count.
- `docs/product-specs/0006-automation.md` — shipped prework, current behavior,
  open scheduler gaps, and test count.
- `docs/QUALITY_SCORE.md` — automation coverage count and slice 200 coverage.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — progress,
  decision log, and linked history.
- `docs/releases/feature-release-notes.md` — user-visible slice 200 note.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build test-automation`
  - `xmake run test-automation`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed:
  - `test-automation` now covers `CronService::execute_due(...)` success-only
    mark-fired behavior, handler failure leaving cron state unchanged, not-due
    handler skipping, and invalid execution policy.
- Bench impact:
  - Not benchmarked; this is a low-frequency scheduler-boundary slice, and the
    scheduler tick performance criterion remains downstream.
- Compile-budget delta:
  - No new translation unit or dependency; code extends existing
    `service.cpp` and adds one public `<functional>` handler shape consistent
    with existing callback surfaces.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
