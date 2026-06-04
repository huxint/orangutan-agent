## [2026-05-24 10:50] | Task: agent loop tool dispatch

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local repository checkout`
- Linked plan: none — this is a focused spec-0017 continuation under the fake-provider-first sequencing contract.

### User Query

> Continue the project implementation after reading the current docs and state; keep one coherent version per commit, detailed code comments, and a standard detailed commit message.

### Changes Overview

- Areas: `oran-agent`, direct tool dispatch loop, spec/status/history docs.
- Key actions: extended `agent::Loop` from the text-only slice into the first sequential tool-use path; bumped the binary slice tag to `2.0.0-slice76`.

### Design Intent

Slice 75 deliberately stopped at the first provider text turn. This slice moves the fake-provider loop one step closer to spec 0017 without inventing the future scheduler: callers can now pass the existing `tool::Registry` and `tool::DispatchContext`, so every tool invocation still crosses the permission/audit/hook boundary already owned by `oran-tool`. The loop preserves provider-facing determinism by dispatching tool-use blocks sequentially in original order, appending a single `Role::tool` message containing ordered `ToolResultContent` blocks, rebuilding the prompt from the updated transcript, and re-entering the provider. Model-repairable tool errors become error tool results; cancellation, storage, and internal dispatch failures propagate rather than being hidden in the transcript.

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
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 76, pointed at this history, refreshed `test-agent` counts, and named the next audit/cancellation/approval-observability loop work.
- `docs/ARCHITECTURE.md` — documented the sequential `agent::Loop` tool-dispatch surface and downstream scheduler/audit boundaries.
- `docs/BUILD_SYSTEM.md` — refreshed the `oran-agent` note from text-only loop to sequential direct-dispatch loop.
- `docs/design-docs/agent-platform.md` — updated the prompt/loop status and provider-reentry narrative.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — marked the shipped scenario #2/#3/#4/#6 subset and the remaining audit/cancellation/retry gaps.
- `docs/QUALITY_SCORE.md` — updated `test-agent` counts and the agent-runtime row.
- `docs/releases/feature-release-notes.md` — added the user-visible slice-76 release note.

### Validation

- Commands run:
  - `xmake build test-agent`
  - `xmake run test-agent`
- Tests added/changed: `tests/agent/test_loop.cpp` adds five direct tool-loop cases; `test-agent` now reports 12 cases / 140 assertions.
- Bench impact: no new bench; loop-overhead comparison still waits for a stable scheduler/audit envelope.
- Compile-budget delta: not measured in this slice; the public header uses forward declarations for `tool::Registry` / `tool::DispatchContext`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md` row `agent-loop-tool-dispatch`.
