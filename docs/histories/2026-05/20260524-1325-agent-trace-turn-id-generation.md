## [2026-05-24 13:25] | Task: Agent trace turn-id generation

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local repository checkout`
- Linked plan: none — focused continuation of specs 0017/0018.

### User Query

> Continue iterating the Orangutan v2 implementation with one version per
> commit, after reading the docs and keeping docs synchronized with behavior.

### Changes Overview

- Areas: `oran-agent`, spec-0018 cause-chain ids, docs/history/version.
- Key actions:
  - Added loop-owned generation of a non-zero 16-byte turn id when an enabled
    trace writer is configured and `RunTurnInputs::turn_id` is absent.
  - Reused the generated id for both `trace_turns.turn_id` and direct-dispatch
    `audit_events.parent_turn_id`.
  - Preserved the existing trace-disabled and repository-less behavior:
    direct-dispatch audit rows keep `parent_turn_id = NULL` unless the caller
    explicitly supplies a turn id.
  - Added storage-backed `test-agent` coverage for a generated-id tool turn
    with matching trace and audit rows.

### Design Intent

Spec 0018 says the loop owns a turn id for trace/audit correlation. Earlier
slices required tests and future bootstrap callers to provide one manually,
which made trace rows durable but left a caller footgun. This slice keeps the
generation private to `src/oran-agent/loop.cpp`: `core::TurnId` remains only
the shared value shape, public headers describe the behavior, and no random
implementation details leak into downstream libraries. Generation happens
before the first prompt render so every tool dispatch in the turn sees the
same id.

### Files Modified

- `include/oran/agent/loop.hpp`
- `src/oran-agent/loop.cpp`
- `tests/agent/test_loop.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 85, pointed at this history, and recorded
  generated trace turn ids plus remaining trace gaps.
- `docs/ARCHITECTURE.md` — documented generated ids in the trace/agent
  inventory and dependency narrative.
- `docs/BUILD_SYSTEM.md` — refreshed the `oran-agent` build-surface note.
- `docs/design-docs/agent-platform.md` — updated the prompt/loop status block.
- `docs/design-docs/storage-runtime.md` — updated the trace repository consumer
  status to note generated ids.
- `docs/design-docs/tool-runtime.md` — updated direct-dispatch audit correlation
  wording for generated loop ids.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — recorded the
  generated-id behavior in loop and per-turn audit status.
- `docs/product-specs/0018-first-loop-observability.md` — marked turn-id
  generation and cause-chain join coverage as shipped at the loop boundary.
- `docs/QUALITY_SCORE.md` — refreshed `test-agent` counts and remaining agent
  runtime gaps.
- `docs/releases/feature-release-notes.md` — added the slice 85 release note.

### Validation

- Commands run:
  - `xmake run test-agent`
  - `xmake build orangutan`
  - `xmake run orangutan`
  - `make ci`
  - `git diff --check`
- Tests added/changed:
  - `Loop generates trace turn ids and correlates storage audit rows`
- Bench impact: none; turn-id generation runs once per traced turn and is not
  on an inner dispatch loop.
- Compile-budget delta: not measured; implementation is isolated to one `.cpp`
  plus public comments.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: config-to-loop wiring, hook publish rows, iteration-cap
  trace rows, CLI `--trace`, binary handoff.
- Linked release note:
  [`docs/releases/feature-release-notes.md`](../../releases/feature-release-notes.md)
