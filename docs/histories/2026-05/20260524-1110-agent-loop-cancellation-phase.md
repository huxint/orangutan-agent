## [2026-05-24 11:10] | Task: agent loop cancellation phase

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `/home/huxint/projects/orangutan-refactor`
- Linked plan: none — this is a focused spec-0017/spec-0018 continuation under the fake-provider-first loop sequencing contract.

### User Query

> Continue the project implementation after reading the current docs and state; keep one coherent version per commit, detailed code comments, and a standard detailed commit message.

### Changes Overview

- Areas: `oran-agent`, cancellation observability, spec/status/history docs.
- Key actions: tagged parent-cancelled provider awaits and direct tool dispatches with stable loop-level `cancellation_phase` context; bumped the binary slice tag to `2.0.0-slice77`.

### Design Intent

Slice 76 proved the loop can run sequential tool turns through the existing registry boundary. The next observability step is not the full trace schema yet; it is making the loop classify the two cancellation phases that specs 0017 and 0018 already require. This slice keeps the error model simple: only `ErrorKind::cancelled` values from `provider::System::send` and `tool::Registry::dispatch` are decorated with `reason=parent_cancelled` and `cancellation_phase=provider|tools`. Ordinary provider failures, retryable network/upstream failures, storage/internal dispatch failures, and model-repairable tool errors keep their prior behavior.

### Files Modified

- `include/oran/agent/loop.hpp`
- `src/oran-agent/loop.cpp`
- `tests/agent/test_loop.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/BUILD_SYSTEM.md`
- `docs/design-docs/agent-platform.md`
- `docs/product-specs/0017-fake-provider-first-agent-loop.md`
- `docs/product-specs/0018-first-loop-observability.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 77, pointed at this history, refreshed `test-agent` counts, and kept trace/audit rows as downstream.
- `docs/ARCHITECTURE.md` and `docs/BUILD_SYSTEM.md` — documented the loop's cancellation-phase context and the unchanged binary handoff boundary.
- `docs/design-docs/agent-platform.md` — updated the prompt/loop status and cancellation-observability narrative.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — marked scenarios #9/#10 as returning phase-tagged cancellation errors while audit/trace rows remain future work.
- `docs/product-specs/0018-first-loop-observability.md` — documented the pre-trace source for `trace_turns.cancellation_phase`.
- `docs/QUALITY_SCORE.md` — refreshed `test-agent` counts and the agent-runtime row.
- `docs/releases/feature-release-notes.md` — added the user-visible slice-77 release note.

### Validation

- Commands run:
  - `xmake build test-agent`
  - `xmake run test-agent`
- Tests added/changed: `tests/agent/test_loop.cpp` adds two parent-cancellation cases; `test-agent` now reports 14 cases / 154 assertions.
- Bench impact: no new bench; this is a classification-only branch on already-returned errors.
- Compile-budget delta: not measured in this slice; the public API is unchanged aside from documentation comments.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md` row `agent-loop-cancellation-phase`.
