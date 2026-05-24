## [2026-05-24 13:40] | Task: Agent iteration-cap trace rows

### Execution Context

- Agent: `Claude`
- Base model: `Opus 4.7`
- Runtime: `orangutan-refactor` local workspace
- Linked plan: none — focused continuation of specs 0017/0018.

### User Query

> Continue deeply understanding the project architecture and implementation
> progress, then keep advancing the project code implementation with one
> version per commit and docs in sync.

### Changes Overview

- Areas: `oran-agent`, spec-0018 iteration-cap trace rows, docs/history/version.
- Key actions:
  - Tracked the last successful `prompt::RenderedPrompt` and provider
    response model across loop iterations.
  - Added a body-free `trace_turns` write with `stop_reason=error` when
    `LoopOptions::max_iterations` is exhausted by repeated tool_use turns
    and a trace context is configured. The existing `Error::internal`
    (`reason=iteration_cap`) is still returned unchanged afterwards.
  - Added storage-backed `test-agent` coverage for the iteration-cap trace
    row with aggregated provider usage and the final response model.

### Design Intent

Spec 0018 says every terminal turn produces exactly one durable trace row;
slices 80-84 covered terminal-success, cancellation, and ordinary error
exits, but the iteration-cap exit dropped the loop without a row. This
slice closes that last loop-owned writer hole by capturing the most recent
rendered prompt and response model during the iteration loop, then reusing
the existing error trace writer once the cap fires. The change keeps the
returned error shape unchanged, so callers still see the previously-stable
`reason=iteration_cap` / `max_iterations` context entries, and the trace
row is only written when a trace writer is configured.

### Files Modified

- `include/oran/agent/loop.hpp`
- `src/oran-agent/loop.cpp`
- `tests/agent/test_loop.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 86, pointed at this history, and
  recorded iteration-cap trace rows plus the remaining trace gaps.
- `docs/ARCHITECTURE.md` — documented iteration-cap trace rows in the
  oran-agent inventory and dependency narrative.
- `docs/product-specs/0018-first-loop-observability.md` — updated the
  turn-finished publisher status to note iteration-cap rows.
- `docs/QUALITY_SCORE.md` — refreshed `test-agent` counts.
- `docs/releases/feature-release-notes.md` — added the slice 86 release
  note.

### Validation

- Commands run:
  - `xmake run test-agent`
  - `xmake build orangutan`
  - `xmake run orangutan`
  - `make ci`
- Tests added/changed:
  - `Loop persists iteration-cap trace rows`
- Bench impact: none; this is one extra SQLite insert on already
  iteration-capped turns, not a new hot path.
- Compile-budget delta: not measured; no new public templates and the
  state added is two locals inside a single `.cpp` function.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: trace config-to-loop wiring, hook publish rows,
  CLI `--trace`, binary handoff.
- Linked release note:
  [`docs/releases/feature-release-notes.md`](../../releases/feature-release-notes.md)
