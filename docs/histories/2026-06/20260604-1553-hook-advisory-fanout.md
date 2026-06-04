## [2026-06-04 15:53] | Task: Hook advisory fan-out

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `OpenAI API / local xmake`
- Linked plan: none — small debt-tracker cleanup slice.

### User Query

> Deeply understand the project architecture and current implementation progress
> before coding, use the repository docs plus codegraph MCP, then take the next
> grounded small implementation slice.

### Changes Overview

- Areas: `oran-hook`, hook docs/status, deep-review tech-debt tracker.
- Key actions:
  - Changed `hook::Bus::publish_advisory` from sequential sink awaits to
    concurrent sibling child coroutines.
  - Preserved subscription-ordered `PublishOutcome` rows despite out-of-order
    completion.
  - Kept advisory errors non-vetoing and left blocking hook publishes
    sequential/short-circuiting.
  - Added a regression that observes overlapping sink callbacks rather than
    asserting on wall-clock thresholds.

### Design Intent

This closes the deep-review P2 parallel advisory fan-out item without changing
the blocking-decision contract in
[`docs/product-specs/0015-blocking-hook-decisions.md`](../../product-specs/0015-blocking-hook-decisions.md).
Advisory hooks are observation-only, so independent sinks can run concurrently;
blocking hooks still need subscription-order short-circuit semantics because the
first non-`proceed` decision changes dispatch flow.

The implementation uses the existing async style: child coroutines report rows
through bounded `async::Channel`, parent cancellation emits child cancellation
signals, and the parent drains completions before returning so sink lifetimes
remain tied to the publish await.

### Files Modified

- `src/oran-hook/bus.cpp`
- `include/oran/hook/bus.hpp`
- `include/oran/hook/sink.hpp`
- `tests/hook/test_bus.cpp`
- `bench/hook/scenarios/bus.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/permissions-and-hooks.md` — advisory delivery is now
  concurrent fan-out with ordered gather; benchmark numbers refreshed.
- `docs/product-specs/0015-blocking-hook-decisions.md` — advisory fan-out marked
  shipped while blocking semantics remain sequential.
- `docs/ARCHITECTURE.md` — hook library inventory updated for concurrent
  advisory delivery.
- `docs/exec-plans/tech-debt-tracker.md` — closed the parallel
  `publish_advisory` fan-out debt item.
- `docs/releases/feature-release-notes.md` — recorded the embedder-visible
  advisory hook delivery change.
- `docs/STATUS.md` — advanced to slice 156, added focused validation and bench
  results, and refreshed the open-debt summary.
- `docs/QUALITY_SCORE.md` — refreshed hook test count and hook bench facts.
- `docs/histories/2026-06/20260604-1553-hook-advisory-fanout.md` — this slice
  record.

### Validation

- Commands run:
  - `xmake build test-hook` — passed.
  - `xmake run test-hook` — passed: 33 cases / 231 assertions.
  - `xmake build bench-hook` — passed.
  - `xmake run bench-hook` — passed.
- Tests added/changed:
  - Added `publish_advisory fans out async sinks while preserving outcome order`.
- Bench impact:
  - `publish_no_sinks` ~325 ns.
  - `publish_one_sink` ~1.63 µs.
  - `publish_three_sinks` ~4.07 µs.
  - `publish_blocking_no_sinks` ~348 ns.
  - `publish_blocking_one_sink` ~2.88 µs.
  - `publish_blocking_three_sinks_all_proceed` ~7.66 µs.
  - `publish_blocking_short_circuit_second` ~5.32 µs.
- Compile-budget delta:
  - Not measured; slice touches one focused hook TU and does not change the
    compile-budget gate.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: closed the 2026-05-21 deep-review P2 parallel advisory
  fan-out item.
- Linked release note:
  [`docs/releases/feature-release-notes.md`](../../releases/feature-release-notes.md).
