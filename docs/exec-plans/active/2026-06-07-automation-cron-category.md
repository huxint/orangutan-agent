# Automation Cron Category

## Goal

Land the first cron-category planning boundary for `oran-automation` without
starting a scheduler service. The plan starts with a deterministic POSIX
5-field cron parser/evaluator that returns the next due fire for a caller-owned
clock and stored state, then leaves persistence, service-loop startup, queues,
notifiers, and agent firing for later scheduler slices.

## Scope

- In scope:
- Add a public cron schedule shape and evaluator in `oran-automation`.
- Support POSIX-style 5-field expressions (`minute hour day-of-month month
  day-of-week`) with `*`, lists, ranges, and steps.
- Keep evaluation deterministic and caller-clocked, with no database, backend,
  hook, agent, or background task ownership.
- Update automation docs/status/history/release notes in the same slice.
- Out of scope:
- Cron-authored config fields or a scheduler service that reads config.
- Triggered jobs, queueing/backpressure, notifier routing, and agent firing.
- Bootstrap opening `automation.db`, starting timers, or spawning detached
  automation work.
- Scheduler tick performance work beyond focused correctness coverage.

## Context

- Relevant docs:
- `docs/STATUS.md`
- `docs/product-specs/0006-automation.md`
- `docs/design-docs/automation-runtime.md`
- `docs/rules/docs-in-sync.md`
- `docs/rules/testing-and-bench.md`
- Relevant code paths:
- `include/oran/automation/periodic.hpp`
- `src/oran-automation/periodic.cpp`
- `tests/automation/test_periodic.cpp`
- `include/oran/automation.hpp`
- Constraints:
- Keep `oran-automation` independent of `oran-config`, `oran-agent`, and
  bootstrap scheduling ownership.
- Public headers must stay lightweight and third-party-free.
- Cron evaluation must not skip/coalesce missed firings; later service policy
  decides catch-up/drop behavior.
- Compile-budget impact (if any):
- Implementation stays in the existing `periodic.cpp` translation unit and
  adds only standard-library parsing helpers.

## Risks

- Risk: cron parsing becomes a hidden scheduler. Mitigation: expose only
  deterministic evaluation over caller-supplied `now` and state.
- Risk: day-of-month/day-of-week semantics are ambiguous. Mitigation: document
  the POSIX/Vixie-style OR behavior when both fields are restricted.
- Risk: impossible schedules cause unbounded scans. Mitigation: bound the
  search window and return a validation error when no matching fire is found.

## Milestones

1. **Cron evaluator.**
   Add `CronSchedule` and `evaluate_cron_schedule(...)` with parser and focused
   tests for exact fires, future fires, stored state, steps/lists/ranges, and
   validation.
2. **Scheduler/category owner.**
   Later: persist configured cron jobs and layer them into the explicit
   automation runtime without bootstrap-owned background work.
3. **Triggered/notifier/queue policy.**
   Later: add triggered categories, queueing/backpressure, notifier routing,
   and agent execution leases.
4. **Scheduler performance.**
   Later: measure the 1 000-job scheduler tick criterion once the scheduler
   exists.

## Validation

- Commands:
- `xmake build test-automation`
- `xmake run test-automation`
- `xmake build oran-automation`
- `xmake build orangutan`
- `xmake run orangutan -- --help`
- `git diff --check`
- `make ci`
- Manual checks:
- Confirm cron evaluator is pure and caller-clocked.
- Confirm bootstrap still does not open or run `automation.db`.
- Confirm no new dependency direction crosses from automation into bootstrap,
  config, or agent.
- Observability checks:
- Not applicable in the first cron evaluator slice; no hook events are emitted.
- Bench comparison (if perf-relevant):
- Not perf-relevant until a scheduler tick loop exists.

## Progress Log

- [x] 2026-06-07 17:56 +0800: Selected cron-category planning as the next
  spec-0006 boundary after the retention loop reached the explicit runtime
  policy layer. This slice stays pure and does not introduce scheduler startup.
- [x] 2026-06-07 18:00 +0800: Implemented `CronSchedule` plus
  `evaluate_cron_schedule(...)` as a caller-clocked POSIX 5-field UTC
  evaluator with `*`, lists, ranges, steps, Sunday `0`/`7`, DOM/DOW OR
  semantics, bounded impossible-schedule scans, and no persistence or runtime
  service ownership.
- [x] **Update the docs that this slice invalidates in the same PR**
  (`docs/rules/docs-in-sync.md`).
- [x] Run validation and record results.
- [x] Update `docs/QUALITY_SCORE.md` row.
- [x] Write history entry.
- [x] Add release note.

## Decision Log

- 2026-06-07: Start cron work with parsing/evaluation, not service startup.
  The cron acceptance criterion needs a deterministic schedule primitive before
  a process service loop can route jobs, and a pure evaluator keeps bootstrap
  free of hidden automation ownership.

## Linked Artifacts

- Related design doc: `docs/design-docs/automation-runtime.md`
- Related product spec: `docs/product-specs/0006-automation.md`
- PRs:
- History entry:
- `docs/histories/2026-06/20260607-1800-automation-cron-schedule.md`
- Release note:
- `docs/releases/feature-release-notes.md#2026-06`
