## [2026-05-28 00:40] | Task: slice 117 — ToolScheduler per-path lock table

### Execution Context

- Agent: Claude Opus 4.7
- Base model: claude-opus-4-7
- Runtime: Claude Code (single-session implementation)
- Linked plan: [`docs/exec-plans/active/2026-05-27-tool-scheduler-v1.md`](../exec-plans/active/2026-05-27-tool-scheduler-v1.md)
  — second of five slices (116-120). Spec
  [`docs/product-specs/0012-tool-scheduler-and-state.md`](../product-specs/0012-tool-scheduler-and-state.md)
  carries the contract.

### User Query

> 深度了解项目架构，了解当前项目实现进度。在进一步进行代码实现前，必须充分阅读理解所有相关文档，
> 始终保持深度思考(ultrathink)…继续执行此plan @docs/exec-plans/active/2026-05-27-tool-scheduler-v1.md

Slice 117 of the spec-0012 ToolScheduler arc adds the per-canonical-path
read/write lock table required by AC3, AC4, and AC10. Cancellation
guarantee (AC5), audit fan-out (AC7), and loop wiring + bench (AC12) stay
staged for slices 118-120.

### Changes Overview

- Areas: `oran-agent` (new private impl + public Stats / reap surface +
  scheduler integration + tests).
- Key actions:
  - Add `src/oran-agent/_impl/path_lock_table.hpp` /
    `src/oran-agent/path_lock_table.cpp`: the new `agent::detail::PathLockTable`
    with `PathLockMode { shared, exclusive }`, move-only `PathLockGuard`
    (RAII release on destruction), `PathLockTableOptions { idle_ttl }`,
    and `PathLockTableStats { shared_acquires, exclusive_acquires,
    contended_acquires, cancelled_acquires, reaped_entries,
    current_entries, peak_entries }`. Acquire / release / reap discipline
    is single-strand by contract (mirroring `core::BoundedCache`):
    `acquire` either takes the lock immediately or queues a waiter on a
    private `async::Channel<std::monostate>(executor, 1)`. Release
    wakes the next waiter and pre-increments the reader/writer counter
    on the wakee's behalf so a fresh acquire racing the wake sees the
    busy state. Cancellation during wait is reconciled: if the cancelled
    waiter is still in the queue, it is erased; if a release has already
    pre-incremented and removed it, the cancel arm undoes that increment
    and forwards the wake to the next waiter so the chain does not stall.
    The map uses transparent string hashing (`is_transparent` +
    `std::equal_to<>`) so `string_view` lookups in release avoid an
    allocation. `reap` walks entries that became idle past `idle_ttl`
    and erases them; entries with active holders or queued waiters are
    skipped.
  - Update `include/oran/agent/scheduler.hpp`: add `ToolSchedulerLockStats`
    public POD (no private inheritance from detail), grow `ToolScheduler`
    with `lock_stats()` and `reap_idle_locks(core::Time)`, pull in
    `<cstdint>` and `<oran/core/time.hpp>` for the new surface.
  - Update `src/oran-agent/scheduler.cpp`: add `classify_lock_mode(caps)`
    that turns `core::Capability::write_file`/`edit_file`/`delete_path`
    into exclusive, `read_file`/`list_directory` into shared, and
    everything else into "no lock". Add `derive_lock_key(workspace,
    input_json, mode, caps)` that parses the JSON `path` field
    privately via `nlohmann::json` (oran-agent already has the private
    dep) and routes it through the matching `Workspace::resolve_*`
    so the lock key is the canonical absolute path. `Impl::run_call`
    now acquires a `PathLockGuard` between the semaphore receive and
    the dispatch race; on parent-cancelled acquire the per-call result
    becomes `Error::cancelled` with `reason=parent_cancelled` and the
    completion still fires. `Impl::lock_stats()` projects the inner
    `PathLockTableStats` into the public `ToolSchedulerLockStats` shape
    and `reap_idle_locks(now)` forwards to the table.
  - Extend `tests/agent/test_scheduler.cpp` with eight new cases:
    AC3 (two writes to same canonical path serialize via the exclusive
    lock; peak in-flight = 1; second acquire counts as contended),
    AC4 (concurrent read + write on the same path do not overlap;
    peak in-flight = 1; one shared + one exclusive acquire counter),
    shared-lock parallel-read (two reads to same path; peak in-flight
    = 2; both shared acquires; zero contention), distinct-path
    parallel-writes (peak in-flight = 2; two exclusive acquires;
    `current_entries == 2`), capability-free fall-through (no lock
    taken — counters stay 0 and current_entries == 0),
    `reap_idle_locks` drops entries past the TTL while leaving entries
    within TTL alone, AC10 (`PathLockTable` direct test: 10 000 distinct
    paths acquire/release; reap with `now + idle_ttl + 1s` evicts
    every entry; `stats().current_entries == 0`,
    `stats().reaped_entries == 10 000`), and a cancellation-during-wait
    case that holds an exclusive lock, queues + cancels a second
    exclusive waiter, then verifies that a fresh acquire on the same
    key succeeds after the original holder releases (no orphaned
    permit and `stats().cancelled_acquires >= 1`).

### Design Intent

The hard call was where to put the lock-key derivation. Two viable
options: (a) reach into `Registry::dispatch`'s pre-resolution path so
the scheduler and the registry agree on the canonical path, or
(b) re-resolve in the scheduler. (a) couples the scheduler to a
private `_impl/path_resolution.hpp` from `oran-tool`, and the registry
sets `resolved_path` *during* dispatch — which is after the scheduler
needs to know the key. (b) costs one extra `Workspace::resolve_*` per
call, but the cost is in microseconds against a tool call that's
already racing a 60 s timeout, and the scheduler keeps a clean
boundary. Picked (b). Hook-rewrite mid-dispatch could in principle
produce a different effective path than the lock key; for v1 that's a
documented corner case — the spec calls out hook rewrites as advisory
and not the workload the lock table protects against.

Cancellation while waiting is the other subtle bit. A naive design
treats the wait channel as the source of truth: cancel → channel
returns Err → return. But `release_locked` may have already pushed a
permit on our channel before our receive cancelled, in which case the
permit sits in the channel's `values_` forever and the reader/writer
counter shows us as a holder. The fix is the two-case reconciliation
in the cancel arm: if we are still in the queue, just erase; if we
are not, the table has already booked us as a holder, so we must
undo that booking and pass the wake along. Without this, two
back-to-back cancel events on contended locks would orphan the entry
permanently.

The lock key is the workspace-resolved absolute path string. This
matches the registry's own pre-resolution canonical path for the
common case (no hook rewrite), so two LLM calls that name the same
target with different surface spellings (`./foo`, `foo`, an absolute
path inside the workspace) end up on the same lock entry. Tools that
declare no filesystem capability — `tool.search` today, future
memory tools — skip path locking entirely and run under the existing
bounded-parallelism slot only. The spec's globally-serialised tools
(`shell.exec`, `agent.spawn`, `tool.runtime_loader`) stay unbounded
until a later slice gives them their own slot type.

`PathLockTable` is single-strand by contract, exactly like
`core::BoundedCache`. The scheduler drives every acquire/release/reap
on `state->executor` (the agent strand) so no internal mutex is
needed. If a future caller hops the table to a different strand,
they wrap it; we did not pay the mutex cost up front for a single
caller.

### Files Modified

- `include/oran/agent/scheduler.hpp` — add `ToolSchedulerLockStats`,
  `ToolScheduler::lock_stats()`, `ToolScheduler::reap_idle_locks(core::Time)`,
  pull in `<cstdint>` + `<oran/core/time.hpp>`, refresh the file
  preamble to note that slice 117 ships the lock table.
- `src/oran-agent/scheduler.cpp` — add `classify_lock_mode` +
  `derive_lock_key` helpers, take the new `nlohmann/json` + workspace
  + capability headers, integrate `detail::PathLockTable` into
  `Impl::run_call`, forward `lock_stats()` / `reap_idle_locks(now)`
  through the pimpl.
- `src/oran-agent/_impl/path_lock_table.hpp` — **new** private header
  exporting `PathLockMode`, `PathLockGuard`, `PathLockTable`,
  `PathLockTableOptions`, `PathLockTableStats`.
- `src/oran-agent/path_lock_table.cpp` — **new** implementation.
- `tests/agent/test_scheduler.cpp` — extend with the eight slice-117
  cases listed above; refresh the file preamble; pull in
  `<asio/detached.hpp>`, `<filesystem>`, `<fstream>`, `<mutex>`,
  `<oran/core/time.hpp>`, and the private path-lock header via the
  same `"../../src/oran-agent/_impl/..."` pattern `tests/tool/test_parse_input.cpp`
  already uses.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 117; `Last completed history`
  points at this file; refreshed the `oran-agent` line in
  `Latest Library Surfaces` (32 / 462 → 40 / 10 545); wrote a
  slice-117 next-intended-slice paragraph that summarises the lock
  table, stats, classification rule, and slice-118 staging.
- `docs/ARCHITECTURE.md` — extended the `oran-agent` row with a
  `(slice 117)` paragraph naming the lock table file path, the
  classification, the lock-key derivation, the FIFO + writer-priority
  semantics, the cancellation reconciliation, the public
  `ToolSchedulerLockStats` + `reap_idle_locks` accessors, and the
  remaining staged items.
- `docs/design-docs/tool-runtime.md` — promoted the "Scheduler Boundary"
  subsection from forward-looking (slice 116) to slice-117 status:
  the lock table now ships, with the classification, lock-key, FIFO
  semantics, cancellation reconciliation, and `ToolSchedulerLockStats`
  fields documented.
- `docs/exec-plans/active/2026-05-27-tool-scheduler-v1.md` — checked
  off the slice-117 lock table + docs bullets in the progress log
  with a one-paragraph implementation-choice summary so the next
  agent does not re-derive the design.

### Validation

- Commands run:
  - `xmake build oran-agent` — succeeded.
  - `xmake build test-agent` — succeeded.
  - `xmake build test-tool test-bootstrap` — succeeded (unchanged libraries
    but rebuilt as a smoke check).
  - `xmake test` — **14 / 14 test suites passed.**
  - `./build/linux/x86_64/release/test-agent --reporter=console
    --verbosity=normal` — **All tests passed (10 545 assertions in
    40 test cases).**
  - `./build/linux/x86_64/release/test-tool --reporter=console
    --verbosity=normal` — All tests passed (1 866 assertions in 185
    test cases) (unchanged).
  - `./build/linux/x86_64/release/test-bootstrap --reporter=console
    --verbosity=normal` — All tests passed (316 assertions in 72 test
    cases) (unchanged).
- Tests added/changed:
  - `tests/agent/test_scheduler.cpp` — 8 new cases / +10 083 assertions
    (the AC10 case dominates with 10 000 acquire-and-release REQUIREs
    inside one Catch2 case).
- Bench impact: none in slice 117; `bench/agent/scheduler_overhead` and
  `scheduler_audit_fanout` still land in slice 120 alongside the loop
  wiring so the bench measures the production call path, not a
  half-built abstraction.
- Compile-budget delta: not measured this slice — the new TU
  (`src/oran-agent/path_lock_table.cpp`) is ~180 lines and the public
  scheduler header only grew by two trivial accessors and a small POD;
  the heavy `nlohmann/json` include stays private to `scheduler.cpp`.
  Slice 120 will rebench `oran-agent` once the loop consumes the
  scheduler.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none added. Slice 118's approval / audit fan-out
  work stays inside the active arc, not the tracker.
- Linked release note: none — internal abstraction; user-visible
  parallel tool dispatch lands in slice 120 with the release note.
