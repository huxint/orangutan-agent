## [2026-06-07 05:08] | Task: automation retention state

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local xmake / GCC 16.1 release build
- Linked plan:
  `docs/exec-plans/completed/2026-06-07-automation-retention-cadence.md`

### User Query

> Continue implementing the most valuable product-capability slice in the
> active plan, avoid bench-only churn, keep docs/status/history synchronized,
> validate the result, and commit with a Conventional Commit subject.

### Changes Overview

- Areas: automation persistence, storage-boundary docs, memory retention docs,
  build dependency docs.
- Key actions: added `automation::AutomationRepository` over `storage::Pool`,
  embedded the first `automation.db` retention migration in `oran-automation`,
  persisted durable memory-retention jobs by `job_key`, stored
  `last_fired_at`, recorded success/failure run rows, listed recent runs, and
  added focused `test-automation` repository coverage.

### Design Intent

Slice 188 left bootstrap with an automation-owned retention descriptor but no
durable state for the future periodic owner. This slice puts the state boundary
in `oran-automation`, not `oran-storage`, so storage remains the generic
SQLite/migration/pool substrate while automation owns its domain schema.
`job_key` is the durable repository identity; `scope_key` remains the long-term
memory decay scope inside the stored `MemoryRetentionJob`, allowing future
automation policies to share a memory scope without overwriting each other.

The slice intentionally does not start a scheduler, open `automation.db` from
bootstrap, acquire leases, call `memory::longterm::Backend::decay(...)`, publish
periodic `memory_decay`, queue work, or own cancellation for active jobs. The
next product boundary is an explicit service/tick owner that consumes these
stored jobs and records real run outcomes.

### Files Modified

- `include/oran/automation.hpp`
- `include/oran/automation/repository.hpp`
- `src/oran-automation/repository.cpp`
- `src/oran-automation/migrations/automation/0001-automation-retention-state.sql`
- `tests/automation/test_repository.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `xmake/targets.lua`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/automation-runtime.md` — documents the shipped repository
  API, schema, persistence semantics, and remaining service-loop gaps.
- `docs/product-specs/0006-automation.md` — updates shipped prework and open
  v1 scheduler/service items.
- `docs/design-docs/storage-runtime.md` — keeps `oran-storage` generic and
  records that automation state lives above the pool in `oran-automation`.
- `docs/design-docs/secrets-and-state.md` — updates `automation.db` ownership
  while keeping bootstrap ownership downstream.
- `docs/design-docs/module-boundaries.md`, `docs/ARCHITECTURE.md`, and
  `docs/BUILD_SYSTEM.md` — document the new `oran-automation` dependencies on
  `oran-async`, `oran-storage`, and `oran-memory`.
- `docs/design-docs/memory-system.md` and
  `docs/product-specs/0005-memory-system.md` — distinguish persistent
  automation state from periodic execution/publishing.
- `docs/STATUS.md`, `docs/QUALITY_SCORE.md`, and
  `docs/releases/feature-release-notes.md` — move the project snapshot,
  quality counts, and release note to slice 189.
- `docs/exec-plans/completed/2026-06-07-automation-retention-cadence.md` — marks
  the persistent-state milestone complete and records the service/tick owner as
  the next boundary.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build oran-automation`
  - `xmake build test-automation`
  - `xmake run test-automation`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed:
  - `tests/automation/test_repository.cpp` covers migration idempotence, job
    round-trips, policy/state updates, `last_fired_at`, run recording/listing,
    list limits, and repository input validation.
  - Focused result: `test-automation` passed with 12 cases / 110 assertions.
  - Binary sanity: `xmake run orangutan -- --help` reports
    `orangutan v2.0.0-slice189`.
  - Base gate: `make ci` passed, including STATUS freshness and dependency
    layering checks.
  - Path hygiene: checked modified/untracked files for local absolute path
    leakage; no matches.
- Bench impact: none; this is persistence state plumbing for the next service
  owner, not scheduler tick performance.
- Compile-budget delta: one automation repository TU plus a light public
  header; no new third-party dependency.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#automation-retention-state`
