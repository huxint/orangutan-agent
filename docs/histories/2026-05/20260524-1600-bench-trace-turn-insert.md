## [2026-05-24 16:00] | Task: Per-insert `trace_turn_insert` bench + adjacent `id_for` fix

### Execution Context

- Agent: `Claude`
- Base model: `Opus 4.7`
- Runtime: `orangutan-refactor` local workspace
- Linked plan: none — focused continuation of spec 0018 acceptance criterion 12.

### User Query

> Continue deeply understanding the project architecture and implementation
> progress, then keep advancing the project code implementation.

### Changes Overview

- Areas: `bench/storage`, spec-0018 acceptance criteria, docs/history/version.
- Key actions:
  - Added `bench/storage/scenarios/trace_turn_insert.cpp` — a per-insert
    A-vs-B pair against `trace_turns`. Each nanobench iteration performs
    exactly one insert so the reported "ns per batch" reads directly as the
    per-turn observability cost called out by spec 0018 AC12 (≤ 50 µs /
    insert). The raw scenario goes through `Pool` + `StatementCache` with a
    constexpr INSERT and four bind sites (turn_id, session_id, started,
    finished); the repository scenario goes through
    `TraceRepository::append_turn` with a full `AppendTraceTurnRequest`. Both
    scenarios open their own temp DB so namespaces stay isolated and the
    benches do not race each other on PRIMARY KEY collisions.
  - Per-iteration turn ids are unique by packing the iteration counter into
    the trailing 8 bytes of the 16-byte BLOB and a namespace tag into the
    leading 8 bytes. That lets nanobench advance through arbitrarily many
    epochs without ever re-inserting the same key.
  - Adjacent bug fix in `bench/storage/scenarios/trace_repository.cpp`:
    the existing `id_for(batch, row, salt)` helper computed each byte as
    `salt + row + i + (batch & 0x0f)`. That overlapping sum collided across
    `(batch, row)` tuples — for example `(0, 1)` and `(1, 0)` both
    produced bytes `0x11 + i`. The first nanobench epoch inserted batch 0's
    rows successfully; the second epoch hit batch 1, row 0 and the trace
    `PRIMARY KEY` guard aborted the bench (`sqlite statement step failed`),
    which is why the existing trace_repository scenarios had been silently
    aborting `bench-storage` ever since slice 78. The fix swaps the
    overlapping-sum encoding for a non-overlapping byte layout: byte 0
    carries the namespace salt, bytes 1-4 carry the row counter (LE
    `std::uint32_t`), bytes 8-15 carry the batch counter via `std::memcpy`.
    Every `(salt, row, batch)` tuple now maps to a distinct BLOB, and both
    batch scenarios run to completion alongside the new single-insert pair.
  - Wired the new scenario into `bench/storage/main.cpp` (forward
    declaration + register call) and documented it in
    `bench/storage/README.md` next to the batch scenario row.
  - Bumped `kVersion` to `2.0.0-slice89` in `src/oran-bootstrap/bootstrap.cpp`.

### Design Intent

Slice 88 closed spec-0018 AC10 (the CLI inspector) and left three downstream
items: hook publish rows (AC5), the `trace_turn_insert` bench (AC12), and the
binary handoff that drives `agent::Loop` from inside the `orangutan` binary.
AC5 is blocked on spec 0015's blocking-veto hook semantics, which is still
deferred (see `STATUS.md` tech-debt row from 2026-05-18). The binary handoff
needs a real provider adapter — only `FakeProvider` ships today, and the
multi-slice transport/protocol/retry work is the actual blocker. AC12 is the
cleanest unblocked item: it has no external dependencies, the storage surface
is already shipped, and `bench/storage/scenarios/trace_repository.cpp`
already exists as a sibling pattern.

The new scenario is intentionally a **single insert per iteration**, not a
batch. AC12 spells the metric as "≤ 50 µs per insert"; a batch bench (the
existing 32-row scenario) only reports a per-batch number that operators
must divide by hand, and the trailing `count_turns` query in the batch
scenario dilutes the per-row signal. The single-insert shape makes the spec
number read directly off the nanobench output.

The id collision was discovered while validating AC12: running the freshly
built `bench-storage` produced SIGABRT after the audit scenarios. Tracing
the abort through `std::println(stderr, ...)` instrumentation in the
`migrate` and `run_raw_pool_insert` paths showed the failure at
`(batch=1, row=0)` with `sqlite statement step failed`. Inspecting the
existing `id_for` revealed the overlapping-sum encoding had never produced
unique ids — the bench had been silently broken since slice 78, but nothing
ran it (CI doesn't gate on `xmake run bench-storage`, and prior trace
slices verified correctness via `xmake run test-storage` instead). Fixing
the bug is in-scope here because AC12 verification depends on the bench
running to completion; leaving it broken would mean my new scenario also
never runs (its registration is after `register_trace_repository` in
`main.cpp`).

Both scenarios share a per-bench temp DB rather than a single shared one
because (a) it matches the audit/session pattern for isolated repository
benches and (b) it removes any cross-scenario coupling so the numbers stay
attributable. Total bench-time cost is two extra migrations at registration
plus one temp file per scenario; both are paid once and reused across
thousands of iterations.

### Files Modified

- `bench/storage/scenarios/trace_turn_insert.cpp` (new)
- `bench/storage/scenarios/trace_repository.cpp` (id_for fix + `<cstring>`)
- `bench/storage/main.cpp`
- `bench/storage/README.md`
- `src/oran-bootstrap/bootstrap.cpp` (version bump)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 89, pointed at this history, recorded
  the bench scenarios + the id_for fix as the closed AC12 item, and updated
  the remaining downstream items (hook publish rows + binary handoff).
- `docs/product-specs/0018-first-loop-observability.md` — flipped AC12 to
  shipped status with the scenario names and the initial WSL2 numbers.
- `docs/releases/feature-release-notes.md` — added the slice 89 release note.

### Validation

- Commands run:
  - `xmake build bench-storage`
  - `xmake run bench-storage` (now completes cleanly; produces numbers for
    every scenario including the new pair)
  - `xmake run test-storage` (no regression: 72 cases / 886 assertions
    still pass)
- Bench numbers (WSL2 / Linux 6.6 / nanobench with the project's
  `minEpochIterations(1000)` + `warmup(100)`):
  - `storage.trace_turn_insert_raw_pool` — about 12.7 µs / insert
  - `storage.trace_turn_insert_repository` — about 15.9 µs / insert
  - `storage.trace_raw_pool_insert` (existing 32-row batch) — about
    47 µs / insert amortised
  - `storage.trace_repository_insert` (existing 32-row batch) — about
    123 µs / insert amortised (the batch scenario includes a trailing
    `count_turns` query per iteration, so it is not directly comparable
    to the single-insert pair).
- Tests added/changed: none — bench-only slice.
- Compile-budget delta: not measured; one new TU in an existing bench
  bucket, no new public templates.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: hook publish rows (AC5) and binary handoff remain the
  named downstream items on spec 0018. `--trace-export` (JSON Lines, spec
  0018 v1.1) is still untouched. The bench numbers were captured in WSL2,
  not the reference 8-core / NVMe / native Linux hardware the compile
  budget targets — once that hardware lands, AC12's "≤ 50 µs" claim should
  be re-verified there.
- Linked release note:
  [`docs/releases/feature-release-notes.md`](../../releases/feature-release-notes.md)
