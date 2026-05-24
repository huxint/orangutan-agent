## [2026-05-25 05:57] | Task: Agent loop approval clock bridge

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none — this is a focused spec-0015 / spec-0017 follow-up after the direct ask bridge and CLI prompt sink slices.

### User Query

> Continue iterating the project after reading the documentation, keep one coherent version per commit, keep docs in sync, and do not implement before understanding the requirements.

### Changes Overview

- Areas: `oran-agent`, `oran-tool` approval integration, bootstrap slice tag, docs.
- Key actions: made `agent::Loop` refresh `DispatchContext::now` from `core::time::now_utc()` around every direct tool dispatch, preserve the caller's previous `parent_turn_id` and `now` values afterward, added broker-backed fake-provider approval coverage in `test-agent`, and bumped the binary slice tag to `2.0.0-slice96`.

### Design Intent

Slices 94 and 95 made `permission_ask_rendered` usable by direct dispatch and terminal sinks, but the fake-provider loop still passed through whatever wall-clock value the caller left in the reusable `DispatchContext`. That was acceptable for fixed-time unit tests and wrong for the upcoming binary handoff: prompt `requested_at`, broker expiry, and immediate broker verification should describe the actual tool call, not a stale value such as the default epoch. The loop already scoped `parent_turn_id` per dispatch for trace joins, so this slice broadens that scoped context update to include `now` without moving approval rendering into `oran-agent`.

### Files Modified

- `include/oran/agent/loop.hpp`
- `src/oran-agent/loop.cpp`
- `tests/agent/test_loop.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR

- `docs/STATUS.md` — moved the project snapshot to slice 96 and recorded the focused `test-agent` result.
- `docs/ARCHITECTURE.md` — updated the `oran-agent` inventory to describe the per-dispatch approval clock refresh.
- `docs/QUALITY_SCORE.md` — refreshed test counts and area summaries for agent/runtime approval coverage.
- `docs/design-docs/agent-platform.md` — documented the loop-owned per-dispatch clock refresh.
- `docs/design-docs/permissions-and-hooks.md` — marked the fake-provider loop as the first approval-clock consumer.
- `docs/product-specs/0015-blocking-hook-decisions.md` — updated the approval round-trip status with loop coverage.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — updated the loop status and remaining downstream work.
- `docs/releases/feature-release-notes.md` — added the slice 96 release note.

### Validation

- Commands run:
  - `xmake run test-agent`
  - `xmake run test-bootstrap`
  - `xmake run orangutan -- --help`
  - `make ci`
  - `git diff --check`
- Tests added/changed:
  - Added a fake-provider loop test that drives a `file.read` ask through `permission::ApprovalBroker` plus a blocking `hook::InProcessSink`, verifies the prompt payload's identity/replay/TTL/request time, confirms audit `permission_ask_decisions` metadata, returns the approved tool result to the provider, restores the caller's stale `ctx.now`, and checks the issued token against the prompt time.
- Bench impact:
  - None. The slice adds one wall-clock read per direct tool dispatch; this is not a hot-path tradeoff requiring a new A/B bench.
- Compile-budget delta:
  - Minimal. `oran-agent` adds one include of `<oran/core/time.hpp>` in the `.cpp`; no public heavy include was added.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md` row `agent-loop-approval-clock`.
