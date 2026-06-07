## [2026-06-07 17:27] | Task: automation retention loop policy

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI/API
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-retention-cadence.md`

### User Query

> Continue implementing the most valuable current slice, follow the documented
> workflow, keep docs/status in sync, and finish with a conforming commit.

### Changes Overview

- Areas: `oran-automation`, automation runtime docs, memory-retention status.
- Key actions: added `MemoryRetentionLoop::run(...)` plus bounded loop
  request/result/stop-reason types; the loop repeatedly drives the existing
  leased `run_once(...)` step for one stored retention job until
  `max_iterations` is reached or no due work remains within `max_total_wait`.
  Focused runtime tests now cover due-backlog catch-up, no-due-work stopping,
  and invalid loop-policy budgets.

### Design Intent

This slice lands the explicit service-loop policy boundary requested by
`docs/STATUS.md` without turning bootstrap into a scheduler. Reusing
`run_once(...)` keeps lease, wait, cancellation, backend, and hook semantics in
one place; the new `run(...)` layer only adds caller-owned repetition and
summary reporting. The slice intentionally leaves process timers, cron,
triggered jobs, queueing/backpressure, notifier routing, and agent firing for a
separate spec-0006 plan.

### Files Modified

- `include/oran/automation/loop.hpp`
- `src/oran-automation/loop.cpp`
- `tests/automation/test_runtime.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/automation-runtime.md` — documented the finite loop API and
  stop semantics.
- `docs/product-specs/0006-automation.md` — updated shipped prework and open
  scheduler scope.
- `docs/product-specs/0005-memory-system.md` — recorded that memory still does
  not own periodic execution.
- `docs/design-docs/memory-system.md` — updated long-term retention rollups.
- `docs/design-docs/storage-runtime.md` — recorded automation stays above
  generic storage.
- `docs/design-docs/secrets-and-state.md` — recorded that loop policy adds no
  secret material.
- `docs/ARCHITECTURE.md` — updated automation ownership notes.
- `docs/BUILD_SYSTEM.md` — updated the automation target ownership summary.
- `docs/QUALITY_SCORE.md` — updated automation coverage and next-step status.
- `docs/STATUS.md` — bumped to slice 196 and pointed at this history.
- `docs/exec-plans/active/2026-06-07-automation-retention-cadence.md` — added
  the finite loop-policy milestone and progress entry.
- `docs/releases/feature-release-notes.md` — added the user-facing release note.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `xmake run test-automation`
  - `xmake build bench-automation`
  - `xmake run bench-automation`
  - `xmake build oran-automation`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - `MemoryRetentionLoop::run drives finite due backlog without hidden service ownership`
  - `MemoryRetentionLoop::run stops when the next fire exceeds the caller wait budget`
  - `MemoryRetentionLoop::run rejects invalid loop policy budgets`
- Bench impact: no new benchmark; existing `bench-automation` ran successfully
  with schedule evaluation and retention planning rows.
- Compile-budget delta: not measured separately; no new third-party dependency,
  target, or heavy public include was added.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#automation-retention-loop-policy`
