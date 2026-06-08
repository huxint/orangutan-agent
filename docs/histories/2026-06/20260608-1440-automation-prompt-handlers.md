## [2026-06-08 14:40] | Task: automation prompt handlers

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue the automation workstream, pick the highest-value current slice
> instead of blindly following the next STATUS line, and close it end to end
> with docs, validation, and a compliant commit.

### Changes Overview

- Areas: `oran-automation`, `oran-bootstrap`, automation docs.
- Key actions: added an automation-owned prompt-runner adapter surface,
  exposed cron/triggered handler factories over stored job prompts, and
  extended automation service tests to prove success/failure behavior through
  the existing run-history/state-advance paths.

### Design Intent

Slice 219 made cron and triggered descriptors carry durable `agent_prompt`, but
the scheduler/runtime boundary still lacked a small reusable adapter that could
turn those stored prompts into executable work without making
`oran-automation` depend on bootstrap, CLI, provider, or detached service-loop
ownership. This slice adds that thin seam inside `oran-automation`:
callers inject an `AutomationPromptRunner`, then reuse the existing
`CronService`, `TriggeredService`, and `TriggeredQueue` execution paths so
lease, retry, run-history, and state-advance semantics remain unchanged. The
tradeoff is deliberate: automation still does not construct
`bootstrap::AgentPromptRunner`, route notifications, or define blocked-agent
hold/requeue policy. Those ownership decisions stay for later slices.

### Files Modified

- `include/oran/automation.hpp`
- `include/oran/automation/prompt.hpp`
- `src/oran-automation/prompt.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_service.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/automation-runtime.md` — documents the new
  `AutomationPromptRunner` surface and the cron/triggered handler adapters.
- `docs/product-specs/0006-automation.md` — updates shipped prework,
  implementation status, and acceptance notes for prompt-runner adapters.
- `docs/ARCHITECTURE.md` — records the expanded `oran-automation` public
  surface and slice-220 ownership boundary.
- `docs/QUALITY_SCORE.md` — updates automation coverage notes and
  `test-automation` counts.
- `docs/STATUS.md` — bumps the active snapshot to slice 220 and points at this
  history.
- `docs/releases/feature-release-notes.md` — adds the user-facing release note
  for automation prompt-handler adapters.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — records
  slice-220 progress and focused validation coverage.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build test-automation`
  - `build/linux/x86_64/release/test-automation "Cron prompt handler runs stored cron job prompt"`
  - `build/linux/x86_64/release/test-automation "Triggered prompt handler runs stored triggered job prompt"`
  - `xmake run test-automation`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed: added cron and triggered service coverage for the
  injected prompt-runner adapter, including cron success with durable
  `last_fired_at` advancement and triggered failure with durable failure
  run-row recording.
- Bench impact: none; the slice adds an adapter seam and focused service tests
  only.
- Compile-budget delta: one small `oran-automation` implementation file and one
  lightweight public header; no new dependency.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
