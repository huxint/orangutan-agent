## [2026-06-14 18:27] | Task: Provider cancellation phases

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none - small Observability v1.1 follow-up from `docs/STATUS.md`.

### User Query

> Continue iterating the project slice by slice, first understand the docs and
> real progress, choose the most valuable current slice, keep docs in sync, and
> commit with a detailed conventional message.

### Changes Overview

- Areas: `oran-agent`, trace observability, bootstrap version metadata.
- Key actions:
  - Added an internal provider phase sink around `agent::Loop` provider awaits.
  - Split provider cancellation context from the old broad provider phase into
    `provider_initial`, `provider_stream`, and `provider_complete`.
  - Persisted the same refined phase to `trace_turns.cancellation_phase` for
    trace-enabled parent-cancelled provider turns.
  - Added focused agent-loop coverage for all three provider phases.

### Design Intent

The previous `provider` cancellation phase could prove that cancellation landed
inside the provider await, but not whether the user stopped before any upstream
output, during visible streaming, or after the provider had delivered its
terminal stream marker. This slice keeps the existing trace schema and provider
contract intact: the loop observes callbacks through a borrowed sink wrapper and
records only the phase label. Calls without a caller-supplied sink keep the old
no-sink provider behavior so observability does not change protocol selection.

### Files Modified

- `include/oran/agent/loop.hpp`
- `src/oran-agent/loop.cpp`
- `tests/agent/test_loop.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 244 snapshot, history pointer, latest result, and
  agent test count.
- `docs/ROADMAP.md` - Observability frontier and non-prescriptive next step.
- `docs/ARCHITECTURE.md` - agent-loop cancellation phase summary.
- `docs/QUALITY_SCORE.md` - agent runtime summary and focused test count.
- `docs/design-docs/agent-platform.md` - agent-loop cancellation phase contract.
- `docs/design-docs/api-portability.md` - streaming Ctrl-C phase reference.
- `docs/product-specs/0007-web-ui.md` - future stop-control expectation.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` - fake-provider
  loop cancellation status.
- `docs/product-specs/0018-first-loop-observability.md` - trace phase values,
  v1.1 status, and acceptance criterion.
- `docs/releases/feature-release-notes.md` - operator-facing slice 244 note.

### Validation

- Commands run:
  - `xmake build test-agent && xmake run test-agent`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed:
  - `test-agent` now covers `provider_initial`, `provider_stream`, and
    `provider_complete` cancellation phases with trace rows.
- Bench impact:
  - None; this is trace/error-context classification over existing callbacks.
- Compile-budget delta:
  - Not measured separately; the change adds no public heavy include and only a
    small private sink wrapper in `src/oran-agent/loop.cpp`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  [`docs/releases/feature-release-notes.md`](../../releases/feature-release-notes.md)
