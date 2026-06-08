## [2026-06-09 06:48] | Task: automation service loop policy

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

- Areas: `oran-automation`, automation runtime/service loop policy, automation
  docs/spec/plan/status tracking.
- Key actions: added the explicit finite caller-owned loop policy above
  `AutomationService::run_cycle(...)`, so one owner can repeat cycles,
  aggregate triggered/cron results, sleep within caller-owned retry budgets
  when held blocked triggered work remains, and stop with explicit reasons.

### Design Intent

Slice 224 proved that owner-local hold/retry belongs on `AutomationService`,
not on `TriggeredQueue`, but a single `run_cycle(...)` call still left the
caller to hand-roll the repeated retry loop around that state. The next
highest-value boundary was therefore not broader queue semantics, but a finite
caller-owned run policy above the composed owner itself. Slice 225 follows the
same repository style as `CronLoop::run(...)` and `MemoryRetentionLoop::run(...)`:
bounded iteration count, bounded wait budget, explicit stop reasons, no hidden
detached background work, and no bootstrap takeover of `automation.db`.

### Files Modified

- `include/oran/automation/runtime.hpp`
- `src/oran-automation/runtime.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_runtime.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumps the snapshot to slice 225 and records the new
  finite caller-owned service-loop boundary above `AutomationService`.
- `docs/ARCHITECTURE.md` — updates the automation inventory and ownership notes
  to include `AutomationService::run(...)` and its explicit stop-policy layer.
- `docs/design-docs/automation-runtime.md` — documents the new run request /
  result types, stop reasons, and held-work retry sleep semantics.
- `docs/product-specs/0006-automation.md` — updates shipped prework, current
  implementation, and open-items text to reflect that the finite service-loop
  policy above `AutomationService` now exists.
- `docs/QUALITY_SCORE.md` — updates the automation and test-framework rows for
  slice-225 coverage/results and the next recommended automation step.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — records
  slice-225 progress and reframes the next downstream step as startup/shutdown
  ownership above the new finite loop.
- `docs/releases/feature-release-notes.md` — adds the user-facing release note
  for automation service loop policy.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `build/linux/x86_64/release/test-automation "AutomationService::run retries held blocked work within caller wait budget"`
  - `build/linux/x86_64/release/test-automation "AutomationService::run stops when held work exceeds caller retry budget"`
  - `build/linux/x86_64/release/test-automation "AutomationService::run honors outer stop requests before starting work"`
  - `build/linux/x86_64/release/test-automation "AutomationService::run stops on triggered handler failure"`
  - `build/linux/x86_64/release/test-automation "AutomationService::run validates finite loop policy before side effects"`
  - `build/linux/x86_64/release/test-automation "AutomationService::run stops on cron handler failure"`
  - `xmake run test-automation`
  - `xmake build orangutan`
- `xmake run orangutan -- --help`
- `git diff --check`
- `make ci`
- Final focused bucket result: `test-automation` 106 cases / 1849 assertions.
- Tests added/changed: added runtime-level coverage for held-work retry sleeps,
  held-work retry-budget exhaustion, outer stop requests, triggered handler
  failure stop, cron handler failure stop, and loop-policy validation before
  side effects.
- Bench impact: none; the slice adds finite service-loop policy and focused
  runtime coverage only.
- Compile-budget delta: no new dependency direction and no new heavy public
  includes; implementation stays in the existing automation runtime translation
  unit.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
