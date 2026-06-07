## [2026-06-07 11:07] | Task: automation runtime state handle

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local xmake / GCC 16.1 release build
- Linked plan: `docs/exec-plans/completed/2026-06-07-automation-retention-cadence.md`

### User Query

Continue the most valuable product-capability slice under the repository
workflow: read status/docs first, use codegraph, avoid bench-only churn, keep
docs/status/history synchronized, validate, and commit with a Conventional
Commit subject.

### Changes Overview

- Areas: automation runtime state ownership, retention service construction,
  bootstrap versioning, docs/status.
- Key actions: added `automation::AutomationRuntimeOptions` and the move-only
  `automation::AutomationRuntime` public API; implemented explicit open/migrate
  ownership for `automation.db`; kept the `storage::Pool` and
  `AutomationRepository` lifetime behind the runtime handle; exposed the
  migration report and repository; added a retention-service factory over the
  owned repository state; added focused runtime coverage; and bumped the binary
  slice tag to `2.0.0-slice192`.

### Design Intent

Slice 190 made the retention tick owner real, and slice 191 made successful
ticks observable through advisory `memory_decay` metadata. The next useful
boundary was not another benchmark or a hidden background loop; it was a stable
state owner that service-loop code can stand on later. `AutomationRuntime` keeps
database opening explicit: callers provide the path and executor, the runtime
creates parent directories, opens and migrates the automation DB, stores the
migration report, and owns the repository lifetime used by retention services.

This deliberately keeps timers, leases, cancellation ownership, agent firing,
bootstrap automatic opening of `automation.db`, and job lifecycle hooks out of
the slice. Those concerns need a caller-started service-loop owner over this
runtime handle.

### Files Modified

- `include/oran/automation.hpp`
- `include/oran/automation/runtime.hpp`
- `src/oran-automation/runtime.cpp`
- `tests/automation/test_runtime.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumped to slice 192, refreshed latest slice and next
  intended slice.
- `docs/design-docs/automation-runtime.md` - documented
  `AutomationRuntime::open(...)`, state ownership, validation, and remaining
  service-loop gaps.
- `docs/product-specs/0006-automation.md` - recorded the shipped runtime state
  handle and open scheduler/service-loop items.
- `docs/design-docs/memory-system.md` and
  `docs/product-specs/0005-memory-system.md` - recorded that automation now owns
  explicit DB open/migrate state without moving periodic execution into memory.
- `docs/design-docs/secrets-and-state.md` and
  `docs/design-docs/storage-runtime.md` - clarified caller-owned automation
  state and storage's generic boundary.
- `docs/design-docs/module-boundaries.md`, `docs/ARCHITECTURE.md`, and
  `docs/BUILD_SYSTEM.md` - documented the runtime state handle and unchanged
  layering.
- `docs/QUALITY_SCORE.md` - refreshed automation test counts and next step.
- `docs/releases/feature-release-notes.md` - added the slice 192 release note.
- `docs/exec-plans/completed/2026-06-07-automation-retention-cadence.md` - marked
  the runtime state-handle milestone complete.

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
  - Added runtime coverage for parent directory creation plus migration,
    already-migrated reopen, empty database path validation, and retention
    service construction over owned repository state with periodic
    `memory_decay` publishing.
  - Focused result: `test-automation` passed with 22 cases / 245 assertions.
  - Binary sanity: `xmake run orangutan -- --help` reports
    `orangutan v2.0.0-slice192`.
  - Base gate: `make ci` passed, including STATUS freshness and dependency
    layering checks.
  - Path hygiene: checked modified/untracked files for local absolute path
    leakage; no matches.
- Bench impact:
  - No benchmark change; this is an ownership/API slice, not a competing
    implementation choice.
- Compile-budget delta:
  - Public automation adds one small runtime header over existing async,
    storage, and service surfaces; no new third-party dependency.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#automation-runtime-state-handle`
