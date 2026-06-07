# Automation Retention Cadence

## Goal

Land the first real `oran-automation` runtime boundary by turning periodic
cadence decisions into deterministic, tested code, then giving the stored
retention descriptor a durable state home. The plan's current end state is a
small automation library that can evaluate periodic jobs, derive the long-term
memory retention decay request from a retention policy, persist retention
job/run state, and later grow into the cron/periodic/triggered service without
moving scheduling math or service ownership into bootstrap or `oran-memory`.

## Scope

- In scope:
  - Add the `oran-automation` library, umbrella header, test bucket, and bench
    bucket parity.
  - Ship a deterministic `PeriodicSchedule` / `PeriodicJobState` evaluator for
    due-at, due/not-due, and overdue metadata.
  - Ship a memory-retention job planner that produces a
    `memory::longterm::DecayRequest` only when the periodic cadence is due.
  - Keep `decay_check_interval_hours` represented as automation cadence input.
  - Ship the first automation-owned retention repository for job/run/last-fired
    state above `storage::Pool`.
  - Update docs, status, quality, history, and release notes per the Prime
    Directive.
- Out of scope:
  - Cron parsing, triggered jobs, per-agent leases, queueing/backpressure,
    notifier callbacks, and background service loops.
  - Bootstrap ownership of the periodic runner or bootstrap opening
    `automation.db`.
  - Periodic `memory_decay` hook publication; that lands when the periodic
    producer actually runs decay.

## Context

- Relevant docs:
  - `docs/product-specs/0006-automation.md`
  - `docs/design-docs/automation-runtime.md`
  - `docs/design-docs/memory-system.md`
  - `docs/product-specs/0005-memory-system.md`
  - `docs/design-docs/module-boundaries.md`
  - `docs/rules/docs-in-sync.md`
  - `docs/rules/testing-and-bench.md`
- Relevant code paths:
  - `include/oran/memory/longterm.hpp`
  - `include/oran/config/config.hpp`
  - `xmake/targets.lua`
  - `xmake/tests.lua`
  - `xmake/bench.lua`
- Constraints:
  - `oran-automation` is an agent-runtime layer library and may depend downward
    on `oran-core`, `oran-async`, `oran-storage`, and `oran-memory`;
    `oran-memory` and `oran-storage` must not depend upward on automation.
  - Public headers must stay third-party-free and avoid owning asio/sqlite
    types.
  - The first slice must not create a hidden background loop inside bootstrap.
  - Periodic memory decay remains metadata-only until a producer publishes
    `memory_decay`.
- Compile-budget impact:
  - `oran-automation` uses stdlib plus public `oran-core`, `oran-async`,
    `oran-storage`, and `oran-memory` surfaces in this plan. It should fit the
    existing orchestration/automation budget row in
    `docs/rules/compile-budget.md`.

## Risks

- Risk: The first slice accidentally looks like a complete scheduler.
  Mitigation: Keep names explicit (`PeriodicSchedule`, `plan_memory_retention`)
  and document cron/triggered/persistence as downstream.
- Risk: Retention request generation drifts from bootstrap startup decay.
  Mitigation: Use the same `memory::longterm::DecayRequest` shape and the same
  policy fields: scope, unused cutoff, importance floor, max records, and
  decay time.
- Risk: Adding a new library expands docs/build churn.
  Mitigation: Keep the implementation narrow, add focused tests, add a tiny
  bench bucket only for target parity, and update the routing docs in the same
  commit.

## Milestones

1. **Library shell and cadence evaluator.**
   Add `oran-automation`, periodic cadence API, tests, bench parity, and docs.
2. **Memory retention planner.**
   Produce due-only `DecayRequest` values from retention policy input and
   periodic state.
3. **Bootstrap/runner ownership.**
   Done in slice 188: map configured-route `memory.longterm.retention` into an
   automation-owned periodic job descriptor without creating a background loop
   in `RuntimeAssembly::build`.
4. **Persistent state.**
   Done in slice 189: add `automation.db` job/run rows and
   `AutomationRepository` without bootstrap opening the database or starting a
   service.
5. **Explicit service/tick owner.**
   Done in slice 190: consume stored jobs, evaluate due work, call the memory
   backend, and record run outcomes without hidden bootstrap loops.
6. **Hook producer.**
   Later slice: publish periodic `memory_decay` metadata from the actual
   periodic producer.

## Validation

- Commands:
  - `xmake build test-automation`
  - `xmake run test-automation`
  - `xmake build bench-automation`
  - `xmake run bench-automation`
  - `xmake build oran-automation`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `git diff --check`
  - `make ci`
- Manual checks:
  - Confirm no bootstrap background loop is introduced.
  - Confirm `oran-memory` dependency direction does not change.
- Observability checks:
  - No new hook publish is claimed until the periodic producer exists.
- Bench comparison:
  - First bucket only pins cadence-evaluation overhead for target parity; the
    service tick benchmark from spec 0006 remains downstream.

## Progress Log

- [x] 2026-06-07 01:20 +0800: Confirmed from `STATUS.md`,
  `memory-system.md`, and spec 0006 that periodic retention cadence is the next
  memory capability gap after startup `memory_decay`.
- [x] 2026-06-07 01:20 +0800: Scoped the first automation slice to deterministic
  periodic cadence and memory-retention request planning; full scheduler,
  persistence, leases, and hook producer remain downstream.
- [x] 2026-06-07 01:45 +0800: Added the `oran-automation` public API,
  implementation, tests, bench bucket, and xmake targets for deterministic
  periodic schedule evaluation plus memory-retention request planning.
- [x] 2026-06-07 01:58 +0800: Updated architecture, automation design/spec,
  memory docs, build/README routing, status, quality, history, and release
  notes to describe the shipped planner and keep scheduler/DB/hook producer
  work downstream.
- [x] 2026-06-07 04:24 +0800: Added the bootstrap-owned config-to-retention-job
  mapping helper, stored the descriptor on `RuntimeAssembly`, and derived
  startup decay options from the same job so startup retention and future
  periodic retention share one policy shape.
- [x] 2026-06-07 04:24 +0800: Confirmed the slice does not start a background
  scheduler, open `automation.db`, persist job state, or publish periodic
  `memory_decay`; those remain service-owner work.
- [x] 2026-06-07 05:08 +0800: Added `AutomationRepository` over
  `storage::Pool` plus an embedded `automation.db` retention migration for
  durable job, `last_fired_at`, and run-row state keyed by `job_key`.
- [x] 2026-06-07 05:08 +0800: Kept bootstrap unopened for `automation.db`; the
  next useful product boundary is an explicit service/tick owner that consumes
  the stored jobs and records actual backend run outcomes.
- [x] 2026-06-07 09:59 +0800: Added `MemoryRetentionService::tick(...)` as the
  explicit caller-driven owner for one stored retention job. The tick reuses
  the planner, invokes a supplied long-term backend only when due, records
  success/failure rows, and advances `last_fired_at` only after success.
- [x] 2026-06-07 09:59 +0800: Kept bootstrap unopened for `automation.db` and
  kept periodic `memory_decay` publishing out of the tick slice; the next useful
  product boundary is hook production from the actual periodic execution owner.
- [x] Update docs that this slice invalidates in the same PR
  (`docs/rules/docs-in-sync.md`).
- [x] Run validation and record results.
- [x] Write history entry and release note.

## Decision Log

- 2026-06-07: Start with cadence/request planning, not cron or `automation.db`.
  The retention policy already has a shipped interval field and decay request
  shape; a deterministic planner gives later scheduler code a stable contract
  without adding an unowned background service.
- 2026-06-07: Keep periodic `memory_decay` publishing out of the first
  automation slice. Startup publishing is already real; periodic publishing
  should land only when a periodic producer actually invokes decay.
- 2026-06-07: Let bootstrap map `memory.longterm.retention` into
  `MemoryRetentionJob`, because bootstrap is the composition root that can see
  both config and automation. Keep `oran-automation` independent of
  `oran-config`, and set the configured-route job's first fire after the
  startup decay pass so a future scheduler does not immediately repeat it.
- 2026-06-07: Keep automation persistence in `oran-automation`, not
  `oran-storage`. Storage remains the generic SQLite/migration/pool substrate;
  the retention job/run schema belongs to the automation domain.
- 2026-06-07: Use durable `job_key` as the repository identity and keep
  `scope_key` as the memory-decay scope inside `MemoryRetentionJob`, so future
  automation jobs can share a memory scope without overwriting each other.
- 2026-06-07: Advance `last_fired_at` to the scheduled fire time only after a
  successful backend decay and run-row insert. Backend errors record a failed
  run and leave state unadvanced so retry/catch-up policy remains explicit.
- 2026-06-07: Keep `MemoryRetentionService` as a caller-driven tick rather than
  a hidden service loop. Timers, leases, shutdown, notifier routing, and
  periodic `memory_decay` hook publication belong to the future loop owner.

## Linked Artifacts

- Related design doc: `docs/design-docs/automation-runtime.md`
- Related design doc: `docs/design-docs/memory-system.md`
- Related product spec: `docs/product-specs/0006-automation.md`
- Related product spec: `docs/product-specs/0005-memory-system.md`
- PRs: TBD
- History entry:
  `docs/histories/2026-06/20260607-0129-automation-retention-cadence.md`
  and
  `docs/histories/2026-06/20260607-0424-bootstrap-retention-job.md`
  and
  `docs/histories/2026-06/20260607-0508-automation-retention-state.md`
  and
  `docs/histories/2026-06/20260607-0959-automation-retention-service-tick.md`
- Release note:
  `docs/releases/feature-release-notes.md#automation-retention-service-tick`
