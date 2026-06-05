## [2026-06-05 09:31] | Task: Long-term runtime recall composition

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local shell + xmake/GCC 16.1
- Linked plan: none; small follow-up from the deep-review memory tracker.

### User Query

> Continue iterating after the FTS5 backend commit, keep docs current, and use
> `std::format_to` instead of string concatenation for prompt text rendering.

### Changes Overview

- Areas: `oran-memory`, long-term memory docs/status/history.
- Key actions:
  - Added `memory::longterm::Runtime`, `RecallRequest`, and `RecallResult`.
  - Added `render_recall_framing(...)`, which turns recall hits into stable
    section-5 `memory::Framing` bytes.
  - Kept the slice library-local: no bootstrap/config recall source, no memory
    tools, no vector dependency, and no hybrid ranking yet.
  - Rendered prompt framing with `std::format_to(std::back_inserter(...), ...)`
    rather than ad hoc string concatenation.
  - Added tests for runtime validation-before-dispatch, backend delegation,
    deterministic recall framing, empty recall framing, and composition through
    the real `Fts5Backend`.

### Design Intent

Slice 161 made FTS5 searchable, but callers still had to talk directly to the
backend and hand-roll prompt bytes. This slice introduces the smallest runtime
composition layer that future bootstrap/config wiring can depend on: validate a
query, delegate to a backend, and render deterministic memory framing once before
the agent loop. The framing intentionally omits scores, clocks, trace ids, and
request ids so it obeys the prompt cache rules for sections 1-6.

### Files Modified

- `include/oran/memory/longterm.hpp`
- `include/oran/memory.hpp`
- `src/oran-memory/longterm_runtime.cpp`
- `tests/memory/test_longterm.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/memory-system.md` — documents shipped `longterm::Runtime`,
  `RecallRequest` / `RecallResult`, and deterministic recall framing.
- `docs/product-specs/0005-memory-system.md` — updates v1 scope and acceptance
  status for runtime search/framing.
- `docs/ARCHITECTURE.md` — records the new `oran-memory` runtime composition
  surface and narrows the remaining memory work.
- `docs/QUALITY_SCORE.md` and `docs/STATUS.md` — update slice number, latest
  history, test count, and remaining follow-ups.
- `docs/exec-plans/tech-debt-tracker.md` — closes the library-local runtime
  recall composition half and leaves bootstrap/config wiring, memory tools, and
  vector/hybrid work open.

### Validation

- Commands run:
  - `xmake build test-memory`
  - `xmake run test-memory`
  - `git diff --check`
  - `make ci`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `xmake test`
- Tests added/changed:
  - `tests/memory/test_longterm.cpp` adds runtime validation/delegation,
    deterministic framing, empty recall, and FTS5-backed recall coverage.
- Bench impact:
  - No new bench; this slice is correctness/composition only. The 10k-record
    runtime search benchmark remains the acceptance item for a later memory
    performance slice.
- Compile-budget delta:
  - Not measured in this slice; the new runtime implementation is a separate
    `oran-memory` translation unit and keeps the public surface to small value
    types plus the runtime wrapper.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: existing deep-review tracker row now leaves bootstrap/config
  recall wiring, memory tools, gated sqlite-vec, and hybrid ranking as remaining
  memory work.
- Linked release note: none; internal runtime infrastructure.
