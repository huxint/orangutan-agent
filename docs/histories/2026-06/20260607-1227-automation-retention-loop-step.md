## [2026-06-07 12:27] | Task: automation retention loop step

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local xmake / GCC 16.1 release build
- Linked plan: `docs/exec-plans/completed/2026-06-07-automation-retention-cadence.md`

### User Query

Continue the most valuable automation product-capability slice under the
repository workflow: read status/docs first, use codegraph, avoid bench-only
churn, keep docs/status/history synchronized, validate, and commit with a
Conventional Commit subject.

### Changes Overview

- Areas: automation runtime loop ownership, retention wait/cancellation
  boundary, bootstrap versioning, docs/status.
- Key actions: added `automation::MemoryRetentionLoopRunOnceRequest`,
  `automation::MemoryRetentionLoopRunOnceResult`, and
  `automation::MemoryRetentionLoop`; exposed `AutomationRuntime::memory_retention_loop(...)`;
  implemented `MemoryRetentionLoop::run_once(...)` as a caller-started single
  awaitable that ticks immediately, waits only within the caller's budget,
  delegates due work back to `MemoryRetentionService::tick(...)`, propagates
  cancellation while sleeping, rejects negative wait budgets, and bumped the
  binary slice tag to `2.0.0-slice193`.

### Design Intent

Slice 192 gave future owners a stable automation state handle. The next useful
boundary was a small loop step over that handle, not a full scheduler and not
another benchmark. `MemoryRetentionLoop` keeps ownership explicit: callers
construct it from `AutomationRuntime`, choose the `job_key`, provide the clock
value, and cap how long the awaitable may wait. If the job is not due and the
next fire is outside that budget, the call returns the not-due tick result
without sleeping. If the next fire is within budget, the call uses
`async::sleep_for(...)`, so parent cancellation is surfaced as
`ErrorKind::cancelled` before any second tick runs.

This deliberately keeps bootstrap automatic startup, detached tasks, per-agent
leases, cron parsing, queueing/backpressure, job lifecycle hooks, notifier
routing, and long-running service-loop policy out of the slice. Those concerns
need the next owner to build around this explicit loop step.

### Files Modified

- `include/oran/automation.hpp`
- `include/oran/automation/loop.hpp`
- `include/oran/automation/runtime.hpp`
- `src/oran-automation/loop.cpp`
- `src/oran-automation/runtime.cpp`
- `tests/automation/test_runtime.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumped to slice 193, refreshed latest slice and next
  intended slice.
- `docs/design-docs/automation-runtime.md` - documented the loop API,
  runtime factory, run-once semantics, cancellation, validation, and remaining
  service-loop gaps.
- `docs/product-specs/0006-automation.md` - recorded the shipped caller-started
  loop step and kept cron/triggered/bootstrap/service-loop work open.
- `docs/design-docs/memory-system.md` and
  `docs/product-specs/0005-memory-system.md` - recorded that automation can now
  wait for one due retention job without moving service-loop ownership into
  memory.
- `docs/design-docs/secrets-and-state.md` and
  `docs/design-docs/storage-runtime.md` - clarified that the loop step is above
  caller-owned automation state and does not add storage-owned schema.
- `docs/design-docs/module-boundaries.md`, `docs/ARCHITECTURE.md`, and
  `docs/BUILD_SYSTEM.md` - documented the new public loop step and unchanged
  layering.
- `docs/QUALITY_SCORE.md` - refreshed automation test counts and next step.
- `docs/releases/feature-release-notes.md` - added the slice 193 release note.
- `docs/exec-plans/completed/2026-06-07-automation-retention-cadence.md` - marked
  the retention loop-step milestone complete.

### Validation

- Commands run:
  - `git diff --check`
  - `scripts/check-deps.sh`
  - `xmake build oran-automation`
  - `xmake build test-automation`
  - `xmake run test-automation`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
  - local path-leak scan for changed files
- Tests added/changed:
  - Added loop coverage for skipping waits beyond budget, waiting within budget
    and running due work, cancellation during the wait, and negative wait-budget
    validation.
  - Focused result: `test-automation` passed with 26 cases / 274 assertions.
  - Binary sanity: `xmake run orangutan -- --help` reports
    `orangutan v2.0.0-slice193`.
  - Base gate: `make ci` passed, including STATUS freshness and dependency
    layering checks.
  - Path hygiene: checked modified/untracked files for local absolute path
    leakage; no matches.
- Bench impact:
  - No benchmark change; this is a correctness/ownership slice with no
    competing implementation choice.
- Compile-budget delta:
  - One small automation loop header and implementation over existing async and
    service surfaces; no new third-party dependency.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#automation-retention-loop-step`
