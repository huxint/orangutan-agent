## [2026-06-04 22:00] | Task: async runtime run-state clarification

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `CLI coding session`
- Linked plan: none; small tracked tech-debt cleanup slice.

### User Query

> Continue the project after re-orienting through the docs, use codegraph, start the
> next slice, keep docs in sync, verify, and commit.

### Changes Overview

- Areas: `oran-async`, async design docs, tracker/status/history.
- Key actions: replaced the private `Runtime::Impl` dual-boolean lifecycle with
  a single `idle/running/stopped` state, made post-stop `Runtime::run()` reuse
  fail with a clear `ErrorKind::conflict`, and wrapped each `io_context.run()`
  worker so escaping handler exceptions stop the runtime and return the first
  failure as `ErrorKind::internal`.

### Design Intent

The deep-review tracker called out `Runtime::Impl::run()` as unclear. The public
API stays one-shot and bootstrap-owned, matching `docs/design-docs/async-model.md`,
but the private implementation now names the lifecycle directly instead of relying
on `running` staying true forever after the first run. Handler exceptions are also
contained at the runtime boundary, which preserves the repository's `Result<T>`
error model instead of letting an executor worker unwind out of sight.

### Files Modified

- `src/oran-async/runtime.cpp`
- `tests/async/test_async.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/design-docs/async-model.md`
- `docs/exec-plans/tech-debt-tracker.md`
- `docs/QUALITY_SCORE.md`
- `docs/STATUS.md`
- `docs/histories/2026-06/20260604-2200-async-runtime-run-state.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/async-model.md` — documents one-shot runtime lifecycle and
  worker exception containment.
- `docs/exec-plans/tech-debt-tracker.md` — marks the P3 `Runtime::Impl::run()`
  clarification item closed.
- `docs/QUALITY_SCORE.md` — updates async test counts and area summary.
- `docs/STATUS.md` — bumps slice 159, points to this history, records focused
  validation, and refreshes open tracker summary.

### Validation

- Commands run:
  - `clang-format -i src/oran-async/runtime.cpp tests/async/test_async.cpp`
  - `xmake build test-async && xmake run test-async`
  - `make ci`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `xmake test`
- Tests added/changed:
  - `test-async` now covers post-stop `Runtime::run()` conflict semantics.
  - `test-async` now covers executor handler exception translation to
    `ErrorKind::internal`.
- Bench impact: none; lifecycle/error-path clarification, no hot-path benchmark
  tradeoff.
- Compile-budget delta: not measured separately; `xmake test` rebuilt and ran the
  full test suite, and the C++ implementation touch is limited to one small
  `oran-async` TU.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none added; this closes the tracked P3 runtime item.
- Linked release note: none; internal runtime clarification.
