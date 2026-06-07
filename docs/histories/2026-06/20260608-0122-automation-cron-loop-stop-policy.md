## [2026-06-08 01:22] | Task: automation cron loop stop policy

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue the automation cron/category workstream slice by slice, choose the
> most useful next boundary after reading current docs/code, validate it, sync
> docs, and commit with a Conventional Commit message.

### Changes Overview

- Areas: `oran-automation`, caller-owned cron loop policy, automation docs.
- Key actions: added `CronLoopStopPredicate`,
  `CronLoopRunRequest::stop_requested`,
  `CronLoopRunStopReason::stop_requested`, runtime service-cycle pass-through,
  and focused coverage for stop-before-work plus stop-after-success behavior.

### Design Intent

The next smallest scheduler-service ownership boundary was graceful shutdown
before detached timers, queues, or notifiers. A synchronous stop predicate gives
runtime owners a way to end a finite cron loop between explicit execution
iterations without changing handler cancellation semantics or introducing a
background service. Active handler cancellation, aborted run classification,
and retry/drop policy remain separate downstream choices.

### Files Modified

- `include/oran/automation/loop.hpp`
- `include/oran/automation/runtime.hpp`
- `src/oran-automation/loop.cpp`
- `src/oran-automation/runtime.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_runtime.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

List every doc edited in the same PR as part of this change. If the change
invalidated a doc and the matching edit is *missing*, the PR is incomplete.

- `docs/design-docs/automation-runtime.md` — records the stop predicate API,
  loop semantics, service-cycle pass-through, and validation counts.
- `docs/product-specs/0006-automation.md` — marks cooperative cron loop stop
  policy as shipped while keeping active handler cancellation downstream.
- `docs/ARCHITECTURE.md` — updates automation ownership notes for cron loop
  stop policy.
- `docs/STATUS.md` — bumps the project snapshot to slice 207.
- `docs/QUALITY_SCORE.md` — updates automation/test counts and shipped slice
  coverage.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — records the
  slice decision, validation, and progress.
- `docs/releases/feature-release-notes.md` — adds the slice 207 user-facing
  release note.

### Validation

- Commands run:
  `xmake build test-automation`;
  `build/linux/x86_64/release/test-automation "CronLoop::run honors stop requests before starting work"`;
  `build/linux/x86_64/release/test-automation "CronLoop::run stops after a successful iteration when requested"`;
  `build/linux/x86_64/release/test-automation "AutomationRuntime forwards cron service cycle stop requests"`;
  `xmake run test-automation`;
  `xmake build orangutan`;
  `xmake run orangutan -- --help`;
  `xmake build test-bootstrap`;
  `xmake run test-bootstrap`;
  `git diff --check`;
  `make ci`.
- Tests added/changed: runtime coverage for stop-before-work, stop-after-one
  successful cron iteration, and service-cycle forwarding of the stop predicate.
- Bench impact: none; this is loop policy correctness, not scheduler
  throughput.
- Compile-budget delta: one `std::function` field in the existing public loop
  header and small implementation changes in existing translation units; no new
  dependency or TU.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
