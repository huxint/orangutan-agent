## [2026-05-24 12:55] | Task: Agent cancellation trace rows

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `none`

### User Query

> Continue implementing the Orangutan v2 rewrite, reading the docs first, keeping
> one coherent version per commit with standard detailed commit messages.

### Changes Overview

- Areas: `oran-agent`, spec-0018 cancellation trace rows, docs/history/version.
- Key actions: taught `agent::Loop` to append cancelled `trace_turns` rows for
  parent cancellation during the provider await or direct tool dispatch; added
  provider/tool storage-backed cancellation trace tests; bumped the binary slice
  tag to `2.0.0-slice83`.

### Design Intent

Slice 77 already classified parent cancellation as `cancellation_phase=provider`
or `tools`, and slice 80 added the terminal-success trace writer. This slice
connects those two pieces without broadening into ordinary error rows: only
`ErrorKind::cancelled` with the loop-owned phase writes a cancelled trace row.
Because storage acquisition deliberately observes the current coroutine's
cancellation state, the loop briefly resets cancellation only around the trace
insert; the user-visible result still returns the original cancelled error with
`reason=parent_cancelled`.

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
- `docs/histories/2026-05/20260524-1255-agent-cancellation-trace-rows.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 83, pointed at this history, and recorded
  provider/tool cancellation trace rows plus remaining ordinary-error work.
- `docs/ARCHITECTURE.md` — documented cancellation trace rows in the inventory
  and `oran-agent` / storage rows.
- `docs/BUILD_SYSTEM.md` — refreshed the `oran-agent` dependency note with the
  disabled trace gate and cancellation rows.
- `docs/design-docs/agent-platform.md` — refreshed the loop status with
  persisted provider/tool cancellation phases.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — updated the
  cancellation scenario statuses and per-turn audit envelope.
- `docs/product-specs/0018-first-loop-observability.md` — marked AC4 as shipped
  for trace-enabled parent-cancelled provider/tool phases.
- `docs/QUALITY_SCORE.md` — refreshed `test-agent` counts and current
  agent/storage notes.
- `docs/releases/feature-release-notes.md` — added the slice 83 release note.

### Validation

- Commands run:
  - `xmake run test-agent`
  - `xmake build orangutan`
  - `xmake run orangutan`
  - `make ci`
  - `git diff --check`
- Tests added/changed: `tests/agent/test_loop.cpp` adds provider-await and
  tool-dispatch cancellation trace-row cases. `test-agent` reports 19 cases /
  290 assertions.
- Bench impact: no new benchmark; this reuses the existing trace insert path and
  adds no new hot-path data structure.
- Compile-budget delta: no new target or dependency edge.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: ordinary provider/internal error trace rows, hook publish
  rows, config-to-loop wiring, CLI `--trace`, and binary handoff remain
  downstream.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
