# Current State

Read this first, then the relevant row in [`ROADMAP.md`](ROADMAP.md) and the
module-specific contract routed from [`../AGENTS.md`](../AGENTS.md).

## Current Focus

- The runtime foundations refactor is active:
  [`exec-plans/active/2026-07-11-runtime-foundations-refactor.md`](exec-plans/active/2026-07-11-runtime-foundations-refactor.md).
- New feature work is paused while correctness foundations are repaired.
- The QQ credentialed smoke remains externally blocked and is tracked in
  [`exec-plans/active/2026-06-10-channel-qq-port.md`](exec-plans/active/2026-06-10-channel-qq-port.md).
- The executable currently reports slice 273. Slice numbers are compatibility
  metadata, not a documentation workflow.

## Verified Baseline

At commit `8b66c8d`, the release build, all test buckets, binary help smoke, and
`make ci` passed. That change fixed scheduler cancellation-laggard state lifetime,
newer-libcurl WebSocket pong handling, unauthenticated public webhook binds, and
raw `MemoryRemember` input exposure to non-trusted hooks.

Do not copy test/assertion totals into this file. Test discovery and CI results are
the authoritative inventory.

## Priority Risks

The canonical ranked backlog is
[`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md), row
`review/deep-2026-07-11`. The refactor addresses these groups in order:

1. Handle-based workspace confinement across asynchronous operations —
   complete: every filesystem built-in (recursive walks and ignore-file reads
   included) executes through pinned authorities and the scheduler derives
   lock keys without re-resolving paths (runtime-foundations milestone 2).
2. Structured ownership, bounded channel workers, cancellation, and shutdown.
3. HTTP/webhook connection bounds and protocol state-machine correctness.
4. Storage migration/pool/write-conflict correctness.
5. Enforced hosted quality gates and removal of low-signal tests/code.

## Maintenance Rule

Update this file only when the current focus, verified baseline, or priority risks
materially change. Per-change narratives, manual test totals, and release ledgers do
not belong here; Git and CI already own them.

## See Also

- [`ROADMAP.md`](ROADMAP.md) — subsystem frontiers and dependencies.
- [`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md) — live debt.
- [`rules/docs-in-sync.md`](rules/docs-in-sync.md) — current-contract discipline.
