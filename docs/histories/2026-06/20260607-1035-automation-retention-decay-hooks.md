## [2026-06-07 10:35] | Task: automation retention decay hooks

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

- Areas: automation retention service, hook metadata, build dependencies,
  docs/status.
- Key actions: added optional hook settings to
  `automation::MemoryRetentionService`, published advisory `memory_decay`
  metadata after successful due retention, surfaced advisory sink/failure
  counts on `MemoryRetentionTickResult`, kept not-due/backend-failure/no-bus
  paths silent, added focused service/hook coverage, added the `oran-hook`
  dependency for `oran-automation`, and bumped the binary slice tag to
  `2.0.0-slice191`.

### Design Intent

Slice 190 made the retention tick owner real, so hook publication can now live
at the actual periodic execution boundary instead of in bootstrap or
`oran-memory`. This slice keeps the producer optional and caller-owned:
`MemoryRetentionService::tick(...)` publishes only when constructed with a
`hook::Bus*`, only after a due tick has durably recorded the successful run and
advanced `last_fired_at`, and only with the content-free `MemoryDecayPayload`
shape already used for startup retention. Advisory sink failures are reported
but do not roll back retention state. Timers, leases, cancellation ownership,
bootstrap opening of `automation.db`, and job lifecycle hooks remain future
service-loop concerns.

### Files Modified

- `include/oran/automation/service.hpp`
- `src/oran-automation/service.cpp`
- `tests/automation/test_service.cpp`
- `include/oran/hook/payload.hpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `xmake/targets.lua`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumped to slice 191, refreshed latest slice and next
  intended slice.
- `docs/design-docs/automation-runtime.md` - documented optional hook options,
  publish timing, and remaining service-loop ownership gaps.
- `docs/product-specs/0006-automation.md` - updated shipped prework and open
  scheduler/service-loop items.
- `docs/design-docs/memory-system.md` and
  `docs/product-specs/0005-memory-system.md` - recorded periodic advisory
  `memory_decay` from the explicit tick owner while keeping memory free of
  service ownership.
- `docs/design-docs/permissions-and-hooks.md` - documented the periodic
  producer for the shared `MemoryDecayPayload`.
- `docs/design-docs/module-boundaries.md`, `docs/ARCHITECTURE.md`, and
  `docs/BUILD_SYSTEM.md` - documented the new `oran-automation` dependency on
  `oran-hook`.
- `docs/design-docs/secrets-and-state.md` and
  `docs/design-docs/storage-runtime.md` - clarified that hook publishing does
  not move database or secret ownership.
- `docs/QUALITY_SCORE.md` - refreshed automation test counts and next step.
- `docs/releases/feature-release-notes.md` - added the slice 191 release note.
- `docs/exec-plans/completed/2026-06-07-automation-retention-cadence.md` - marked
  the hook producer milestone complete and set service-loop ownership as the
  next boundary.

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
  - Added service coverage for successful periodic `memory_decay` publishing
    and advisory sink-failure reporting.
  - Focused result: `test-automation` passed with 18 cases / 207 assertions.
  - Binary sanity: `xmake run orangutan -- --help` reports
    `orangutan v2.0.0-slice191`.
  - Base gate: `make ci` passed, including STATUS freshness and dependency
    layering checks.
  - Path hygiene: checked modified/untracked files for local absolute path
    leakage; no matches.
- Bench impact:
  - No benchmark change; this is a correctness/ownership slice, not a competing
    implementation choice.
- Compile-budget delta:
  - `oran-automation` now includes the public `oran-hook` surface and one small
    advisory publish helper; no new third-party dependency.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#automation-retention-decay-hooks`
