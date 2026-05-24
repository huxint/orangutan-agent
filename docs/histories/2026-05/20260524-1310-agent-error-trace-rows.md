## [2026-05-24 13:10] | Task: Agent error trace rows

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `orangutan-refactor` local workspace
- Linked plan: none — focused continuation of specs 0017/0018.

### User Query

> Continue deeply understanding the project architecture and implementation
> progress, then keep implementing the next project slice with one version per
> commit and docs in sync.

### Changes Overview

- Areas: `oran-agent`, spec-0018 error trace rows, docs/history/version.
- Key actions:
  - Added `agent::Loop` trace writes for non-cancelled provider failures.
  - Added trace writes for response-backed loop-boundary errors: missing
    dispatch services, `tool_use` stop reasons without tool blocks,
    unsupported non-terminal stop reasons, and non-cancelled
    storage/internal direct-dispatch failures.
  - Preserved slice-83 cancellation behavior by avoiding extra trace awaits
    on active parent-cancellation paths unless the cancellation trace writer
    first resets coroutine cancellation state.
  - Added storage-backed `test-agent` coverage for provider-error and
    loop-boundary-error trace rows.

### Design Intent

Spec 0018 needs the first loop to leave a durable row even when a turn fails.
This slice keeps the implementation narrow: failures that already have a
rendered prompt and either a provider route or provider response can reuse the
body-free `trace_turns` writer with `stop_reason=error`. Provider errors still
return unchanged so retry/fallback policy remains a later execution-layer
concern. Cancellation remains a distinct `stop_reason=cancelled` path because
the coroutine is already under parent cancellation and must not casually
`co_await` additional work.

### Files Modified

- `src/oran-agent/loop.cpp`
- `tests/agent/test_loop.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 84, pointed at this history, and recorded
  provider/loop-boundary error rows plus remaining trace gaps.
- `docs/ARCHITECTURE.md` — documented error trace rows in the storage/agent
  inventory and dependency narrative.
- `docs/BUILD_SYSTEM.md` — refreshed the `oran-agent` build-surface note.
- `docs/design-docs/agent-platform.md` — updated the prompt/loop status block.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — recorded
  provider-error trace behavior for scenarios #7/#8.
- `docs/product-specs/0018-first-loop-observability.md` — updated trace writer
  status for ordinary provider and loop-boundary failures.
- `docs/QUALITY_SCORE.md` — refreshed `test-agent` counts and remaining agent
  runtime gaps.
- `docs/releases/feature-release-notes.md` — added the slice 84 release note.

### Validation

- Commands run:
  - `xmake run test-agent`
  - `xmake build orangutan`
  - `xmake run orangutan`
  - `make ci`
  - `git diff --check`
- Tests added/changed:
  - `Loop persists provider error trace rows`
  - `Loop persists loop-boundary error trace rows`
- Bench impact: none; this is one extra SQLite insert on already failing
  traced turns, not a new hot-path alternative.
- Compile-budget delta: not measured; no new public templates or headers.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: iteration-cap trace rows, hook publish rows, trace
  config-to-loop wiring, CLI `--trace`, binary handoff.
- Linked release note:
  [`docs/releases/feature-release-notes.md`](../../releases/feature-release-notes.md)
