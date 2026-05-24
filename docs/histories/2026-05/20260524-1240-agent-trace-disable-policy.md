## [2026-05-24 12:40] | Task: Agent trace disabled policy

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `none`

### User Query

> Continue implementing the Orangutan v2 rewrite, reading the docs first, keeping
> one coherent version per commit with standard detailed commit messages.

### Changes Overview

- Areas: `oran-agent`, spec-0018 trace/audit observability, docs/history/version.
- Key actions: added an explicit `agent::TraceContext::enabled` switch; taught
  `agent::Loop` to skip terminal-success trace writes when it is false; gated
  direct-dispatch audit parent-id stamping through the same switch; added a
  storage-backed loop test for the trace-disabled path; bumped the binary slice
  tag to `2.0.0-slice82`.

### Design Intent

Spec 0018 AC9 requires trace-disabled turns to produce no `trace_turns` rows and
leave tool audit rows in the trace-off shape (`parent_turn_id = NULL`). Slice 81
landed the typed config surface, but bootstrap does not yet map that config into
the agent loop. This slice deliberately closes the lower-level loop contract
first: callers can pass `RunTurnInputs::trace.enabled=false` today, and future
bootstrap wiring can consume the same switch without changing direct-dispatch
audit semantics. The default remains `true` so existing trace-enabled tests and
pre-trace callers that supply only a turn id keep their current behavior.

### Files Modified

- `include/oran/agent/loop.hpp`
- `src/oran-agent/loop.cpp`
- `tests/agent/test_loop.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/design-docs/agent-platform.md`
- `docs/product-specs/0017-fake-provider-first-agent-loop.md`
- `docs/product-specs/0018-first-loop-observability.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`
- `docs/histories/2026-05/20260524-1240-agent-trace-disable-policy.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 82, pointed at this history, and recorded
  the explicit disabled-policy behavior plus remaining config-to-loop work.
- `docs/ARCHITECTURE.md` — documented `TraceContext::enabled` as the agent-loop
  gate for trace rows and direct-dispatch audit parent ids.
- `docs/design-docs/agent-platform.md` — refreshed the prompt/loop status with
  trace-disabled context restoration semantics.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — updated the loop
  status and per-turn audit envelope notes for disabled trace contexts.
- `docs/product-specs/0018-first-loop-observability.md` — marked AC9 loop-boundary
  behavior as shipped while keeping bootstrap config mapping downstream.
- `docs/QUALITY_SCORE.md` — refreshed `test-agent` counts and current agent/storage
  notes.
- `docs/releases/feature-release-notes.md` — added the slice 82 release note.

### Validation

- Commands run:
  - `xmake run test-agent`
  - `xmake build orangutan`
  - `xmake run orangutan`
  - `make ci`
  - `git diff --check`
- Tests added/changed: `tests/agent/test_loop.cpp` adds a storage-backed
  single-tool turn with `TraceContext::enabled=false`, asserting zero trace rows,
  NULL audit `parent_turn_id`, and reusable dispatch-context restoration.
  `test-agent` reports 17 cases / 247 assertions.
- Bench impact: no new benchmark; this gates existing optional trace/audit work and
  does not introduce a new hot algorithm.
- Compile-budget delta: no new target or dependency edge.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: bootstrap still needs to map `config::TraceConfig::enabled`
  into `RunTurnInputs::trace.enabled`; cancellation/error trace rows, hook rows,
  CLI `--trace`, and binary handoff remain downstream.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
