# Runtime Foundations Refactor

## Goal

Rebuild Orangutan's load-bearing foundations around capability-safe filesystem
handles, structured task ownership, strict provider protocol state machines,
bounded network service state, and enforced C++ CI. At the same time, shrink
the repository's documentation system from a historical narrative database to
a small set of current contracts. The refactor lands as independently green
milestones; no compatibility shim or test is kept unless it protects a real
external contract or a previously reproduced failure.

## Scope

- In scope:
  - documentation-governance simplification and deletion of redundant history;
  - dirfd-relative workspace/file authority and compare-at-commit writes;
  - structured task groups, explicit shutdown/join, and atomic cancellation
    bridges for CPU work;
  - bounded webhook/channel ownership and strict provider streaming decoders;
  - storage migration/concurrency fixes, desktop/CLI terminal correctness;
  - hosted release/debug tests, static analysis, and compile-budget truth.
- Out of scope:
  - new product surfaces, providers, channels, tools, or UI features;
  - preserving internal APIs solely for source compatibility;
  - adding third-party libraries unless measurement shows an in-repo primitive
    would be materially worse.

## Context

- Relevant docs: `docs/STATUS.md`, `docs/rules/critical-rules.md`,
  `docs/rules/async-and-concurrency.md`, specs 0012/0013, and the
  `review/deep-2026-07-11` tracker row. These are inputs, not immutable output
  requirements; milestone 1 deliberately replaces the current docs policy.
- Relevant code paths: `oran-tool` workspace/file built-ins, `oran-io`,
  `oran-agent` scheduler, `oran-async` runtime, `oran-http`, bootstrap serve
  owners, provider SSE decoders, storage migrations/pool, CLI/desktop bridges.
- Constraints: C++26/GCC 16.1, asio awaitables, `core::Result`, no new thread
  pool, no exception boundary, no weakened permission/audit behavior.
- Compile-budget impact: prefer opaque implementation state and `.cpp`-local
  Linux helpers; reject public-header-heavy generic frameworks. Measure each
  new foundation before expanding its consumers.

## Risks

- Risk: wide migration leaves two authority/lifetime models. Mitigation: each
  milestone introduces one owner API and migrates all callers before removing
  the old path.
- Risk: tests encode implementation details and obstruct simplification.
  Mitigation: retain contract, security, state-machine, and cancellation tests;
  delete duplicate shape/count tests during the owning migration.
- Risk: Linux-first secure filesystem APIs reduce portability. Mitigation:
  make Linux the hardened production backend and return an explicit unsupported
  error on platforms without an equivalent until a safe backend exists.
- Risk: documentation deletion loses rationale. Mitigation: keep current
  contracts plus this decision log; Git history remains the archive.

## Milestones

1. **Governance reset.** Remove per-slice history/test-count requirements,
   simplify current-state docs, and stop CI from requiring historical artifacts.
2. **Filesystem authority.** Add a Linux workspace root handle and dirfd-relative
   read/list/write/edit/delete/rename operations; remove path-string authority.
3. **Structured concurrency.** Add bounded task ownership and explicit join;
   migrate scheduler, runtime, hooks, HTTP CPU work, webhook/channel workers,
   and desktop sessions.
4. **Protocol and service correctness.** Strict provider state machines,
   bounded/authenticated server surfaces, channel retirement/caps, UI terminal
   semantics.
5. **Storage/conflict correctness.** Serialize migrations, fix pool cancellation,
   and move expected-version checks to commit.
6. **Enforced quality.** Hosted build/test/static analysis, sanitizer coverage,
   compile-budget measurement, and deletion of redundant code/tests/docs.

## Validation

- Commands: focused target builds/tests per milestone; `xmake test`; debug
  ASan/UBSan; analyzer builds for descriptor/parser TUs; `make ci`.
- Manual checks: workspace escape attempts, cancellation/shutdown soak,
  provider malformed-stream corpus, webhook/channel overload behavior.
- Observability checks: no lost terminal events; bounded worker/task metrics;
  errors retain target/phase/context attribution.
- Bench comparison: compile-time deltas for public surfaces; filesystem
  path-resolution/write cost; scheduler/task-group overhead where changed.

## Progress Log

- [x] 2026-07-11: deep review and slice-273 containment fixes completed.
- [x] 2026-07-11: refactor scope and milestone order established.
- [x] Governance reset landed: historical ledgers, manual quality/test totals,
  and their freshness scaffolding were removed; `make ci` now gates current contracts.
- [x] Dirfd/openat2 authority foundation landed and Workspace resolution now
  carries a capability across approval; individual file handlers are migrating.
- [x] `FileRead`, `FileWrite`, and `FileEdit` execute through pinned authorities;
  write/edit replacement commits perform final target-identity validation.
- [x] `FileDelete` materializes a pinned target capability before approval,
  executes recursive no-follow deletion through dirfds, and has no pathname API.
- [x] `oran-io` exposes pinned single-directory enumeration as the base primitive
  for migrating `DirectoryList` and `FileSearch` recursive walkers.
- [x] Non-recursive `DirectoryList` consumes pinned authority across approval;
  recursive listing remains coupled to the pathname-based ignore walker.
- [x] Atomic HTTP cancellation bridge, strict provider terminal states, bounded
  channel conversations, and bounded/deadline-limited webhook intake landed.
- [x] Bounded named `async::TaskGroup` foundation landed; runtime owners are migrating.
- [x] `Runtime::stop_and_join()` landed; start-mode serve/desktop owners now join
  Runtime workers before borrowed assembly/provider state can be destroyed.
- [x] Advisory hook fan-out now owns subscribed sink coroutines through a
  bounded `async::TaskGroup` and joins them after parent cancellation before
  releasing borrowed sink references.
- [x] `serve_channels` adapter pumps now live in a bounded `async::TaskGroup`
  and are cancelled/joined on shutdown before the owner releases the borrowed
  `ChannelManager`.
- [x] Per-conversation channel workers moved into a bounded `async::TaskGroup`
  sized by `max_active_conversations`, making the worker cap structural and the
  shutdown join explicit. Fixed a lost-wake defect the new scheduling order
  exposed: a retirement requested while the dispatcher drained an earlier one
  had its single-slot progress wake consumed by that drain, so the dispatcher
  parked forever with a worker stuck mid-retirement.
- [x] Filesystem authority migration landed: every filesystem built-in —
  recursive walks and ignore-file reads included — executes through pinned
  authorities, and the scheduler derives lock keys without re-resolving paths.
  One deliberate survivor: the read resolver keeps a `refers_to_path`-gated
  pathname pass as its symlink normaliser (see the 2026-07-12 decision below
  and the recursive-walk sub-plan decision log).
- [ ] Structured task ownership migrated across runtime surfaces.
- [ ] Network/provider/UI correctness milestone landed.
- [ ] Storage/conflict milestone landed.
- [ ] Hosted quality gates active; redundant artifacts removed.
- [ ] Full release/debug/sanitizer/analyzer verification complete.

## Decision Log

- 2026-07-11: correctness foundations take priority over new features. The
  roadmap is frozen for feature expansion until milestones 2–5 are green.
- 2026-07-11: documentation becomes current-contract-only. Slice narratives,
  duplicated test counts, release-note ledgers, and completed-plan archives are
  deletion candidates; Git history is the historical record.
- 2026-07-11: tests are selected by failure mode and contract, not assertion
  count. A smaller high-signal suite is preferable to duplicated coverage.
- 2026-07-11: secure filesystem authority is handle-based. A validated pathname
  string is never treated as authorization across an await.
- 2026-07-11: the Linux backend uses `openat2` beneath a pinned root dirfd;
  fallback may reject more symlinks but may never weaken confinement. Absolute
  symlinks are no longer a supported workspace traversal mechanism.
- 2026-07-11: detached work may not borrow owner state. Every background task
  belongs to an explicit task group whose shutdown policy is observable.
- 2026-07-11: cancellation is a request, not ownership. An owner cannot release
  state while a laggard still borrows it; hard deadlines require process
  isolation rather than optimistic coroutine races.
- 2026-07-12: amends the 2026-07-11 "absolute symlinks are no longer a
  supported workspace traversal mechanism" line. Absolute-target symlinks
  remain readable through the read/list resolvers' pathname normalisation
  pass (spec 0013 AC2) while the root pathname still names the pinned
  directory, because `RESOLVE_BENEATH` cannot follow them and the
  normaliser's output is re-anchored beneath the pinned root before
  execution — no confinement authority flows from the pathname. Anchored
  execution itself still rejects absolute symlinks everywhere else: nested
  walk entries, matched-file opens, ignore-file loads, mutations, and the
  renamed-root read fallback.

## Linked Artifacts

- Related review backlog: `review/deep-2026-07-11`.
- Prior containment commit: `8b66c8d`.
- Histories/release notes: intentionally not required after milestone 1.
