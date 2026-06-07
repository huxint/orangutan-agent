## [2026-06-07 09:59] | Task: automation retention service tick

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: CLI coding session
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-retention-cadence.md`

### User Query

Continue the most valuable slice under the repository
workflow: read status/docs first, use codegraph, prefer product capability over
bench-only churn, keep docs/status/history synced, validate, and commit with a
Conventional Commit message.

### Changes Overview

- Areas: `oran-automation`, long-term memory retention automation, docs/status.
- Key actions: added `automation::MemoryRetentionService` as the explicit
  caller-driven retention tick owner; added service coverage for not-due,
  due-success, backend-failure, invalid-input, and missing-job paths; bumped the
  binary slice tag to `2.0.0-slice190`; synced status, architecture, design,
  specs, quality, release notes, and the active execution plan.

### Design Intent

Slice 189 gave the future periodic owner durable job/run state but still left
the actual owner of one retention execution ambiguous. This slice keeps the
boundary small: `MemoryRetentionService::tick(...)` loads one stored job,
reuses `plan_memory_retention(...)`, calls a supplied
`memory::longterm::Backend::decay(...)` only when due, records run outcomes, and
advances `last_fired_at` only after a successful backend run and run-row insert.
Backend failures record a failed run and leave `last_fired_at` unchanged so
retry/catch-up policy stays explicit. Timers, leases, periodic `memory_decay`
publication, notifier routing, and bootstrap opening of `automation.db` remain
future service-loop concerns.

### Files Modified

- `include/oran/automation/service.hpp`
- `src/oran-automation/service.cpp`
- `tests/automation/test_service.cpp`
- `include/oran/automation.hpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 190, refreshed latest slice and next slice.
- `docs/design-docs/automation-runtime.md` — documented service/tick API and
  success/failure state semantics.
- `docs/product-specs/0006-automation.md` — updated shipped prework and open
  automation gaps.
- `docs/design-docs/memory-system.md` — recorded caller-driven periodic
  execution while keeping hook publication and service-loop ownership
  downstream.
- `docs/product-specs/0005-memory-system.md` — updated memory retention
  acceptance status and validation count.
- `docs/ARCHITECTURE.md` — updated `oran-automation`, `oran-memory`,
  `oran-storage`, and `oran-bootstrap` boundaries.
- `docs/BUILD_SYSTEM.md` — updated `oran-automation` dependency and ownership
  description.
- `docs/design-docs/secrets-and-state.md` — updated state ownership for the
  retention tick.
- `docs/design-docs/storage-runtime.md` — clarified storage remains generic
  under the automation repository/tick owners.
- `docs/QUALITY_SCORE.md` — refreshed automation and test framework counts.
- `docs/releases/feature-release-notes.md` — added the slice 190 release note.
- `docs/exec-plans/active/2026-06-07-automation-retention-cadence.md` — marked
  the service/tick milestone complete and set hook production as the next
  boundary.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build oran-automation`
  - `xmake build test-automation`
  - `xmake run test-automation`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
  - local path-leak scan for staged/unstaged changed files
- Tests added/changed:
  - Added `tests/automation/test_service.cpp`.
  - Focused result: `test-automation` passed with 16 cases / 169 assertions.
- Bench impact:
  - No benchmark change; this is a correctness/ownership slice, not a
    competing implementation choice.
- Compile-budget delta:
  - One small public header and one `oran-automation` translation unit; no new
    third-party dependency.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#automation-retention-service-tick`
